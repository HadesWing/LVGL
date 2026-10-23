/**
 * @file app_video.c
 * @brief 视频通路：RTP → 解包 → 解码 → 落盘 / 上屏
 *
 * 落盘为什么用裸 YUV420P 而不是 mp4/avi：
 *   1) 判据要"无损"。编码成视频文件又是一次有损压缩，比出来的差异分不清是
 *      解码错还是二次编码造成的 —— 那样验收就失去意义了。
 *   2) 裸 YUV 是解码器的**直接输出**，一字节不差地和参考 YUV 比对，
 *      不同就一定是解码链路有问题，没有第二种解释。
 *   3) 想肉眼看的话 ffplay 一行就能播：
 *      ffplay -f rawvideo -pixel_format yuv420p -video_size 640x480 video_rx.yuv
 *
 * 上屏为什么自己转 RGB565 而不是把 YUV 丢给 LVGL：
 *   采样目标尺寸（288x216）比源（640x480）小一半多，转换时顺手缩小，
 *   一步到位；把整幅 YUV 丢进显存再让 LVGL 缩，浪费 4 倍带宽。
 */
#include "app_video.h"
#include "app_events.h"
#include "app_lock.h"
#include "h264_depack.h"
#include "vdec.h"
#include "yuv2rgb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { VSINK_DROP = 0, VSINK_FILE, VSINK_DISP } vsink_t;

/*---------------------------------------------------------------------------
 * 上屏槽位
 *
 * 状态机（唯一的生产者 = RTP 线程，唯一的消费者 = LVGL 主线程）：
 *
 *      FREE ──解码线程写入──> READY ──主线程取走──> SHOWN
 *        ^                                          │
 *        └──────── 主线程显示下一帧时归还 ────────────┘
 *
 * 不变式：解码线程只写 FREE 槽，主线程只把 SHOWN 槽改回 FREE。
 * 两边都只动"归自己管"的槽，所以不会同时写同一块内存。
 * 即便如此仍然上锁 —— 需要的是一个内存屏障：解码线程写像素在前、
 * 置 READY 在后，没有屏障的话 ARM 上主线程可能先看到 READY 再看到旧像素
 * （表现是偶发花屏，且只在板子上出现，PC 上永远复现不了）。
 *---------------------------------------------------------------------------*/
#define VSLOT_MAX 3

enum { VSLOT_FREE = 0, VSLOT_READY, VSLOT_SHOWN };

typedef struct {
    uint8_t *buf;      /* RGB565 像素，容量 cap 字节 */
    size_t   cap;
    int      w, h;     /* 有效尺寸（每次转换可能变：源分辨率变了） */
    int      state;
} vslot_t;

static struct {
    bool             opened;
    vsink_t          sink;
    bool             nodec;        /* 只解包不解码 */
    char             sink_path[256];

    h264_depack_t   *dep;
    vdec_t          *dec;
    FILE            *fp;

    uint32_t         frames;       /* 已送进 sink 的帧数（落盘或上屏成功） */
    uint32_t         au_in;        /* 收到的访问单元数 */
    uint32_t         w, h;
    uint64_t         bytes;
    bool             logged_size;  /* 尺寸只打一次，避免 30fps 刷屏 */

    /* --- 上屏 --- */
    int              box_w, box_h;
    vslot_t          slot[VSLOT_MAX];
    int              shown;        /* 当前正在显示的槽，-1 = 无 */
    uint32_t         disp_frames;  /* 成功交给 LVGL 的帧数 */
    uint32_t         replace;      /* 槽位占满时被更新帧顶掉的旧帧数 */
    uint32_t         drop_busy;    /* 连可顶掉的都没有而丢弃的帧数（正常应为 0） */
    uint32_t         dec_total;    /* 解码器吐出的总帧数（1 基），对齐参考帧用 */
    app_lock_t       lock;
    bool             lock_ready;
} s_v;

static void slot_lock(void)
{
    if (!s_v.lock_ready) {
        app_lock_init(&s_v.lock);
        s_v.lock_ready = true;
    }
    app_lock_take(&s_v.lock);
}
static void slot_unlock(void) { app_lock_give(&s_v.lock); }

/*---------------------------------------------------------------------------
 * 路由解析
 *---------------------------------------------------------------------------*/
