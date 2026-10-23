/**
 * @file ui_call.c
 * @brief 对讲页 —— 来电/通话中（M3 + 视频上屏）
 *
 * 【设计要点】
 *   1. 只在主线程碰 LVGL：SIP 收包线程通过事件总线把状态送进来（见 app_events）；
 *      视频帧同理 —— 解码线程填好槽位发 EV_VIDEO_FRAME，本文件在主线程取走。
 *   2. 页面用 ui_nav 压/出栈，通话结束自动回到来电前的现场；
 *   3. 计时与 RTP 计数由一个 500ms 的 lv_timer 统一刷新 —— 不要在 SIP 线程里刷 UI。
 *   4. 视频源不拷贝：lv_image 直接指向 app_video 的槽位缓冲。省下每帧 124KB 的拷贝，
 *      代价是那块内存在"下一帧上屏前"不能被解码线程覆盖 —— 这个约束由
 *      app_video 的槽位状态机保证（见 app_video.c 的 FREE/READY/SHOWN）。
 */
#include "ui_call.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "app_events.h"
#include "app_video.h"

#ifdef APP_WITH_SIP
#include "app_sip.h"
#endif

#include <stdio.h>
#include <stdlib.h>

/* 视频窗口尺寸：宽铺满（320 - 16*2），按 4:3 取高 */
#define VIDEO_W 288
#define VIDEO_H 216

static lv_obj_t *s_scr;          /* 呼叫页（NULL = 未显示） */
static lv_obj_t *s_view;         /* 视频窗口容器（圆角裁剪） */
static lv_obj_t *s_ph;           /* 占位图标：还没收到画面时显示 */
static lv_obj_t *s_img;          /* 视频画面（收到第一帧后显示） */
static lv_obj_t *s_name_lbl;
static lv_obj_t *s_hint_lbl;
static lv_obj_t *s_timer_lbl;
static lv_obj_t *s_stat_lbl;
static lv_obj_t *s_btn_answer;
static lv_timer_t *s_tick;

/* lv_image 只存指针不拷贝，所以描述符必须常驻；内容每帧改写即可。
   （LVGL 的图片缓存已关：LV_CACHE_DEF_SIZE=0，不会因为指针没变而返回旧图） */
static lv_image_dsc_t s_dsc;

/* --- 抓屏取证（仅调试用）---
 * SIP_VIDEO_DISP_SHOT=<路径.ppm>   收到第 N 帧时把整个屏幕抓下来存成 PPM
 * SIP_VIDEO_DISP_SHOT_AT=<N>       第几帧抓（默认 30，即 1 秒处）
 * 为什么要这个：上屏这一路"看着对"没有说服力 —— 花屏、偏色、纵向翻转、
 * 只画半边，在小屏上都可能被当成"信号不好"。抓屏 + 逐像素比对才能给结论。 */
static char      s_shot_path[256];
static int       s_shot_at;
static lv_timer_t *s_shot_timer;
static bool      s_shot_done;
static uint32_t  s_last_frame_no;

static const char *peer_name(void)
{
#ifdef APP_WITH_SIP
    const char *n = app_sip_peer_name();
    return (n && *n) ? n : "门口机";
#else
    return "门口机";
#endif
}

static int call_seconds(void)
{
#ifdef APP_WITH_SIP
    return app_sip_call_seconds();
#else
    return 0;
#endif
}

static void rtp_stats(uint32_t *rx, uint32_t *tx)
{
#ifdef APP_WITH_SIP
    app_sip_rtp_stats(rx, tx);
#else
    *rx = *tx = 0;
#endif
}

static void fmt_time(char *buf, size_t n, int sec)
{
    if (sec < 0) sec = 0;
    snprintf(buf, n, "%02d:%02d", sec / 60, sec % 60);
}

/*---------------------------------------------------------------------------
 * 抓屏
 *---------------------------------------------------------------------------*/
static void write_ppm(const char *path, const lv_draw_buf_t *db)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        printf("[disp] 抓屏文件写不了：%s\n", path);
        return;
    }
    const unsigned w = db->header.w, h = db->header.h;
    fprintf(fp, "P6\n%u %u\n255\n", w, h);

    /* 按 RGB565 取的快照：展开成 888 时用**位复制**（r5<<3 | r5>>2），
       这样 scripts/disp_compare.py 再 >>3 量化回来能精确还原成原来那个 565 值。
       如果用 <<3 补零，量化回来也一样；用位复制只是让肉眼看的时候渐变更顺。 */
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = db->data + (size_t)y * db->header.stride;
        for (unsigned x = 0; x < w; x++) {
            uint16_t p = ((const uint16_t *)(const void *)row)[x];
            unsigned r5 = (p >> 11) & 0x1F;
            unsigned g6 = (p >> 5)  & 0x3F;
            unsigned b5 =  p        & 0x1F;
            uint8_t rgb[3] = {
                (uint8_t)((r5 << 3) | (r5 >> 2)),
                (uint8_t)((g6 << 2) | (g6 >> 4)),
                (uint8_t)((b5 << 3) | (b5 >> 2)),
            };
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
}

