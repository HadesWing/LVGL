/**
 * @file ui_home.c
 * @brief 中控台首页 —— 设备分类入口（一级）+ 状态栏 + 场景条（M1.5）
 *
 * 【三级信息架构】
 *   一级 首页：设备分类（灯光/窗帘/空调/新风/插座/摄像头）
 *   二级 分类页：房间筛选（客厅/卧室/全宅）+ 该分类下的设备卡片
 *   三级 设备页：开关 + 参数滑条（亮度/温度/开度）
 *   —— 卡片点击 → ui_nav_push，返回按钮 → ui_nav_pop，现场由页面栈保存。
 */
#include "ui_home.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_category.h"
#include "ui_device.h"
#include "ui_theme.h"
#include "app_devices.h"
#include "app_events.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BAR_H    34
#define SCENE_H  56

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_sip_lbl;
static lv_obj_t *s_voice_lbl;

/*---------------------------------------------------------------------------
 * 事件（主线程）
 *---------------------------------------------------------------------------*/
static void on_voice_text(const app_event_t *e, void *user)
{
    (void)user;
    const char *txt = (const char *)e->ptr;
    lv_label_set_text(s_voice_lbl, txt ? txt : "");
}

static void on_sip_state(const app_event_t *e, void *user)
{
    (void)user;
    switch (e->arg0) {
    case 1:  lv_label_set_text(s_sip_lbl, "SIP 在线");  break;
    case 2:  lv_label_set_text(s_sip_lbl, "SIP 来电!"); break;
    default: lv_label_set_text(s_sip_lbl, "SIP 离线");  break;
    }
}

/*---------------------------------------------------------------------------
 * 分类格子
 *---------------------------------------------------------------------------*/
typedef struct {
    const char *name;
    uint8_t     type;    /* device_type_t */
} category_t;

static const category_t s_categories[] = {
    { "灯光",   DEV_LIGHT   },
    { "窗帘",   DEV_CURTAIN },
    { "空调",   DEV_AC      },
    { "新风",   DEV_FRESH   },
    { "插座",   DEV_PLUG    },
    { "摄像头", DEV_CAM     },
};

static void category_click_cb(lv_event_t *e)
{
    int type = (int)(intptr_t)lv_event_get_user_data(e);
    ui_category_open(type);              /* 二级页面（压栈） */
}

/*---------------------------------------------------------------------------
 * 场景条
 *---------------------------------------------------------------------------*/
static void scene_click_cb(lv_event_t *e)
{
    device_run_scene((int)(intptr_t)lv_event_get_user_data(e));
}

/*---------------------------------------------------------------------------
 * 时钟
 *---------------------------------------------------------------------------*/
static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt->tm_hour, lt->tm_min);
    if (s_time_lbl) lv_label_set_text(s_time_lbl, buf);
}

/*---------------------------------------------------------------------------
 * 自测：LVGL_UI_TEST=1 时自动走一遍 首页→分类→详情→返回→返回
 *   用途：无人值守验证页面栈/转场动画，以及 pop 之后有没有野指针
 *---------------------------------------------------------------------------*/
static void uitest_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int step;
    int s = step++ % 8;
    switch (s) {
    case 0: printf("[uitest] push category: light\n");  ui_category_open(DEV_LIGHT); break;
    case 1: printf("[uitest] push device: light_living\n"); ui_device_open(device_find("light_living")); break;
    case 2: printf("[uitest] set brightness 35\n");
            { device_cmd_t c = { "light_living", CMD_SET, 35 }; device_execute(&c); } break;
    case 3: printf("[uitest] pop -> category: %d\n", ui_nav_depth()); ui_nav_pop(); break;
    case 4: printf("[uitest] pop -> home: %d\n", ui_nav_depth()); ui_nav_pop(); break;
    case 5: printf("[uitest] scene home\n"); device_run_scene(0); break;
    case 6: printf("[uitest] push category: curtain\n"); ui_category_open(DEV_CURTAIN); break;
    case 7: printf("[uitest] pop -> home: %d\n", ui_nav_depth()); ui_nav_pop(); break;
    }
    fflush(stdout);
}

