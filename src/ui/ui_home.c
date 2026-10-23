/**
 * @file ui_home.c
 * @brief 中控台首页 —— 设备分类入口（一级）+ 状态栏 + 场景条
 *
 * 【三级信息架构】
 *   一级 首页：设备分类（灯光/窗帘/空调/新风/插座/摄像头）
 *   二级 分类页：房间筛选（客厅/卧室/全宅）+ 该分类下的设备卡片
 *   三级 设备页：开关 + 参数滑条（亮度/温度/开度）
 *   —— 卡片点击 → ui_nav_push，返回按钮 → ui_nav_pop，现场由页面栈保存。
 *
 * 【OEM 解耦（本文件的硬约束）】
 *   任何品牌相关的**文字、颜色、尺寸、结构**都不允许写字面量：
 *     文案 → oem_text(OEM_TXT_*)
 *     颜色 → 样式表（ui_theme 已接 OEM 色板）
 *     尺寸 → oem()->layout.*
 *     结构 → layout.home_style 选择 build_grid() / build_list()
 *   新增一个客户 = 新增一个 oem_xxx.c（纯数据），本文件零改动。
 */
#include "ui_home.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_category.h"
#include "ui_device.h"
#include "ui_dial.h"
#include "ui_theme.h"
#include "app_devices.h"
#include "app_events.h"
#include "oem.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    if (s_voice_lbl) lv_label_set_text(s_voice_lbl, txt ? txt : "");
}

static void on_sip_state(const app_event_t *e, void *user)
{
    (void)user;
    if (!s_sip_lbl) return;
    oem_text_id_t t;
    switch (e->arg0) {
    case 1:  t = OEM_TXT_SIP_ONLINE;   break;   /* SIP_ONLINE */
    case 2:  t = OEM_TXT_SIP_INCOMING; break;   /* SIP_INCOMING */
    case 3:  t = OEM_TXT_SIP_INCALL;   break;   /* SIP_INCALL */
    case 4:  t = OEM_TXT_SIP_CALLING;  break;   /* SIP_RINGING */
    default: t = OEM_TXT_SIP_OFFLINE;  break;
    }
    lv_label_set_text(s_sip_lbl, oem_text(t));
}

/* 点状态栏的 SIP 文字 = 打开拨号页（输入房号呼出 / 房号@IP 直呼） */
static void sip_lbl_click_cb(lv_event_t *e)
{
    (void)e;
    ui_dial_open();
}

/*---------------------------------------------------------------------------
 * 分类格子：类型 → OEM 文案
 *---------------------------------------------------------------------------*/
typedef struct {
    oem_text_id_t txt;
    uint8_t       type;    /* device_type_t */
} category_t;

static const category_t s_categories[] = {
    { OEM_TXT_CAT_LIGHT,   DEV_LIGHT   },
    { OEM_TXT_CAT_CURTAIN, DEV_CURTAIN },
    { OEM_TXT_CAT_AC,      DEV_AC      },
    { OEM_TXT_CAT_FRESH,   DEV_FRESH   },
    { OEM_TXT_CAT_PLUG,    DEV_PLUG    },
    { OEM_TXT_CAT_CAM,     DEV_CAM     },
};
#define CAT_N (sizeof(s_categories) / sizeof(s_categories[0]))

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
    if (!s_time_lbl) return;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt->tm_hour, lt->tm_min);
    lv_label_set_text(s_time_lbl, buf);
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
 * 首页构建器 A：宫格（大屏 / 触摸）
 *---------------------------------------------------------------------------*/
static void build_grid(lv_obj_t *parent, const oem_layout_t *L)
{
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_add_style(grid, &st_screen, 0);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, L->pad, 0);
    lv_obj_set_style_pad_column(grid, L->gap, 0);
    lv_obj_set_style_pad_row(grid, L->gap, 0);
    lv_obj_set_scrollable(grid, false);

    for (unsigned i = 0; i < CAT_N; i++) {
        lv_obj_t *cell = lv_button_create(grid);
        lv_obj_set_size(cell, L->card_w, L->card_h);
        lv_obj_add_style(cell, &st_card, 0);
        lv_obj_add_style(cell, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 4, 0);
        lv_obj_add_event_cb(cell, category_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_categories[i].type);

        lv_obj_t *icon = lv_label_create(cell);
        lv_label_set_text(icon, device_type_icon(s_categories[i].type));
        lv_obj_set_style_text_color(icon, lv_color_hex(oem_color(OEM_COLOR_PRIMARY)), 0);

        lv_obj_t *name = lv_label_create(cell);
        lv_obj_add_style(name, &st_text_primary, 0);
        lv_obj_set_style_text_font(name, FONT_CJK, 0);
        lv_label_set_text(name, oem_text(s_categories[i].txt));
    }
}

/*---------------------------------------------------------------------------
 * 首页构建器 B：列表（小屏 / 旋钮或遥控操作）
 *   同一份数据、同一套事件，只是排布不同 —— 这就是"结构级 OEM 差异"的做法：
 *   加一个 build_xxx()，在 home_style 里多一个枚举值，其它代码不动。
 *---------------------------------------------------------------------------*/
