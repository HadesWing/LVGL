/**
 * @file ui_nav.h
 * @brief 极简页面栈 —— 二级/三级页面与转场动画
 *
 * 【为什么不用 lv_screen_load() 硬切】
 *   真实产品必须是"压栈/出栈"语义：从设备详情返回要回到"灯光 → 客厅"那个现场，
 *   而不是回到首页。栈里存的是 screen 对象指针，返回时直接 load 回来即可
 *   （LVGL 的 screen 切换本身不做状态保存，栈由应用层维护）。
 *
 * 【动画】入栈 MOVE_LEFT、出栈 MOVE_RIGHT，200ms —— 与手机端心智一致。
 */
#ifndef UI_NAV_H
#define UI_NAV_H

#include "lvgl.h"
#include <stdbool.h>

/* 页面构建回调：往给定的 screen 上建内容，arg 由调用方传入 */
typedef void (*ui_page_build_cb_t)(lv_obj_t *scr, void *arg);

/* 初始化栈（在主页构建前调用一次） */
void ui_nav_init(void);

/* 压栈：新建 screen → 构建 → 左滑进入。超过深度上限返回 false */
bool ui_nav_push(ui_page_build_cb_t build, void *arg);

/* 出栈：右滑回到上一页（当前页自动销毁）。已在根页返回 false */
bool ui_nav_pop(void);

/* 当前栈深度（根页 = 1） */
int  ui_nav_depth(void);

#endif /* UI_NAV_H */