static void shot_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    s_shot_timer = NULL;
    if (!s_img) return;

    /* 抓完这一帧再等 80ms 才抓，是为了让 LVGL 真的把它画到屏幕缓冲区里 ——
       刚 set_src 完就抓，抓到的是上一帧（这个时序坑很难看出来，
       因为对比结果会"像是差一帧"，很容易误判成解码延迟）。 */
    lv_draw_buf_t *db = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!db) {
        printf("[disp] 抓屏失败（LVGL 堆不够？快照要 %ux%ux2 字节）\n",
               (unsigned)lv_obj_get_width(lv_screen_active()),
               (unsigned)lv_obj_get_height(lv_screen_active()));
        return;
    }

    lv_area_t a;
    lv_obj_get_coords(s_img, &a);
    write_ppm(s_shot_path, db);

    printf("[disp] 抓屏 → %s  screen=%ux%u  img_box=%d,%d,%d,%d  dec_frame=%u\n",
           s_shot_path, (unsigned)db->header.w, (unsigned)db->header.h,
           (int)a.x1, (int)a.y1, (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
           (unsigned)s_last_frame_no);

    lv_draw_buf_destroy(db);
}

/*---------------------------------------------------------------------------
 * 事件
 *---------------------------------------------------------------------------*/
static void on_answer(lv_event_t *e)
{
    (void)e;
#ifdef APP_WITH_SIP
    app_sip_answer();
#endif
}

static void on_hangup(lv_event_t *e)
{
    (void)e;
#ifdef APP_WITH_SIP
    app_sip_hangup();
#endif
}

/* 视频帧到了（主线程，由事件总线的 30ms timer 分发过来） */
static void on_video_frame(const app_event_t *e, void *user)
{
    (void)user;
    if (!s_img || !s_view) return;         /* 页面还没建（或已销毁）：直接丢 */

    const uint8_t *px = NULL;
    int w = 0, h = 0;
    if (!app_video_disp_acquire(e->arg0, &px, &w, &h) || !px) return;

    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.flags  = 0;
    s_dsc.header.w      = (uint16_t)w;
    s_dsc.header.h      = (uint16_t)h;
    s_dsc.header.stride = (uint16_t)(w * 2);
    s_dsc.data_size     = (uint32_t)w * (uint32_t)h * 2u;
    s_dsc.data          = px;

    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_size(s_img, w, h);
    lv_obj_center(s_img);
    lv_obj_set_hidden(s_img, false);
    if (s_ph) lv_obj_set_hidden(s_ph, true);

    s_last_frame_no = (uint32_t)e->arg1;

    if (s_shot_at > 0 && !s_shot_done && !s_shot_timer &&
        (int)s_last_frame_no >= s_shot_at) {
        s_shot_done = true;      /* 只抓一次：不置这个位会每 3 帧抓一次，
                                    文件被后面几十帧覆盖，比对时就对不上号了 */
        /* 延迟到渲染完再抓，见 shot_timer_cb 里的说明 */
        s_shot_timer = lv_timer_create(shot_timer_cb, 80, NULL);
    }
}

static void tick_cb(lv_timer_t *t)
{
    if (!s_scr) { lv_timer_delete(t); s_tick = NULL; return; }

    char b[32];
    fmt_time(b, sizeof(b), call_seconds());
    lv_label_set_text(s_timer_lbl, b);

    uint32_t rx = 0, tx = 0;
    rtp_stats(&rx, &tx);
    uint32_t vf = app_video_disp_frames();
    char s[96];
    if (vf)
        snprintf(s, sizeof(s), "RTP %u/%u · 画面 %u", (unsigned)rx, (unsigned)tx, (unsigned)vf);
    else
        snprintf(s, sizeof(s), "RTP 收 %u / 发 %u", (unsigned)rx, (unsigned)tx);
    lv_label_set_text(s_stat_lbl, s);
}

static void on_page_delete(lv_event_t *e)
{
    (void)e;
    /* 先断掉指向槽位缓冲的引用：页面销毁后 lv_image 不再存在，
       但槽位里的内存还归解码线程复用，不能留着悬垂指针。 */
    s_img = NULL;
    s_view = NULL;
    s_ph  = NULL;
    s_scr = NULL;
    s_name_lbl = s_hint_lbl = s_timer_lbl = s_stat_lbl = NULL;
    s_btn_answer = NULL;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
    if (s_shot_timer) { lv_timer_delete(s_shot_timer); s_shot_timer = NULL; }
}

/*---------------------------------------------------------------------------
 * 页面
 *---------------------------------------------------------------------------*/
static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *sym, const char *text,
                                lv_style_t *style, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 84, 84);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_style(btn, style, 0);
    lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn, 4, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, sym);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, FONT_CJK, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* 视频窗口：圆角容器 + 占位图标 + 画面本体，两层叠在一起 */
