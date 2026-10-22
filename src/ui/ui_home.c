/**
 * @file ui_home.c
 * @brief 智能家居中控台主页 —— flex/grid 布局 + 事件驱动刷新（M1）
 *
 * 【这一页练什么（对应 JD）】
 *   - flex 纵向分栏（状态栏/页签/网格/场景条）+ 网格区 flex wrap：全程零绝对坐标；
 *   - 事件驱动：卡片点击只调 device_execute()，界面刷新靠 EV_DEVICE_CHANGED 事件
 *     回来改 —— UI 与业务单向数据流，这是可维护界面代码的分水岭；
 *   - 卡片状态可视化：开=高亮边框+彩色图标，关=暗色，一眼读状态；
 *   - 中文渲染：LVGL 内置思源黑体 16px（lv_conf 里 LV_FONT_SOURCE_HAN_SANS_SC_16_CJK），
 *     M4 再换 lv_font_conv 的按需子集（体积/字号档位优化）。
 *
 * 【线程纪律】本文件所有 LVGL 调用都在主线程：事件回调由 app_events 在主线程分发。
 */
#include "ui_home.h"
#include "ui_theme.h"
#include "app_devices.h"
#include "app_events.h"
#include "lvgl.h"

#include <stdio.h>
#include <time.h>

/*---------------------------------------------------------------------------
 * 常量
 *---------------------------------------------------------------------------*/
#define ROOM_ALL       255          /* 页签"全宅"的哨兵值 */
#define CARD_W         97
#define CARD_H         112
#define BAR_H          34
#define TAB_H          38
#define SCENE_H        56

/*---------------------------------------------------------------------------
 * 本页局部样式（颜色只在这里出现一次，主题切换仍走 ui_theme 的命名样式）
 *---------------------------------------------------------------------------*/
static lv_style_t st_card_on;    /* 设备开：亮色边框 */
static lv_style_t st_card_off;   /* 设备关：暗色边框 */
static lv_style_t st_tab_checked;/* 页签选中态 */

static void local_styles_init(void)
{
    lv_style_init(&st_card_on);
    lv_style_set_border_color(&st_card_on, lv_palette_main(LV_PALETTE_LIGHT_BLUE));
    lv_style_set_border_width(&st_card_on, 2);
    lv_style_set_bg_opa(&st_card_on, LV_OPA_COVER);
    lv_style_set_bg_color(&st_card_on, lv_color_mix(lv_palette_main(LV_PALETTE_LIGHT_BLUE),
                                                    lv_color_black(), LV_OPA_10));

    lv_style_init(&st_card_off);
    lv_style_set_border_color(&st_card_off, lv_palette_main(LV_PALETTE_GREY));
    lv_style_set_border_width(&st_card_off, 1);

    lv_style_init(&st_tab_checked);
    lv_style_set_border_color(&st_tab_checked, lv_palette_main(LV_PALETTE_LIGHT_BLUE));
    lv_style_set_border_width(&st_tab_checked, 2);
}

/* CJK 内置字体（lv_conf.h: LV_FONT_SOURCE_HAN_SANS_SC_16_CJK=1） */
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk)
#define FONT_CJK (&lv_font_source_han_sans_sc_16_cjk)

/*---------------------------------------------------------------------------
 * 页面状态
 *---------------------------------------------------------------------------*/
static lv_obj_t *s_time_lbl;        /* 状态栏时钟 */
static lv_obj_t *s_sip_lbl;         /* SIP 状态 */
static lv_obj_t *s_voice_lbl;       /* 语音字幕区（M2 换浮层） */
static lv_obj_t *s_grid;            /* 设备网格容器 */

#define CARD_MAX 16
static lv_obj_t *s_card[CARD_MAX];      /* 设备下标 → 卡片对象（不在当前页签时为 NULL） */
static lv_obj_t *s_state_lbl[CARD_MAX]; /* 设备下标 → 状态文本 */
static lv_obj_t *s_icon_lbl[CARD_MAX];  /* 设备下标 → 图标（开/关变色用） */
static uint8_t   s_room = ROOM_LIVING;  /* 当前页签 */

/*---------------------------------------------------------------------------
 * 前置声明
 *---------------------------------------------------------------------------*/
static void build_grid(void);
static void refresh_card(int idx);

/*---------------------------------------------------------------------------
 * 事件处理（都在主线程执行）
 *---------------------------------------------------------------------------*/
static void on_device_changed(const app_event_t *e, void *user)
{
    (void)user;
    refresh_card((int)e->arg0);
}

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
 * 卡片
 *---------------------------------------------------------------------------*/
static void card_click_cb(lv_event_t *e)
{
    /* UI 只发动作，不直接改界面 —— 状态回流走 EV_DEVICE_CHANGED */
    device_toggle_by_index((int)(intptr_t)lv_event_get_user_data(e));
}

static void refresh_card(int idx)
{
    const device_t *d = device_get(idx);
    if (!d || !s_card[idx]) return;

    lv_label_set_text(s_state_lbl[idx], device_state_text(d));

    /* 开/关视觉：换边框样式 + 图标颜色 */
    lv_obj_remove_style(s_card[idx], &st_card_on, 0);
    lv_obj_remove_style(s_card[idx], &st_card_off, 0);
    lv_obj_add_style(s_card[idx], d->on ? &st_card_on : &st_card_off, 0);
    lv_obj_set_style_text_color(s_icon_lbl[idx],
                                d->on ? lv_palette_main(LV_PALETTE_LIGHT_BLUE)
                                      : lv_palette_main(LV_PALETTE_GREY), 0);
}

