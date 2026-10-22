/**
 * @file app_video.c
 * @brief 视频通路：RTP → 解包 → 解码 → 落盘/上屏
 *
 * 落盘为什么用裸 YUV420P 而不是 mp4/avi：
 *   1) 判据要"无损"。编码成视频文件又是一次有损压缩，比出来的差异分不清是
 *      解码错还是二次编码造成的 —— 那样验收就失去意义了。
 *   2) 裸 YUV 是解码器的**直接输出**，一字节不差地和参考 YUV 比对，
 *      不同就一定是解码链路有问题，没有第二种解释。
 *   3) 想肉眼看的话 ffplay 一行就能播：
 *      ffplay -f rawvideo -pixel_format yuv420p -video_size 640x480 video_rx.yuv
 */
#include "app_video.h"
#include "h264_depack.h"
#include "vdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { VSINK_DROP = 0, VSINK_FILE, VSINK_DISP } vsink_t;

static struct {
    bool             opened;
    vsink_t          sink;
    bool             nodec;        /* 只解包不解码 */
    char             sink_path[256];

    h264_depack_t   *dep;
    vdec_t          *dec;
    FILE            *fp;

    uint32_t         frames;       /* 已落盘/已上屏的帧数 */
    uint32_t         au_in;        /* 收到的访问单元数 */
    uint32_t         w, h;
    uint64_t         bytes;
    bool             logged_size;  /* 尺寸只打一次，避免 30fps 刷屏 */
} s_v;

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
    n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n, " dec=%s/%s",
             s_v.nodec ? "off" : "on", vdec_backend_name());
    return buf;
}

/*---------------------------------------------------------------------------
 * 打开/关闭
 *---------------------------------------------------------------------------*/
bool app_video_open(void)
{
    if (s_v.opened) return true;
    route_parse_env();

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
    printf("[video] closed (%u 帧, %.1f MB)\n",
           s_v.frames, (double)s_v.bytes / (1024.0 * 1024.0));
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
            s_v.w = (uint32_t)f.w;
            s_v.h = (uint32_t)f.h;
            if (s_v.sink == VSINK_FILE) sink_write_frame(&f);
            else                        s_v.frames++;   /* VSINK_DISP：M6 之后接 lv_image */
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
    printf(" sink_frames=%u\n", s_v.frames);
}