static void route_parse_env(void)
{
    s_v.sink = VSINK_DROP;
    const char *k = getenv("SIP_VIDEO_SINK");
    if (k && *k) {
        if      (!strcmp(k, "file")) s_v.sink = VSINK_FILE;
        else if (!strcmp(k, "disp")) s_v.sink = VSINK_DISP;
        else if (!strcmp(k, "drop")) s_v.sink = VSINK_DROP;
        else {
            printf("[video] SIP_VIDEO_SINK=%s 不认识，按 drop 处理\n", k);
            s_v.sink = VSINK_DROP;
        }
    }

    const char *f = getenv("SIP_VIDEO_SINK_FILE");
    snprintf(s_v.sink_path, sizeof(s_v.sink_path), "%s",
             (f && *f) ? f : "video_rx.yuv");

    s_v.nodec = getenv("SIP_VIDEO_NODEC") != NULL;
}

const char *app_video_route_desc(void)
{
    static char buf[192];
    const char *names[] = { "drop", "file", "disp" };
    if (!s_v.opened) return "closed";
    snprintf(buf, sizeof(buf), "vsink=%s", names[s_v.sink]);
    size_t n = strlen(buf);
    if (s_v.sink == VSINK_FILE)
        snprintf(buf + n, sizeof(buf) - n, "(%s)", s_v.sink_path);
    else if (s_v.sink == VSINK_DISP)
        snprintf(buf + n, sizeof(buf) - n, "(%dx%d)", s_v.box_w, s_v.box_h);
    n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n, " dec=%s/%s",
             s_v.nodec ? "off" : "on", vdec_backend_name());
    return buf;
}

/*---------------------------------------------------------------------------
 * 上屏槽位管理
 *---------------------------------------------------------------------------*/
void app_video_set_box(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (s_v.box_w == w && s_v.box_h == h) return;
    s_v.box_w = w;
    s_v.box_h = h;
    printf("[video] 上屏目标框 %dx%d\n", w, h);
}

bool app_video_disp_acquire(int slot, const uint8_t **rgb565, int *w, int *h)
{
    if (slot < 0 || slot >= VSLOT_MAX) return false;

    bool ok = false;
    slot_lock();
    if (s_v.slot[slot].state == VSLOT_READY) {
        /* 上一帧的槽还给它：从这一刻起解码线程可以覆盖它了。
           为什么现在才还 —— 因为 LVGL 可能还在用它画，早还就可能撕裂。 */
        if (s_v.shown >= 0 && s_v.shown != slot)
            s_v.slot[s_v.shown].state = VSLOT_FREE;

        s_v.slot[slot].state = VSLOT_SHOWN;
        s_v.shown = slot;
        if (rgb565) *rgb565 = s_v.slot[slot].buf;
        if (w) *w = s_v.slot[slot].w;
        if (h) *h = s_v.slot[slot].h;
        s_v.disp_frames++;
        ok = true;
    }
    slot_unlock();
    return ok;
}

uint32_t app_video_disp_frames(void) { return s_v.disp_frames; }

/*---------------------------------------------------------------------------
 * 打开/关闭
 *---------------------------------------------------------------------------*/
bool app_video_open(void)
{
    if (s_v.opened) return true;
    route_parse_env();
    slot_lock();
    slot_unlock();          /* 只为确保锁已初始化（open 之后解码线程随时可能进来） */

    if (s_v.box_w <= 0) app_video_set_box(288, 216);

    /* drop 模式什么都不用开 —— 通话只有声音，这是默认状态 */
    if (s_v.sink == VSINK_DROP && !s_v.nodec) {
        s_v.opened = true;
        return true;
    }

    s_v.dep = h264_depack_new();
    if (!s_v.dep) {
        printf("[video] 解包器分配失败（内存不够？）\n");
        return false;
    }

    if (!s_v.nodec) {
        if (!vdec_available()) {
            printf("[video] 本机没编解码器（vdec=%s），视频只解包不解码\n",
                   vdec_backend_name());
            s_v.nodec = true;
        } else {
            s_v.dec = vdec_open();
            if (!s_v.dec) {
                printf("[video] 解码器打开失败，退化为只解包\n");
                s_v.nodec = true;
            }
        }
    }

    if (s_v.sink == VSINK_FILE) {
        s_v.fp = fopen(s_v.sink_path, "wb");
        if (!s_v.fp) {
            printf("[video] 打不开落盘文件 %s（目录不存在？）\n", s_v.sink_path);
            h264_depack_free(s_v.dep);
            s_v.dep = NULL;
            return false;
        }
        printf("[video] 落盘 → %s (YUV420P 裸流，无容器)\n", s_v.sink_path);
    }

    if (s_v.sink == VSINK_DISP) {
        /* 槽位缓冲**故意不在这里释放**，见 app_video_close 的说明 */
        for (int i = 0; i < VSLOT_MAX; i++) {
            slot_lock();
            s_v.slot[i].state = VSLOT_FREE;
            slot_unlock();
        }
        s_v.shown = -1;
        printf("[video] 上屏 → lv_image %dx%d (YUV420P→RGB565 最近邻缩放)\n",
               s_v.box_w, s_v.box_h);
    }

    s_v.opened = true;
    printf("[video] open ok: %s\n", app_video_route_desc());
    return true;
}