static lv_obj_t *create_card(lv_obj_t *parent, int idx)
{
    const device_t *d = device_get(idx);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_add_style(card, &st_card_off, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(card, true);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    /* 图标：开=亮蓝，关=灰（Montserrat 符号字体，与中文文本分开渲染） */
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, device_type_icon(d->type));
    s_icon_lbl[idx] = icon;

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, d->name);
    lv_obj_add_style(name, &st_text_primary, 0);
    lv_obj_set_style_text_font(name, FONT_CJK, 0);

    lv_obj_t *state = lv_label_create(card);
    lv_obj_add_style(state, &st_text_muted, 0);
    lv_obj_set_style_text_font(state, FONT_CJK, 0);

    s_card[idx]      = card;
    s_state_lbl[idx] = state;
    refresh_card(idx);          /* 统一从 device 状态渲染，避免两处维护 */
    return card;
}

/*---------------------------------------------------------------------------
 * 网格构建 / 页签切换
 *---------------------------------------------------------------------------*/
static void build_grid(void)
{
    lv_obj_clean(s_grid);                       /* 整体重建：页签切换是低频操作 */
    for (int i = 0; i < CARD_MAX; i++) {
        s_card[i] = NULL; s_state_lbl[i] = NULL; s_icon_lbl[i] = NULL;
    }

    for (int i = 0; i < device_count(); i++) {
        const device_t *d = device_get(i);
        if (s_room != ROOM_ALL && d->room != s_room) continue;
        create_card(s_grid, i);
    }
}

static void tab_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    int room = (int)(intptr_t)lv_event_get_user_data(e);

    /* 手风琴选中态：同排兄弟全取消，自己勾上 */
    lv_obj_t *parent = lv_obj_get_parent(btn);
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_remove_state(lv_obj_get_child(parent, i), LV_STATE_CHECKED);
    }
    lv_obj_add_state(btn, LV_STATE_CHECKED);

    s_room = (uint8_t)room;
    build_grid();
}

/*---------------------------------------------------------------------------
 * 状态栏时钟
 *---------------------------------------------------------------------------*/
static void scene_click_cb(lv_event_t *e)
{
    /* 场景 = 预置命令序列，设备层逐条执行并逐条发事件，UI 自动跟着刷新 */
    device_run_scene((int)(intptr_t)lv_event_get_user_data(e));
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", lt->tm_hour, lt->tm_min);
    lv_label_set_text(s_time_lbl, buf);
}

/*---------------------------------------------------------------------------
 * 页面构建
 *---------------------------------------------------------------------------*/
void ui_home_init(void)
{
    local_styles_init();

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_add_style(root, &st_screen, 0);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_set_scrollable(root, false);

    /* ---------- 状态栏 ---------- */
    lv_obj_t *bar = lv_obj_create(root);
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

    s_voice_lbl = lv_label_create(bar);          /* 语音字幕区（M2 换浮层） */
    lv_obj_add_style(s_voice_lbl, &st_text_muted, 0);
    lv_obj_set_style_text_font(s_voice_lbl, FONT_CJK, 0);
    lv_obj_set_flex_grow(s_voice_lbl, 1);
    lv_label_set_long_mode(s_voice_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_voice_lbl, "");

    s_sip_lbl = lv_label_create(bar);
    lv_obj_add_style(s_sip_lbl, &st_bar_text, 0);
    lv_obj_set_style_text_font(s_sip_lbl, FONT_CJK, 0);
    lv_label_set_text(s_sip_lbl, "SIP 离线");

    /* ---------- 房间页签 ---------- */
    lv_obj_t *tabs = lv_obj_create(root);
    lv_obj_add_style(tabs, &st_screen, 0);
    lv_obj_set_size(tabs, lv_pct(100), TAB_H);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tabs, 8, 0);
    lv_obj_set_scrollable(tabs, false);

    static const struct { const char *name; int room; } tab_defs[] = {
        { "客厅", ROOM_LIVING }, { "卧室", ROOM_BED }, { "全宅", ROOM_ALL },
    };
    for (unsigned i = 0; i < sizeof(tab_defs) / sizeof(tab_defs[0]); i++) {
        lv_obj_t *btn = lv_button_create(tabs);
        lv_obj_set_size(btn, 80, 28);
        lv_obj_add_style(btn, &st_btn_common, 0);
        lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(btn, &st_tab_checked, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)tab_defs[i].room);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_add_style(lbl, &st_btn_label, 0);
        lv_obj_set_style_text_font(lbl, FONT_CJK, 0);
        lv_label_set_text(lbl, tab_defs[i].name);

        if (tab_defs[i].room == s_room) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }

    /* ---------- 设备网格 ---------- */
    s_grid = lv_obj_create(root);
    lv_obj_add_style(s_grid, &st_screen, 0);
    lv_obj_set_size(s_grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_grid, 8, 0);
    lv_obj_set_style_pad_column(s_grid, 6, 0);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);   /* 设备多时纵向滚动 */
    build_grid();

    /* ---------- 场景条 ---------- */
    lv_obj_t *scenes = lv_obj_create(root);
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
    }

    /* ---------- 订阅事件 + 时钟 ---------- */
    app_events_subscribe(EV_DEVICE_CHANGED, on_device_changed, NULL);
    app_events_subscribe(EV_VOICE_TEXT,     on_voice_text,     NULL);
    app_events_subscribe(EV_SIP_STATE,      on_sip_state,      NULL);
    lv_timer_create(clock_timer_cb, 10000, NULL);
    clock_timer_cb(NULL);
}