/*---------------------------------------------------------------------------
 * 构建
 *---------------------------------------------------------------------------*/
void ui_home_init(void)
{
    ui_nav_init();
    lv_obj_t *scr = lv_screen_active();
    ui_page_create(scr);

    /* ---------- 状态栏 ---------- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_add_style(bar, &st_bar, 0);
    lv_obj_set_size(bar, lv_pct(100), BAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 10, 0);
    lv_obj_set_scrollable(bar, false);

    s_time_lbl = lv_label_create(bar);
    lv_obj_add_style(s_time_lbl, &st_bar_text, 0);
    lv_obj_set_style_text_font(s_time_lbl, FONT_CJK, 0);
    lv_label_set_text(s_time_lbl, "--:--");

    s_voice_lbl = lv_label_create(bar);
    lv_obj_add_style(s_voice_lbl, &st_text_muted, 0);
    lv_obj_set_style_text_font(s_voice_lbl, FONT_CJK, 0);
    lv_obj_set_flex_grow(s_voice_lbl, 1);
    lv_label_set_long_mode(s_voice_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_voice_lbl, "");

    s_sip_lbl = lv_label_create(bar);
    lv_obj_add_style(s_sip_lbl, &st_bar_text, 0);
    lv_obj_set_style_text_font(s_sip_lbl, FONT_CJK, 0);
    lv_label_set_text(s_sip_lbl, "SIP 离线");

    /* ---------- 标题 ---------- */
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_add_style(title, &st_text_primary, 0);
    lv_obj_set_style_text_font(title, FONT_CJK, 0);
    lv_label_set_text(title, "我的设备");
    lv_obj_set_style_pad_all(title, 10, 0);

    /* ---------- 分类九宫格 ---------- */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_add_style(grid, &st_screen, 0);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_scrollable(grid, false);

    for (unsigned i = 0; i < sizeof(s_categories) / sizeof(s_categories[0]); i++) {
        lv_obj_t *cell = lv_button_create(grid);
        lv_obj_set_size(cell, 140, 88);
        lv_obj_add_style(cell, &st_card, 0);
        lv_obj_add_style(cell, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 4, 0);
        lv_obj_add_event_cb(cell, category_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_categories[i].type);

        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, device_type_icon(s_categories[i].type));
        lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);

        lv_obj_t *name = lv_label_create(cell);
        lv_obj_add_style(name, &st_text_primary, 0);
        lv_obj_set_style_text_font(name, FONT_CJK, 0);
        lv_label_set_text(name, s_categories[i].name);
    }

    /* ---------- 场景条 ---------- */
    lv_obj_t *scenes = lv_obj_create(scr);
    lv_obj_add_style(scenes, &st_bar, 0);
    lv_obj_set_size(scenes, lv_pct(100), SCENE_H);
    lv_obj_set_flex_flow(scenes, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(scenes, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(scenes, false);

    for (int s = 0; s < device_scene_count(); s++) {
        lv_obj_t *btn = lv_button_create(scenes);
        lv_obj_set_size(btn, 64, 38);
        lv_obj_add_style(btn, &st_btn_common, 0);
        lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, scene_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)s);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_add_style(lbl, &st_btn_label, 0);
        lv_obj_set_style_text_font(lbl, FONT_CJK, 0);
        lv_label_set_text(lbl, device_scene_name(s));
        lv_obj_center(lbl);
    }

    /* ---------- 事件订阅 + 时钟 ---------- */
    app_events_subscribe(EV_VOICE_TEXT, on_voice_text, NULL);
    app_events_subscribe(EV_SIP_STATE,  on_sip_state,  NULL);
    lv_timer_create(clock_timer_cb, 10000, NULL);
    clock_timer_cb(NULL);

    if (getenv("LVGL_UI_TEST")) {
        printf("[uitest] auto navigation enabled\n");
        lv_timer_create(uitest_timer_cb, 1200, NULL);
    }
}