void app_video_close(void)
{
    if (!s_v.opened) return;

    if (s_v.fp) {
        fflush(s_v.fp);
        fclose(s_v.fp);
        s_v.fp = NULL;
    }
    if (s_v.dec) {
        vdec_close(s_v.dec);
        s_v.dec = NULL;
    }
    if (s_v.dep) {
        h264_depack_free(s_v.dep);
        s_v.dep = NULL;
    }

    /* 槽位缓冲保留，只把状态复位。
     *
     * 为什么不 free()：close 是在 RTP 线程里调的，而 LVGL 主线程可能正在
     * 用槽里的像素画图（lv_image 只存指针，不拷贝）。这时候 free 就是
     * 教科书级的 use-after-free：表现为挂断瞬间偶发崩溃，且换个时序就复现不了。
     * 代价是常驻 3 个槽（288x216x2 x3 ≈ 356KB），换来的是这一段逻辑
     * 完全没有悬垂指针 —— 这笔交换在这个量级的项目里是划算的。 */
    slot_lock();
    for (int i = 0; i < VSLOT_MAX; i++) s_v.slot[i].state = VSLOT_FREE;
    s_v.shown = -1;
    slot_unlock();

    printf("[video] closed (%u 帧, %.1f MB%s)\n",
           s_v.frames, (double)s_v.bytes / (1024.0 * 1024.0),
           s_v.sink == VSINK_DISP ? "，上屏" : "");
    s_v.opened = false;
}

bool app_video_is_open(void) { return s_v.opened; }

bool app_video_wants_rx(void)
{
    /* drop 且要解码的情况不存在：sink=drop 就意味着不解码（省 CPU） */
    return s_v.opened && s_v.sink != VSINK_DROP;
}

/*---------------------------------------------------------------------------
 * 落盘：按 YUV420P 三个平面依次写
 *
 * 必须按 linesize 逐行拷 —— 解码器的 linesize 通常大于 width（为了对齐），
 * 直接 fwrite(data[0], w*h) 会把每行末尾的对齐垃圾也写进去，
 * 结果就是"图看着斜了"。这个坑几乎所有第一次写 YUV 落盘的人都踩过。
 *---------------------------------------------------------------------------*/
static void sink_write_frame(const vframe_t *f)
{
    if (!s_v.fp) return;

    for (int p = 0; p < 3; p++) {
        int pw = (p == 0) ? f->w : (f->w + 1) / 2;
        int ph = (p == 0) ? f->h : (f->h + 1) / 2;
        const uint8_t *src = f->plane[p];
        if (!src) return;
        for (int y = 0; y < ph; y++) {
            fwrite(src + (size_t)y * (size_t)f->linesize[p], 1, (size_t)pw, s_v.fp);
            s_v.bytes += (uint64_t)pw;
        }
    }
    s_v.frames++;

    if (!s_v.logged_size) {
        printf("[video] 解码输出 %dx%d YUV420P（first frame pts=%lld）\n",
               f->w, f->h, (long long)f->pts);
        s_v.logged_size = true;
    }
}

/*---------------------------------------------------------------------------
 * 上屏：缩放到目标框 → 填进空闲槽 → 发事件让主线程取
 *---------------------------------------------------------------------------*/
