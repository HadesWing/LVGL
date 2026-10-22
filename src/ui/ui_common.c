/**
 * @file ui_common.c
 * @brief 页面公共件实现
 */
#include "ui_common.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "oem.h"

/*---------------------------------------------------------------------------
 * 顶部返回栏
 *---------------------------------------------------------------------------*/
static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_pop();
}

lv_obj_t *ui_topbar_create(lv_obj_t *scr, const char *title)
{
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_add_style(bar, &st_bar, 0);
    lv_obj_set_size(bar, lv_pct(100), oem()->layout.title_h);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(bar, 10, 0);
    lv_obj_set_style_pad_column(bar, 8, 0);
    lv_obj_set_scrollable(bar, false);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 40, 30);
    lv_obj_add_style(back, &st_btn_common, 0);
    lv_obj_add_style(back, &st_btn_pressed, LV_STATE_PRESSED);
    lv_obj_add_event_cb(back, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_add_style(back_icon, &st_btn_label, 0);
    lv_obj_center(back_icon);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_obj_add_style(lbl, &st_bar_text, 0);
    lv_obj_set_style_text_font(lbl, FONT_CJK, 0);
    lv_label_set_text(lbl, title ? title : "");
    lv_obj_set_flex_grow(lbl, 1);

    return bar;
}

/*---------------------------------------------------------------------------
 * 页面容器
 *---------------------------------------------------------------------------*/
lv_obj_t *ui_page_create(lv_obj_t *scr)
{
    lv_obj_add_style(scr, &st_screen, 0);
    lv_obj_set_size(scr, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_pad_row(scr, 0, 0);
    lv_obj_set_scrollable(scr, false);
    return scr;
}

/*---------------------------------------------------------------------------
 * 房间页签
 *---------------------------------------------------------------------------*/
static lv_style_t s_st_tab_checked;
static bool       s_tab_style_ready;

typedef struct {
    ui_room_change_cb_t cb;
    void               *user;
} room_ctx_t;

/* 同一时刻只有一个房间页签行（每个页面各自持有），静态 ctx 足够；
   若将来同屏多行，改成挂在 parent 的 user_data 上即可。 */
static room_ctx_t s_room_ctx;

static void room_tab_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    uint8_t room = (uint8_t)(intptr_t)lv_event_get_user_data(e);

    lv_obj_t *parent = lv_obj_get_parent(btn);
    uint32_t n = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_remove_state(lv_obj_get_child(parent, i), LV_STATE_CHECKED);
    }
    lv_obj_add_state(btn, LV_STATE_CHECKED);

    if (s_room_ctx.cb) s_room_ctx.cb(room, s_room_ctx.user);
}

lv_obj_t *ui_room_tabs_create(lv_obj_t *parent, uint8_t current,
                              ui_room_change_cb_t on_change, void *user)
{
    if (!s_tab_style_ready) {
        lv_style_init(&s_st_tab_checked);
        lv_style_set_border_color(&s_st_tab_checked, lv_palette_main(LV_PALETTE_LIGHT_BLUE));
        lv_style_set_border_width(&s_st_tab_checked, 2);
        s_tab_style_ready = true;
    }
    s_room_ctx.cb   = on_change;
    s_room_ctx.user = user;

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 38);
    lv_obj_add_style(row, &st_screen, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollable(row, false);

    static const struct { oem_text_id_t txt; uint8_t room; } defs[] = {
        { OEM_TXT_ROOM_0,   0        },
        { OEM_TXT_ROOM_1,   1        },
        { OEM_TXT_ROOM_ALL, ROOM_ALL },
    };
    for (unsigned i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_set_size(btn, 80, 28);
        lv_obj_add_style(btn, &st_btn_common, 0);
        lv_obj_add_style(btn, &st_btn_pressed, LV_STATE_PRESSED);
        lv_obj_add_style(btn, &s_st_tab_checked, LV_STATE_CHECKED);
        lv_obj_add_event_cb(btn, room_tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)defs[i].room);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_add_style(lbl, &st_btn_label, 0);
        lv_obj_set_style_text_font(lbl, FONT_CJK, 0);
        lv_label_set_text(lbl, oem_text(defs[i].txt));
        lv_obj_center(lbl);

        if (defs[i].room == current) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }
    return row;
}
