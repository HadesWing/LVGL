/**
 * @file ui_category.c
 * @brief 二级页面：某个设备分类下的设备列表（含房间筛选）
 *
 * 示例：首页点"灯光" → 本页显示 客厅/卧室/全宅 三个页签 + 该分类的设备卡片
 *       点卡片 → 三级设备详情页（开关 + 亮度滑条）
 */
#include "ui_category.h"
#include "ui_common.h"
#include "ui_nav.h"
#include "ui_device.h"
#include "ui_theme.h"
#include "app_devices.h"
#include "app_events.h"
#include "lvgl.h"

#include <stdio.h>

#define CARD_MAX 16
#define CARD_W   97
#define CARD_H   112

static lv_obj_t *s_grid;
static uint8_t   s_type;
static uint8_t   s_room = ROOM_ALL;      /* 分类页默认看全宅，再按房间筛 */

static lv_obj_t *s_card[CARD_MAX];
static lv_obj_t *s_state[CARD_MAX];
static lv_obj_t *s_icon[CARD_MAX];

static lv_style_t s_card_on;
static lv_style_t s_card_off;
static bool       s_styles_ready;

static void styles_init(void)
{
    if (s_styles_ready) return;
    lv_style_init(&s_card_on);
    lv_style_set_border_color(&s_card_on, lv_palette_main(LV_PALETTE_LIGHT_BLUE));
    lv_style_set_border_width(&s_card_on, 2);

    lv_style_init(&s_card_off);
    lv_style_set_border_color(&s_card_off, lv_palette_main(LV_PALETTE_GREY));
    lv_style_set_border_width(&s_card_off, 1);
    s_styles_ready = true;
}

/*---------------------------------------------------------------------------
 * 卡片
 *---------------------------------------------------------------------------*/
static void card_click_cb(lv_event_t *e)
{
    ui_device_open((int)(intptr_t)lv_event_get_user_data(e));   /* 三级：设备详情 */
}

static void refresh_card(int idx)
{
    const device_t *d = device_get(idx);
    if (!d || !s_card[idx]) return;

    lv_label_set_text(s_state[idx], device_state_text(d));
    lv_obj_remove_style(s_card[idx], &s_card_on, 0);
    lv_obj_remove_style(s_card[idx], &s_card_off, 0);
    lv_obj_add_style(s_card[idx], d->on ? &s_card_on : &s_card_off, 0);
    lv_obj_set_style_text_color(s_icon[idx],
                                d->on ? lv_palette_main(LV_PALETTE_LIGHT_BLUE)
                                      : lv_palette_main(LV_PALETTE_GREY), 0);
}

static void on_device_changed(const app_event_t *e, void *user)
{
    (void)user;
    refresh_card((int)e->arg0);
}

static void create_card(lv_obj_t *parent, int idx)
{
    const device_t *d = device_get(idx);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_add_style(card, &s_card_off, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_clickable(card, true);
    lv_obj_add_event_cb(card, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, device_type_icon(d->type));

    lv_obj_t *name = lv_label_create(card);
    lv_obj_add_style(name, &st_text_primary, 0);
    lv_obj_set_style_text_font(name, FONT_CJK, 0);
    lv_label_set_text(name, d->name);

    lv_obj_t *state = lv_label_create(card);
    lv_obj_add_style(state, &st_text_muted, 0);
    lv_obj_set_style_text_font(state, FONT_CJK, 0);

    lv_obj_t *arrow = lv_label_create(card);       /* 暗示可进详情 */
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_add_style(arrow, &st_text_muted, 0);

    s_card[idx] = card;
    s_state[idx] = state;
    s_icon[idx] = icon;
    refresh_card(idx);
}

/*---------------------------------------------------------------------------
 * 列表
 *---------------------------------------------------------------------------*/
static void rebuild_grid(void)
{
    lv_obj_clean(s_grid);
    for (int i = 0; i < CARD_MAX; i++) { s_card[i] = NULL; s_state[i] = NULL; s_icon[i] = NULL; }

    for (int i = 0; i < device_count(); i++) {
        const device_t *d = device_get(i);
        if (d->type != s_type) continue;
        if (s_room != ROOM_ALL && d->room != s_room) continue;
        create_card(s_grid, i);
    }
}

static void on_room_change(uint8_t room, void *user)
{
    (void)user;
    s_room = room;
    rebuild_grid();
}

static void on_page_delete(lv_event_t *e)
{
    (void)e;
    /* 页面销毁必须退订：否则事件回调会摸到已释放的控件（野指针崩溃） */
    app_events_unsubscribe(EV_DEVICE_CHANGED, on_device_changed);
    for (int i = 0; i < CARD_MAX; i++) { s_card[i] = NULL; s_state[i] = NULL; s_icon[i] = NULL; }
    s_grid = NULL;
}

/*---------------------------------------------------------------------------
 * 页面构建
 *---------------------------------------------------------------------------*/
static void build_category(lv_obj_t *scr, void *arg)
{
    s_type = (uint8_t)(intptr_t)arg;
    styles_init();

    ui_page_create(scr);
    lv_obj_add_event_cb(scr, on_page_delete, LV_EVENT_DELETE, NULL);
    ui_topbar_create(scr, device_type_name(s_type));
    ui_room_tabs_create(scr, s_room, on_room_change, NULL);

    s_grid = lv_obj_create(scr);
    lv_obj_add_style(s_grid, &st_screen, 0);
    lv_obj_set_size(s_grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_grid, 1);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(s_grid, 8, 0);
    lv_obj_set_style_pad_column(s_grid, 6, 0);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    rebuild_grid();

    /* 每次进分类页订阅一次：事件回调里按设备下标过滤，不在本页的自然跳过 */
    app_events_subscribe(EV_DEVICE_CHANGED, on_device_changed, NULL);
}

void ui_category_open(uint8_t type)
{
    ui_nav_push(build_category, (void *)(intptr_t)type);
}
