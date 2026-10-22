/**
 * @file ui_common.h
 * @brief 页面公共件：CJK 字体、顶部返回栏、房间页签
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl.h"
#include <stdint.h>

/* 中文字体：lv_font_conv 生成的**按需子集**（scripts/gen_font.py 扫描源码文案生成）
 *
 * 为什么不用 lv_font_source_han_sans_sc_16_cjk：内置字库只收了约 1500 常用字，
 * "灯/调/风/插/线/备"这些界面必用字缺席 → 运行时显示成豆腐块（方框）。
 * 子集字库 = 只包含项目真实用到的字，体积可控且不会缺字。
 *
 * 体积账（面试可讲）：4bpp 下每字 ≈ 字宽×字高/2 字节
 *   16px → 128 B/字，24px → 288 B/字；本工程 295 字 ≈ 37 KB + 83 KB ≈ 120 KB。 */
LV_FONT_DECLARE(ui_font_cjk_16)
LV_FONT_DECLARE(ui_font_cjk_24)
#define FONT_CJK     (&ui_font_cjk_16)   /* 正文/控件文字 */
#define FONT_CJK_LG  (&ui_font_cjk_24)   /* 页面标题/大号状态 */

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
