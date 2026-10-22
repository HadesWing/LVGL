/**
 * @file ui_indoor.c
 * @brief 室内机主界面 —— 版本 2：全部改用命名样式（不再有内联颜色/圆角）
 *
 * 相比 v1 的三处实质变化（这正是生产代码与 demo 的差距）：
 *   1. 颜色/圆角/阴影/字体全部来自 ui_theme 的命名样式，控件代码里看不到一个色值；
 *   2. 状态表达用状态样式（LV_STATE_PRESSED）与"换样式"（add/remove style），
 *      而不是直接改属性 —— 换主题时不会丢；
 *   3. 新增主题切换：点状态栏右上角眼睛图标，明/暗主题即时切换。
 *
 * 【跨平台】本文件只 include lvgl.h + 标准 C，无任何平台专有调用，
 * Windows / Linux / RTOS 上编译同一份代码即可（差异只在 hal/ 层）。
 */
#include "ui_indoor.h"
#include "ui_theme.h"
#include "lvgl.h"
#include <time.h>

/*---------------------------------------------------------------------------
 * 私有对象：界面只创建一次，之后只换样式/改文字（不重建，防内存池碎片）
 *---------------------------------------------------------------------------*/
static lv_obj_t *scr;
static lv_obj_t *time_label;
static lv_obj_t *video_area;
static lv_obj_t *video_hint;
static lv_obj_t *call_popup;
static lv_obj_t *call_state_label;

/*---------------------------------------------------------------------------
 * 时钟定时器
 *---------------------------------------------------------------------------*/
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    lv_label_set_text_fmt(time_label, "%02d:%02d", lt->tm_hour, lt->tm_min);
}

/*---------------------------------------------------------------------------
 * 主题切换（样式热替换 + 图标同步的综合演示）
 *---------------------------------------------------------------------------*/
static void on_theme_clicked(lv_event_t *e)
{
    lv_obj_t *icon = lv_obj_get_child(lv_event_get_target_obj(e), 0);
    ui_theme_toggle();
    lv_label_set_text(icon, ui_theme_get() == UI_THEME_DARK
                                ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

/*---------------------------------------------------------------------------
 * 接听 / 拒接：不是改 Border 属性，而是给视频区"加/减一条样式"
 *---------------------------------------------------------------------------*/
static void on_answer_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_set_hidden(call_popup, true);
    lv_obj_add_style(video_area, &st_surface_calling, 0);   /* ← 换样式 */
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Connected");
}

static void on_reject_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_set_hidden(call_popup, true);
    lv_obj_remove_style(video_area, &st_surface_calling, 0);/* ← 撤回样式 */
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Door Camera");
}

/*---------------------------------------------------------------------------
 * 来电弹窗：2 秒后滑入（动画只改 y，外观仍由样式决定）
 *---------------------------------------------------------------------------*/
void ui_indoor_simulate_call(void)
{
    if (!call_popup) return;                 /* 界面尚未创建 */
    lv_obj_set_hidden(call_popup, false);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, call_popup);
    lv_anim_set_values(&a, -150, 40);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_duration(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 首启 2 秒后自动来一次电，方便直接看到效果 */
static void show_call_popup(lv_timer_t *t)
{
    (void)t;
    ui_indoor_simulate_call();
}

void ui_indoor_set_status_text(const char *txt)
{
    if (call_state_label) lv_label_set_text(call_state_label, txt);
}

/*---------------------------------------------------------------------------
 * 按钮工厂：一条样式链复用给所有按钮
 *   st_btn_common（形状） + st_btn_pressed（按下态） + 语义色（绿/红）
 *---------------------------------------------------------------------------*/
static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h,
                             lv_style_t *semantic_style,
                             const char *txt, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);

    lv_obj_add_style(btn, &st_btn_common, 0);
    lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);  /* 按下下沉 */
    lv_obj_add_style(btn, semantic_style, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, txt);
    lv_obj_add_style(label, &st_btn_label, 0);
    lv_obj_center(label);
    return btn;
}