static void build_list(lv_obj_t *parent, const oem_layout_t *L)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_add_style(list, &st_screen, 0);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(list, L->pad, 0);
    lv_obj_set_style_pad_row(list, L->gap, 0);
    lv_obj_set_scrollable(list, true);

    for (unsigned i = 0; i < CAT_N; i++) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, lv_pct(100), L->card_h);
        lv_obj_add_style(row, &st_card, 0);
        lv_obj_add_style(row, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_add_event_cb(row, category_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_categories[i].type);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, device_type_icon(s_categories[i].type));
        lv_obj_set_style_text_color(icon, lv_color_hex(oem_color(OEM_COLOR_PRIMARY)), 0);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_add_style(name, &st_text_primary, 0);
        lv_obj_set_style_text_font(name, FONT_CJK, 0);
        lv_label_set_text(name, oem_text(s_categories[i].txt));
        lv_obj_set_flex_grow(name, 1);

        lv_obj_t *arrow = lv_label_create(row);
        lv_obj_add_style(arrow, &st_text_muted, 0);
        lv_obj_set_style_text_font(arrow, FONT_CJK, 0);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    }
}

/*---------------------------------------------------------------------------
 * 构建
 *---------------------------------------------------------------------------*/
void ui_home_init(void)
{
    const oem_layout_t *L = &oem()->layout;

    ui_nav_init();
    lv_obj_t *scr = lv_screen_active();
    ui_page_create(scr);

    /* ---------- 状态栏 ---------- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_add_style(bar, &st_bar, 0);
    lv_obj_set_size(bar, lv_pct(100), L->bar_h);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 10, 0);
    lv_obj_set_scrollable(bar, false);

    if (L->show_clock) {
        s_time_lbl = lv_label_create(bar);
        lv_obj_add_style(s_time_lbl, &st_bar_text, 0);
        lv_obj_set_style_text_font(s_time_lbl, FONT_CJK, 0);
        lv_label_set_text(s_time_lbl, "--:--");
    }

    if (L->show_voice_bar) {
        s_voice_lbl = lv_label_create(bar);
        lv_obj_add_style(s_voice_lbl, &st_text_muted, 0);
        lv_obj_set_style_text_font(s_voice_lbl, FONT_CJK, 0);
        lv_obj_set_flex_grow(s_voice_lbl, 1);
        lv_label_set_long_mode(s_voice_lbl, LV_LABEL_LONG_DOT);
        lv_label_set_text(s_voice_lbl, "");
    } else {
        /* 没有语音条的客户：用垫片占位，保证 SIP 文字仍然靠右 */
        lv_obj_t *sp = lv_obj_create(bar);
        lv_obj_set_size(sp, lv_pct(100), 1);
        lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sp, 0, 0);
        lv_obj_set_flex_grow(sp, 1);
        lv_obj_set_scrollable(sp, false);
    }

    s_sip_lbl = lv_label_create(bar);
    lv_obj_add_style(s_sip_lbl, &st_bar_text, 0);
    lv_obj_set_style_text_font(s_sip_lbl, FONT_CJK, 0);
    lv_label_set_text(s_sip_lbl, oem_text(OEM_TXT_SIP_OFFLINE));
    lv_obj_set_style_pad_all(s_sip_lbl, 6, 0);          /* 扩大点击热区 */
    lv_obj_set_clickable(s_sip_lbl, true);
    lv_obj_add_event_cb(s_sip_lbl, sip_lbl_click_cb, LV_EVENT_CLICKED, NULL);

    /* ---------- 标题行（产品名 + 可选品牌名） ---------- */
    lv_obj_t *title_row = lv_obj_create(scr);
    lv_obj_add_style(title_row, &st_screen, 0);
    lv_obj_set_size(title_row, lv_pct(100), L->title_h);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(title_row, L->pad, 0);
    lv_obj_set_scrollable(title_row, false);

    lv_obj_t *title = lv_label_create(title_row);
    lv_obj_add_style(title, &st_title, 0);
    lv_obj_set_style_text_font(title, FONT_CJK_LG, 0);
    lv_label_set_text(title, oem_text(OEM_TXT_PRODUCT));
    lv_obj_set_flex_grow(title, 1);

    if (L->show_brand) {
        lv_obj_t *brand = lv_label_create(title_row);
        lv_obj_add_style(brand, &st_text_muted, 0);
        lv_obj_set_style_text_font(brand, FONT_CJK, 0);
        lv_label_set_text(brand, oem_text(OEM_TXT_BRAND));
    }

    /* ---------- 分类区：按 OEM 选择结构 ---------- */
    if (L->home_style == OEM_HOME_LIST) build_list(scr, L);
    else                                build_grid(scr, L);

    /* ---------- 场景条（可关） ---------- */
    if (L->show_scene_bar && L->scene_h > 0) {
        lv_obj_t *scenes = lv_obj_create(scr);
        lv_obj_add_style(scenes, &st_bar, 0);
        lv_obj_set_size(scenes, lv_pct(100), L->scene_h);
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
    }

    /* ---------- 事件订阅 + 时钟 ---------- */
    if (L->show_voice_bar) app_events_subscribe(EV_VOICE_TEXT, on_voice_text, NULL);
    app_events_subscribe(EV_SIP_STATE, on_sip_state, NULL);
    if (L->show_clock) {
        lv_timer_create(clock_timer_cb, 10000, NULL);
        clock_timer_cb(NULL);
    }

    if (getenv("LVGL_UI_TEST")) {
        printf("[uitest] auto navigation enabled\n");
        lv_timer_create(uitest_timer_cb, 1200, NULL);
    }
}
