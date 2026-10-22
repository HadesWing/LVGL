/**
 * @file ui_call.c
 * @brief 对讲页 —— 来电/通话中（M3）
 *
 * 【设计要点】
 *   1. 只在主线程碰 LVGL：SIP 收包线程通过事件总线把状态送进来（见 app_events）；
 *   2. 页面用 ui_nav 压/出栈，通话结束自动回到来电前的现场；
 *   3. 计时与 RTP 计数由一个 500ms 的 lv_timer 统一刷新 —— 不要在 SIP 线程里刷 UI。
 */
#include "ui_call.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "app_events.h"

#ifdef APP_WITH_SIP
#include "app_sip.h"
#endif

#include <stdio.h>
#include <stdlib.h>

static lv_obj_t *s_scr;          /* 呼叫页（NULL = 未显示） */
static lv_obj_t *s_name_lbl;
static lv_obj_t *s_hint_lbl;
static lv_obj_t *s_timer_lbl;
static lv_obj_t *s_stat_lbl;
static lv_obj_t *s_btn_answer;
static lv_timer_t *s_tick;

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

static void tick_cb(lv_timer_t *t)
{
    if (!s_scr) { lv_timer_delete(t); s_tick = NULL; return; }

    char b[32];
    fmt_time(b, sizeof(b), call_seconds());
    lv_label_set_text(s_timer_lbl, b);

    uint32_t rx = 0, tx = 0;
    rtp_stats(&rx, &tx);
    char s[80];
    snprintf(s, sizeof(s), "RTP 收 %u / 发 %u", (unsigned)rx, (unsigned)tx);
    lv_label_set_text(s_stat_lbl, s);
}

static void on_page_delete(lv_event_t *e)
{
    (void)e;
    s_scr = NULL;
    s_name_lbl = s_hint_lbl = s_timer_lbl = s_stat_lbl = NULL;
    s_btn_answer = NULL;
    if (s_tick) { lv_timer_delete(s_tick); s_tick = NULL; }
}

/*---------------------------------------------------------------------------
 * 页面
 *---------------------------------------------------------------------------*/
static lv_obj_t *make_round_btn(lv_obj_t *parent, const char *sym, const char *text,
                                lv_style_t *style, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 88, 88);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_style(btn, style, 0);
    lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn, 6, 0);

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

static void build_call_page(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_scr = scr;

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1620), 0);

    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 10, 0);

    /* 头像：真实产品这里是视频画面（M6 接 H.264 解码后换成 lv_image） */
    lv_obj_t *avatar = lv_obj_create(scr);
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, 96, 96);
    lv_obj_set_style_radius(avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(avatar, lv_color_hex(0x1F6FEB), 0);
    lv_obj_set_style_bg_opa(avatar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(avatar, 3, 0);
    lv_obj_set_style_border_color(avatar, lv_color_hex(0x58A6FF), 0);
    lv_obj_set_flex_flow(avatar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(avatar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *ic = lv_label_create(avatar);
    lv_label_set_text(ic, LV_SYMBOL_CALL);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(0xFFFFFF), 0);

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
    lv_obj_set_style_pad_top(row, 12, 0);

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
}