static void sink_disp_frame(const vframe_t *f, uint32_t frame_no)
{
    int dw = 0, dh = 0;
    if (!yuv2rgb_fit(f->w, f->h, s_v.box_w, s_v.box_h, &dw, &dh)) return;

    /* 挑一个槽：优先空闲的；没有空闲的，就顶掉一个"已备好但主线程还没取"的旧帧。
     *
     * 为什么是"丢旧留新"而不是"丢新的"：
     *   实时视频的延迟比完整性重要。如果槽位占满时丢掉新帧，那么画面会一直
     *   停在旧帧上，延迟随时间累积（越看越滞后）；顶掉旧的，画面永远是最新的一帧，
     *   延迟恒定 —— 顶多是跳了一小段，眼睛几乎看不出来。
     *   这也是所有实时流产品的默认策略（WebRTC 的 jitter buffer 同理）。
     *
     * 绝不阻塞解码线程：这里不能等主线程来取。RTP 收包跟不上会丢包，
     * 而丢包是花屏，比丢帧严重得多。 */
    int pick = -1;
    bool replaced = false;
    slot_lock();
    for (int i = 0; i < VSLOT_MAX; i++) {
        if (s_v.slot[i].state == VSLOT_FREE) { pick = i; break; }
    }
    if (pick < 0) {
        for (int i = 0; i < VSLOT_MAX; i++) {
            if (s_v.slot[i].state == VSLOT_READY) { pick = i; replaced = true; break; }
        }
    }
    if (pick < 0) {
        s_v.drop_busy++;
        slot_unlock();
        return;
    }
    s_v.slot[pick].state = VSLOT_READY;      /* 先占位，防止重入时被挑走 */
    if (replaced) s_v.replace++;
    slot_unlock();

    vslot_t *sl = &s_v.slot[pick];
    size_t need = (size_t)dw * (size_t)dh * sizeof(uint16_t);
    if (sl->cap < need) {
        uint8_t *nb = (uint8_t *)realloc(sl->buf, need);
        if (!nb) {
            slot_lock();
            sl->state = VSLOT_FREE;
            slot_unlock();
            printf("[video] 槽位扩容失败（要 %u 字节）\n", (unsigned)need);
            return;
        }
        sl->buf = nb;
        sl->cap = need;
    }

    if (!yuv2rgb_565(f, (uint16_t *)(void *)sl->buf, dw, dh)) {
        slot_lock();
        sl->state = VSLOT_FREE;
        slot_unlock();
        return;
    }

    sl->w = dw;
    sl->h = dh;
    s_v.w = (uint32_t)f->w;      /* 统计里打印的是**源**分辨率，便于和参考帧对齐 */
    s_v.h = (uint32_t)f->h;
    s_v.frames++;

    if (!s_v.logged_size) {
        printf("[video] 解码输出 %dx%d → 上屏 %dx%d（first frame pts=%lld）\n",
               f->w, f->h, dw, dh, (long long)f->pts);
        s_v.logged_size = true;
    }

    /* READY 已经置了，像素也写完了，这里发事件（事件总线内部再加锁入队）。
       arg1 带的是"这是解码器吐出的第几帧"而不是"第几帧上屏"——
       中间丢了帧的时候两者会分叉，而抓屏比对要的是前者（对齐参考帧）。 */
    app_events_post(EV_VIDEO_FRAME, pick, (int32_t)frame_no, NULL);
}

/*---------------------------------------------------------------------------
 * 收 RTP
 *---------------------------------------------------------------------------*/
void app_video_on_rtp(const uint8_t *payload, int len, uint32_t ts)
{
    if (!s_v.opened || !s_v.dep || !payload || len <= 0) return;

    h264_depack_push(s_v.dep, payload, len, ts);

    const uint8_t *au = NULL;
    uint32_t au_ts = 0;
    int au_len;
    while ((au_len = h264_depack_pop(s_v.dep, &au, &au_ts)) > 0) {
        s_v.au_in++;
        if (!s_v.dec) continue;          /* 只解包模式 */

        vdec_feed(s_v.dec, au, au_len, (int64_t)au_ts);

        vframe_t f;
        int r;
        while ((r = vdec_get(s_v.dec, &f)) > 0) {
            s_v.dec_total++;
            if (s_v.sink == VSINK_FILE)      sink_write_frame(&f);
            else if (s_v.sink == VSINK_DISP) sink_disp_frame(&f, s_v.dec_total);
            else                             s_v.frames++;
        }
    }
}

/*---------------------------------------------------------------------------
 * 统计
 *---------------------------------------------------------------------------*/
void app_video_stats_print(const char *why)
{
    if (!s_v.opened) return;
    const h264_depack_stats_t *ds = h264_depack_stats(s_v.dep);
    const vdec_stats_t *vs = s_v.dec ? vdec_stats(s_v.dec) : NULL;

    printf("[video] stats(%s)%s", why ? why : "",
           ds ? "" : " (未启用)\n");
    if (!ds) return;

    printf(" pkt=%u au=%u nal=%u stap=%u fu=%u",
           ds->pkt_in, ds->au_done, ds->nal_done, ds->stap_pkt, ds->fu_seq);
    printf(" drop{type=%u,big=%u,slow=%u} fu_broken=%u",
           ds->drop_type, ds->drop_oversize, ds->drop_slow, ds->fu_broken);
    if (vs) printf(" dec{fed=%u,frame=%u,err=%u}", vs->fed, vs->frames, vs->errors);
    if (s_v.w) printf(" %ux%u", s_v.w, s_v.h);
    printf(" sink_frames=%u", s_v.frames);
    if (s_v.sink == VSINK_DISP)
        printf(" disp{shown=%u,replace=%u,drop=%u}",
               s_v.disp_frames, s_v.replace, s_v.drop_busy);
    printf("\n");
}
