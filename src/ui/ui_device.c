/**
 * @file ui_device.c
 * @brief 三级页面：设备详情
 *
 * 反例对照：如果这里也"点击 → 自己改控件"，页面栈一多就会出现
 * "详情页改了、分类页没刷新"的脏数据。这里坚持：动作只调 device_execute()，
 * 界面统一从 device_get() 读真实状态 —— 单一数据源。
 */
#include "ui_device.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "app_devices.h"
#include "lvgl.h"

#include <stdio.h>

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_icon;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_power_btn;
static lv_obj_t *s_power_lbl;
static lv_obj_t *s_slider;
static lv_obj_t *s_value_lbl;
static int       s_idx;
static bool      s_active;

LV_FONT_DECLARE(lv_font_montserrat_32)

/*---------------------------------------------------------------------------
 * 从真实状态刷新整页（唯一渲染入口）
 *---------------------------------------------------------------------------*/
static void refresh_page(void)
{
    const device_t *d = device_get(s_idx);
    if (!d || !s_active) return;

    lv_label_set_text(s_state_lbl, device_state_text(d));
    lv_obj_set_style_text_color(s_icon,
                                d->on ? lv_palette_main(LV_PALETTE_LIGHT_BLUE)
                                      : lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(s_power_lbl, d->on ? "关闭" : "打开");
    lv_obj_set_style_bg_color(s_power_btn,
                              d->on ? lv_palette_main(LV_PALETTE_GREY)
                                    : lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);

    int16_t lo, hi;
    device_value_range(d->type, &lo, &hi);
    if (s_slider) {
        lv_slider_set_range(s_slider, lo, hi);
        if (lv_slider_get_value(s_slider) != d->value) lv_slider_set_value(s_slider, d->value, LV_ANIM_OFF);
        char buf[24];
        snprintf(buf, sizeof(buf), "当前：%d", d->value);
        lv_label_set_text(s_value_lbl, buf);
    }
}

/*---------------------------------------------------------------------------
 * 交互
 *---------------------------------------------------------------------------*/
static void power_click_cb(lv_event_t *e)
{
    (void)e;
    const device_t *d = device_get(s_idx);
    if (!d) return;
    device_cmd_t cmd = { d->id, d->on ? CMD_OFF : CMD_ON, 0 };
    device_execute(&cmd);
    refresh_page();
}

static void slider_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_slider) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "当前：%d", (int)lv_slider_get_value(s_slider));
    lv_label_set_text(s_value_lbl, buf);
}

static void slider_released_cb(lv_event_t *e)
{
    (void)e;
    const device_t *d = device_get(s_idx);
    if (!d || !s_slider) return;
    device_cmd_t cmd = { d->id, CMD_SET, (int16_t)lv_slider_get_value(s_slider) };
    device_execute(&cmd);
    refresh_page();
}

static void on_page_delete(lv_event_t *e)
{
    (void)e;
    s_active = false;
    s_slider = NULL;
}

/*---------------------------------------------------------------------------
 * 构建
 *---------------------------------------------------------------------------*/
static void build_device(lv_obj_t *scr, void *arg)
{
    s_idx = (int)(intptr_t)arg;
    s_active = true;
    const device_t *d = device_get(s_idx);
    if (!d) { s_active = false; return; }

    ui_page_create(scr);
    lv_obj_add_event_cb(scr, on_page_delete, LV_EVENT_DELETE, NULL);
    ui_topbar_create(scr, d->name);

    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_add_style(body, &st_screen, 0);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_scrollable(body, false);

    s_icon = lv_label_create(body);
    lv_label_set_text(s_icon, device_type_icon(d->type));
    lv_obj_set_style_text_font(s_icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_pad_top(s_icon, 10, 0);

    s_title_lbl = lv_label_create(body);
    lv_obj_add_style(s_title_lbl, &st_text_primary, 0);
    lv_obj_set_style_text_font(s_title_lbl, FONT_CJK, 0);
    lv_label_set_text(s_title_lbl, d->name);

    s_state_lbl = lv_label_create(body);
    lv_obj_add_style(s_state_lbl, &st_text_muted, 0);
    lv_obj_set_style_text_font(s_state_lbl, FONT_CJK, 0);
    lv_label_set_text(s_state_lbl, device_state_text(d));

    /* 参数滑条（只有可调类型才建） */
    int16_t lo, hi;
    device_value_range(d->type, &lo, &hi);
    s_slider = NULL;
    s_value_lbl = NULL;
    if (hi > lo) {
        s_slider = lv_slider_create(body);
        lv_obj_set_width(s_slider, lv_pct(85));
        lv_slider_set_range(s_slider, lo, hi);
        lv_slider_set_value(s_slider, d->value, LV_ANIM_OFF);
        lv_obj_add_event_cb(s_slider, slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(s_slider, slider_released_cb, LV_EVENT_RELEASED, NULL);

        s_value_lbl = lv_label_create(body);
        lv_obj_add_style(s_value_lbl, &st_text_muted, 0);
        lv_obj_set_style_text_font(s_value_lbl, FONT_CJK, 0);
        lv_label_set_text(s_value_lbl, "");
    }

    /* 开关按钮 */
    s_power_btn = lv_button_create(body);
    lv_obj_set_size(s_power_btn, 160, 48);
    lv_obj_add_style(s_power_btn, &st_btn_common, 0);
    lv_obj_add_style(s_power_btn, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_power_btn, power_click_cb, LV_EVENT_CLICKED, NULL);

    s_power_lbl = lv_label_create(s_power_btn);
    lv_obj_add_style(s_power_lbl, &st_btn_label, 0);
    lv_obj_set_style_text_font(s_power_lbl, FONT_CJK, 0);
    lv_obj_center(s_power_lbl);

    refresh_page();
}

void ui_device_open(int idx)
{
    ui_nav_push(build_device, (void *)(intptr_t)idx);
}