/*===========================================================================
 * 界面入口
 *===========================================================================*/
void ui_indoor_init(void)
{
    /* 样式表必须先就绪：先主题，后控件 */
    ui_theme_init(UI_THEME_LIGHT);

    scr = lv_screen_active();
    lv_obj_add_style(scr, &st_screen, 0);

    /*---------------- 1. 状态栏 ----------------*/
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 320, 32);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_add_style(bar, &st_bar, 0);

    time_label = lv_label_create(bar);
    lv_obj_set_pos(time_label, 0, 0);
    lv_obj_add_style(time_label, &st_bar_text, 0);
    lv_label_set_text(time_label, "00:00");

    /* 主题切换按钮（右上角）：透明按钮，只承载一个图标 */
    lv_obj_t *theme_btn = lv_button_create(bar);
    lv_obj_set_size(theme_btn, 32, 24);
    lv_obj_align(theme_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_style(theme_btn, &st_bar, 0);
    lv_obj_set_style_bg_opa(theme_btn, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(theme_btn, on_theme_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *theme_icon = lv_label_create(theme_btn);
    lv_obj_add_style(theme_icon, &st_bar_text, 0);
    lv_label_set_text(theme_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(theme_icon);

    /* 系统图标：WiFi + 电量 */
    lv_obj_t *icons = lv_label_create(bar);
    lv_obj_align(icons, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_add_style(icons, &st_bar_text, 0);
    lv_label_set_text(icons, LV_SYMBOL_WIFI "  " LV_SYMBOL_BATTERY_FULL);

    /*---------------- 2. 视频占位区 ----------------*/
    video_area = lv_obj_create(scr);
    lv_obj_set_size(video_area, 296, 220);
    lv_obj_set_pos(video_area, 12, 44);
    lv_obj_add_style(video_area, &st_surface, 0);

    video_hint = lv_label_create(video_area);
    lv_obj_add_style(video_hint, &st_text_muted, 0);
    lv_label_set_text(video_hint, LV_SYMBOL_VIDEO "  Door Camera");
    lv_obj_center(video_hint);

    call_state_label = lv_label_create(scr);
    lv_obj_set_pos(call_state_label, 12, 272);
    lv_obj_add_style(call_state_label, &st_text_muted, 0);
    lv_label_set_text(call_state_label, "Door Station 101  ·  Online");

    /*---------------- 3. 底部常驻操作区 ----------------*/
    make_button(scr, 12, 400, 140, 64, &st_btn_danger,
                LV_SYMBOL_CLOSE "  End", on_reject_clicked);
    make_button(scr, 168, 400, 140, 64, &st_btn_ok,
                LV_SYMBOL_CALL "  Answer", on_answer_clicked);

    /*---------------- 4. 来电弹窗 ----------------*/
    call_popup = lv_obj_create(scr);
    lv_obj_set_size(call_popup, 280, 150);
    lv_obj_set_pos(call_popup, 20, -150);
    lv_obj_add_style(call_popup, &st_card, 0);
    lv_obj_set_hidden(call_popup, true);

    lv_obj_t *caller = lv_label_create(call_popup);
    lv_obj_set_pos(caller, 0, 0);
    lv_obj_add_style(caller, &st_title, 0);
    lv_label_set_text(caller, LV_SYMBOL_BELL "  Door Station 101");

    lv_obj_t *sub = lv_label_create(call_popup);
    lv_obj_set_pos(sub, 0, 26);
    lv_obj_add_style(sub, &st_text_muted, 0);
    lv_label_set_text(sub, "Incoming video call...");

    make_button(call_popup, 0, 74, 120, 52, &st_btn_ok,
                LV_SYMBOL_CALL "  Answer", on_answer_clicked);
    make_button(call_popup, 136, 74, 120, 52, &st_btn_danger,
                LV_SYMBOL_CLOSE "  Reject", on_reject_clicked);

    /*---------------- 5. 周期定时器 ----------------*/
    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(show_call_popup, 2000, NULL);
}