static void build_video_view(lv_obj_t *scr)
{
    s_view = lv_obj_create(scr);
    lv_obj_remove_style_all(s_view);
    lv_obj_set_size(s_view, VIDEO_W, VIDEO_H);
    /* 直角，不用圆角：圆角会把四角裁成背景色，那些像素必然和参考帧不同，
       逐像素比对就废了（只能退化成"看个大概"或者引入掩膜）。
       这里选"可验证"优先 —— 见 docs/视频上屏验证.md 的取舍说明。 */
    lv_obj_set_style_radius(s_view, 0, 0);
    lv_obj_set_style_bg_color(s_view, lv_color_hex(0x121C26), 0);
    lv_obj_set_style_bg_opa(s_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_view, 1, 0);
    lv_obj_set_style_border_color(s_view, lv_color_hex(0x24313D), 0);
    lv_obj_set_style_clip_corner(s_view, true, 0);     /* 画面不溢出边框 */
    lv_obj_set_scrollbar_mode(s_view, LV_SCROLLBAR_MODE_OFF);

    /* 占位：还没收到画面时显示，收到第一帧后隐藏。
       真实产品这里通常是"门口机离线"或头像 —— 明确留白反而比如实显示更有信息量。 */
    s_ph = lv_obj_create(s_view);
    lv_obj_remove_style_all(s_ph);
    lv_obj_set_size(s_ph, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_ph, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ph, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ph, 6, 0);

    lv_obj_t *ic = lv_label_create(s_ph);
    lv_label_set_text(ic, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0x3D4F60), 0);

    lv_obj_t *t = lv_label_create(s_ph);
    lv_label_set_text(t, "等待画面");
    lv_obj_set_style_text_font(t, FONT_CJK, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x4E6072), 0);

    s_img = lv_image_create(s_view);
    lv_obj_set_hidden(s_img, true);
    /* 控件尺寸 == 图片尺寸，所以居中即可，不需要任何缩放 —— 缩放会走变换路径，
       不仅慢，还会让"抓屏 vs 参考帧"的逐像素比对失去意义。 */
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
}

static void build_call_page(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_scr = scr;

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1620), 0);

    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);

    /* 视频窗口：告诉解码线程目标尺寸，按这个框等比缩放 */
    app_video_set_box(VIDEO_W, VIDEO_H);
    build_video_view(scr);

    s_name_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_name_lbl, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_name_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_name_lbl, peer_name());

    s_hint_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint_lbl, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(0x9AA7B4), 0);
    lv_label_set_text(s_hint_lbl, "来电呼叫");

    s_timer_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_timer_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_timer_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_timer_lbl, "");

    s_stat_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_stat_lbl, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_stat_lbl, lv_color_hex(0x6E7B8A), 0);
    lv_label_set_text(s_stat_lbl, "");

    /* 按钮行 */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 28, 0);
    lv_obj_set_style_pad_top(row, 6, 0);

    s_btn_answer = make_round_btn(row, LV_SYMBOL_CALL, "接听", &st_btn_ok, on_answer);
    make_round_btn(row, LV_SYMBOL_CLOSE, "挂断", &st_btn_danger, on_hangup);

    lv_obj_add_event_cb(scr, on_page_delete, LV_EVENT_DELETE, NULL);
}

static void open_page(void)
{
    if (s_scr) return;
    ui_nav_push(build_call_page, NULL);
}

/*---------------------------------------------------------------------------
 * 状态订阅
 *---------------------------------------------------------------------------*/
static void on_sip_state(const app_event_t *e, void *user)
{
    (void)user;
    int ui = e->arg0;   /* 0=idle 1=online 2=来电 3=通话中 4=呼出中 */

    if (ui == 2) {                          /* 来电：弹页面 */
        open_page();
        if (!s_scr) return;
        lv_label_set_text(s_name_lbl, peer_name());
        lv_label_set_text(s_hint_lbl, "来电呼叫");
        lv_label_set_text(s_timer_lbl, "");
        lv_label_set_text(s_stat_lbl, "");
        lv_obj_set_hidden(s_btn_answer, false);
        return;
    }

    if (ui == 3) {                          /* 通话中 */
        open_page();
        if (!s_scr) return;
        lv_obj_set_hidden(s_btn_answer, true);
        lv_label_set_text(s_hint_lbl, "通话中");
        if (!s_tick) s_tick = lv_timer_create(tick_cb, 500, NULL);
        return;
    }

    /* 其余状态（含挂断后回到 online）：收页面 */
    if (s_scr) {
        s_scr = NULL;                       /* 先置空，避免 pop 触发 DELETE 时重入 */
        ui_nav_pop();
    }
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
}

void ui_call_init(void)
{
    app_events_subscribe(EV_SIP_STATE, on_sip_state, NULL);
    app_events_subscribe(EV_VIDEO_FRAME, on_video_frame, NULL);

    const char *p = getenv("SIP_VIDEO_DISP_SHOT");
    if (p && *p) {
        snprintf(s_shot_path, sizeof(s_shot_path), "%s", p);
        const char *a = getenv("SIP_VIDEO_DISP_SHOT_AT");
        s_shot_at = (a && *a) ? atoi(a) : 30;
        if (s_shot_at < 1) s_shot_at = 1;
        printf("[disp] 抓屏已开启：第 %d 帧 → %s\n", s_shot_at, s_shot_path);
    }
}
