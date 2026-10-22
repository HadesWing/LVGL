/**
 * @file ui_indoor.c
 * @brief 室内机主界面实现 —— 第一个自研 LVGL 界面
 *
 * 注意本文件用的都是 LVGL v9 API（v8 → v9 改名对照见 README）：
 *   lv_scr_act()      → lv_screen_active()
 *   lv_btn_create()   → lv_button_create()
 *   lv_disp_*         → lv_display_*
 */
#include "ui_indoor.h"
#include "lvgl.h"
#include <time.h>

/*---------------------------------------------------------------------------
 * 私有对象：整个界面只创建一次，之后只改属性不重建（省内存池、防碎片）
 *---------------------------------------------------------------------------*/
static lv_obj_t *scr;             /* 根屏幕 */
static lv_obj_t *status_bar;      /* 顶部状态栏 */
static lv_obj_t *time_label;      /* 时间显示 */
static lv_obj_t *video_area;      /* 视频占位区 */
static lv_obj_t *video_hint;      /* 视频区中央提示文字 */
static lv_obj_t *call_popup;      /* 来电弹窗 */
static lv_obj_t *call_state_label;/* 呼叫状态提示 */

/*---------------------------------------------------------------------------
 * 时钟定时器：每秒刷新一次状态栏时间
 * PC 上用 time() 取系统时间；上板子后换成 RTC / NTP，接口不变
 *---------------------------------------------------------------------------*/
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    char buf[8];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    lv_label_set_text_fmt(time_label, "%02d:%02d", lt->tm_hour, lt->tm_min);
}

/*---------------------------------------------------------------------------
 * 事件回调：LVGL 的事件模型 —— 控件产生事件，你注册回调响应
 *---------------------------------------------------------------------------*/
static void on_answer_clicked(lv_event_t *e)
{
    (void)e;
    /* 接听：弹窗收起，视频区进入"通话中"状态 */
    lv_obj_add_flag(call_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(video_area, lv_color_hex(0x2E7D32), 0);
    lv_obj_set_style_border_width(video_area, 3, 0);
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Connected");
}

static void on_reject_clicked(lv_event_t *e)
{
    (void)e;
    /* 拒接：弹窗收起，视频区恢复待机 */
    lv_obj_add_flag(call_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(video_area, 0, 0);
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Door Camera");
}

/*---------------------------------------------------------------------------
 * 来电弹窗：2 秒后由定时器触发，从屏幕顶部滑入（lv_anim 动画）
 *---------------------------------------------------------------------------*/
static void show_call_popup(lv_timer_t *t)
{
    (void)t;
    lv_obj_remove_flag(call_popup, LV_OBJ_FLAG_HIDDEN);

    /* 滑入动画：y 从 -150（屏外）滑到 40，400ms，ease-out 缓动 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, call_popup);
    lv_anim_set_values(&a, -150, 40);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);  /* 每帧改 y 坐标 */
    lv_anim_set_duration(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 构建"接听/拒接"按钮对的工具函数 —— 同样的代码造两套按钮，这就是复用 */
static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, const char *txt,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_center(label);
    return btn;
}

/*===========================================================================
 * 界面入口：由 main() 调用一次
 *===========================================================================*/
void ui_indoor_init(void)
{
    scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xECEFF1), 0);  /* 浅灰底 */

    /*---------------- 1. 状态栏（32px 高，深灰底） ----------------*/
    status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, 320, 32);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x263238), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);

    time_label = lv_label_create(status_bar);
    lv_obj_set_pos(time_label, 8, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(time_label, "00:00");

    /* 右侧系统图标：LVGL 内置符号字体，不用图片资源 */
    lv_obj_t *icons = lv_label_create(status_bar);
    lv_obj_align(icons, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_text_color(icons, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(icons, LV_SYMBOL_WIFI "  " LV_SYMBOL_BATTERY_FULL);

    /*---------------- 2. 视频占位区（正式产品 = 解码器输出帧贴到这） ----------------*/
    video_area = lv_obj_create(scr);
    lv_obj_set_size(video_area, 296, 220);
    lv_obj_set_pos(video_area, 12, 44);
    lv_obj_set_style_bg_color(video_area, lv_color_hex(0x37474F), 0);
    lv_obj_set_style_radius(video_area, 8, 0);
    lv_obj_set_style_border_width(video_area, 0, 0);

    video_hint = lv_label_create(video_area);
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Door Camera");
    lv_obj_set_style_text_color(video_hint, lv_color_hex(0xB0BEC5), 0);
    lv_obj_center(video_hint);

    call_state_label = lv_label_create(scr);
    lv_obj_set_pos(call_state_label, 12, 272);
    lv_obj_set_style_text_color(call_state_label, lv_color_hex(0x546E7A), 0);
    lv_label_set_text(call_state_label, "Door Station 101  ·  Online");

    /*---------------- 3. 底部常驻操作区 ----------------*/
    make_button(scr, 12, 400, 140, 64, lv_color_hex(0xC62828),
                LV_SYMBOL_CLOSE "  End", on_reject_clicked);
    make_button(scr, 168, 400, 140, 64, lv_color_hex(0x2E7D32),
                LV_SYMBOL_CALL "  Answer", on_answer_clicked);

    /*---------------- 4. 来电弹窗（默认隐藏，定时器触发滑入） ----------------*/
    call_popup = lv_obj_create(scr);
    lv_obj_set_size(call_popup, 280, 150);
    lv_obj_set_pos(call_popup, 20, -150);           /* 初始在屏幕外 */
    lv_obj_set_style_bg_color(call_popup, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(call_popup, 12, 0);
    lv_obj_set_style_border_width(call_popup, 0, 0);
    lv_obj_set_style_shadow_width(call_popup, 40, 0);
    lv_obj_set_style_shadow_opa(call_popup, LV_OPA_40, 0);
    lv_obj_add_flag(call_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *caller = lv_label_create(call_popup);
    lv_obj_set_pos(caller, 16, 12);
    lv_obj_set_style_text_color(caller, lv_color_hex(0x263238), 0);
    lv_label_set_text(caller, LV_SYMBOL_BELL "  Door Station 101");

    lv_obj_t *sub = lv_label_create(call_popup);
    lv_obj_set_pos(sub, 16, 38);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x78909C), 0);
    lv_label_set_text(sub, "Incoming video call...");

    make_button(call_popup, 12, 78, 120, 52, lv_color_hex(0x2E7D32),
                LV_SYMBOL_CALL "  Answer", on_answer_clicked);
    make_button(call_popup, 148, 78, 120, 52, lv_color_hex(0xC62828),
                LV_SYMBOL_CLOSE "  Reject", on_reject_clicked);

    /*---------------- 5. 两个周期定时器 ----------------*/
    lv_timer_create(clock_timer_cb, 1000, NULL);   /* 1s 刷时钟 */
    lv_timer_create(show_call_popup, 2000, NULL);  /* 2s 后来电演示 */
}
