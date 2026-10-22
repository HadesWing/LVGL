/**
 * @file ui_common.h
 * @brief 页面公共件：CJK 字体、顶部返回栏、房间页签
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl.h"
#include <stdint.h>

/* 中文文本统一用 LVGL 内置思源黑体 16（M4 换 lv_font_conv 子集后只改这一处） */
LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk)
#define FONT_CJK (&lv_font_source_han_sans_sc_16_cjk)

/* 顶部栏（返回按钮 + 标题），返回按钮自动出栈 */
lv_obj_t *ui_topbar_create(lv_obj_t *scr, const char *title);

/* 标准页面容器：铺满 + 纵向 flex + 无滚动（子区域自己决定滚不滚） */
lv_obj_t *ui_page_create(lv_obj_t *scr);

/* 房间页签行：客厅/卧室/全宅；切换时回调 on_change(room, user) */
typedef void (*ui_room_change_cb_t)(uint8_t room, void *user);
lv_obj_t *ui_room_tabs_create(lv_obj_t *parent, uint8_t current,
                              ui_room_change_cb_t on_change, void *user);

#define ROOM_ALL 255

#endif /* UI_COMMON_H */
