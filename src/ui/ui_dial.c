/**
 * @file ui_dial.c
 * @brief 拨号页 —— 房号键盘 + 呼叫（室内机的基本能力：呼其它房间/门口机）
 *
 * 【目标地址怎么拼】（与 app_sip_call 的语义对齐）
 *   输入里带 '@'          → 按 "num@host[:port]" 直呼（绕过服务器）
 *   纯数字 + 已注册服务器 → 交原样给 app_sip_call，它拼 "sip:num@server:port"
 *   纯数字 + 没有服务器   → 提示，不拨 —— 房号路由必须靠服务器，瞎猜 IP 是骗人
 *
 * 【为什么用 lv_btnmatrix 而不是 12 个按钮】
 *   布局、点击命中、按下态全是它管，代码量少一半；
 *   代价是样式统一（一个字体）—— 数字和"删除/呼叫"共用 CJK 24px，可接受。
 */
#include "ui_dial.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_theme.h"

#ifdef APP_WITH_SIP
#include "app_sip.h"
#endif
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#define NUM_MAX 16

static lv_obj_t *s_disp;        /* 已输入的房号（大字） */
static lv_obj_t *s_hint;        /* 提示行 */

static const char *s_hint_idle;
static char s_num[NUM_MAX];

static void refresh(void)
{
    lv_label_set_text(s_disp, s_num[0] ? s_num : "—");
#ifdef APP_WITH_SIP
    s_hint_idle = app_sip_registered() ? "输入房号，按呼叫拨出"
                                       : "未注册服务器：请拨 房号@IP:端口";
#else
    s_hint_idle = "SIP 未启用";
#endif
    lv_label_set_text(s_hint, s_hint_idle);
}

static void do_call(void)
{
#ifdef APP_WITH_SIP
    if (!s_num[0]) {
        lv_label_set_text(s_hint, "请先输入房号");
        return;
    }
    /* 带 '@' 的按直呼 URI 用；纯房号必须有服务器路由 */
    if (!strchr(s_num, '@') && !app_sip_registered()) {
        lv_label_set_text(s_hint, "未注册服务器，无法按房号呼叫");
        return;
    }
    if (app_sip_call(s_num) == 0) {
        ui_nav_pop();               /* 呼出后由 ui_call 的"呼叫中"页接管 */
        return;
    }
    lv_label_set_text(s_hint, "当前状态不能发起呼叫");
#else
    lv_label_set_text(s_hint, "SIP 未启用");
#endif
}

static void kb_cb(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(bm);
    const char *txt = lv_buttonmatrix_get_button_text(bm, id);
    if (!txt) return;

    if (!strcmp(txt, "删除")) {
        if (s_num[0]) s_num[strlen(s_num) - 1] = '\0';
        refresh();
        return;
    }
    if (!strcmp(txt, "呼叫")) {
        do_call();
        return;
    }
    size_t n = strlen(s_num);
    if (n + 1 < NUM_MAX) {
        s_num[n] = txt[0];
        s_num[n + 1] = '\0';
        refresh();
    }
}

static void build(lv_obj_t *scr, void *arg);

void ui_dial_open(void)
{
    ui_nav_push(build, NULL);
}

static void build(lv_obj_t *scr, void *arg)
{
    (void)arg;
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0E1620), 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 8, 0);

    ui_topbar_create(scr, "呼叫房号");

    /* 显示屏 */
    s_disp = lv_label_create(scr);
    lv_obj_set_width(s_disp, 296);
    lv_obj_set_style_text_font(s_disp, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_disp, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_disp, lv_color_hex(0x121C26), 0);
    lv_obj_set_style_bg_opa(s_disp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_disp, 8, 0);
    lv_obj_set_style_pad_all(s_disp, 10, 0);
    lv_label_set_long_mode(s_disp, LV_LABEL_LONG_SCROLL);

    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hint, FONT_CJK, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x6E7B8A), 0);

    /* 键盘 */
    static const char *kb_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        "删除", "0", "呼叫", ""
    };
    static const lv_buttonmatrix_ctrl_t ctrl[] = {
        1, 1, 1,
        1, 1, 1,
        1, 1, 1,
        1, 1, 2,            /* 呼叫占两格宽 */
    };
    lv_obj_t *bm = lv_buttonmatrix_create(scr);
    lv_buttonmatrix_set_map(bm, kb_map);
    lv_buttonmatrix_set_ctrl_map(bm, ctrl);
    lv_buttonmatrix_set_one_checked(bm, false);
    lv_obj_set_size(bm, 296, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(bm, FONT_CJK_LG, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(bm, 4, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x1B2836), LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 8, LV_PART_ITEMS);
    lv_obj_set_style_border_width(bm, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_event_cb(bm, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_num[0] = '\0';
    refresh();
}
