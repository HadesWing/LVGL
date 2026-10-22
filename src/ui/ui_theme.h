/**
 * @file ui_theme.h
 * @brief 主题与样式系统 —— LVGL 生产代码的标准做法
 *
 * 【为什么要有这一层】
 *   1. 内联写法 `lv_obj_set_style_bg_color(obj, color, 0)` 会把颜色/圆角/字号散落在
 *      每个控件创建处，改一次主题要改几十处。生产代码一个界面动辄上百控件，
 *      必须把"长什么样"抽成命名样式，控件只说"我要哪种样式"。
 *   2. 样式对象是共享的（一份 lv_style_t 可被任意多控件引用），改样式属性 +
 *      lv_obj_report_style_change() 就能全屏生效 —— 这是主题/夜间模式的标准实现。
 *   3. 状态样式（LV_STATE_PRESSED / CHECKED / DISABLED）只能靠样式表表达：
 *      按下变深、选中变色、置灰不可点，都是"同一控件 + 不同状态"。
 *
 * 【跨平台】
 *   本文件与 ui_indoor.c 一样，只依赖 lvgl.h，不含任何 Windows/Linux 专有头文件，
 *   Windows(MSVC/MinGW)、Linux(SDL/fbdev)、RTOS 上都能直接编译。
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

typedef enum {
    UI_THEME_LIGHT = 0,
    UI_THEME_DARK,
} ui_theme_t;

/*---------------------------------------------------------------------------
 * 命名样式（全局唯一实例，初始化一次，之后到处 lv_obj_add_style）
 * 命名约定：st_ = style；语义化命名，不写颜色（颜色由主题决定）
 *---------------------------------------------------------------------------*/
extern lv_style_t st_screen;          /* 屏幕底色 */
extern lv_style_t st_bar;             /* 状态栏容器 */
extern lv_style_t st_bar_text;        /* 状态栏文字/图标 */
extern lv_style_t st_surface;         /* 卡片/视频区表面 */
extern lv_style_t st_surface_calling; /* 通话中表面（绿边框） */
extern lv_style_t st_card;            /* 弹窗卡片（带阴影） */
extern lv_style_t st_text_primary;    /* 主文字 */
extern lv_style_t st_text_muted;      /* 次要文字 */
extern lv_style_t st_title;           /* 弹窗标题（粗一点） */
extern lv_style_t st_btn_common;      /* 按钮公共部分（圆角/边框/内边距） */
extern lv_style_t st_btn_pressed;     /* 按钮按下态（下沉 2px，纯视觉反馈） */
extern lv_style_t st_btn_ok;          /* 语义色：接听/确认 */
extern lv_style_t st_btn_danger;      /* 语义色：拒接/危险 */
extern lv_style_t st_btn_label;       /* 按钮文字 */

/* 初始化全部样式并应用指定主题（界面创建前调用一次） */
void ui_theme_init(ui_theme_t theme);

/* 切换明/暗主题（会触发全屏重绘，可绑到按钮事件上） */
void ui_theme_toggle(void);

ui_theme_t ui_theme_get(void);

#endif
