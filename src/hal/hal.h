/**
 * @file hal.h
 * @brief 硬件抽象层（HAL）—— 平台差异的唯一出口
 *
 * 【分层原则（这是"一份 UI 代码跑遍所有平台"的关键）】
 *
 *   ui/  （纯 LVGL，零平台依赖）     ← 界面、样式、事件、动画
 *     ↑ 只调用 lv_* API
 *   main.c（应用入口 + 心跳循环）    ← lv_init / hal_init / ui_init / lv_timer_handler
 *     ↓ 只调用 hal_init()
 *   hal/ （平台相关，按需替换）      ← 显示 + 输入 + tick
 *     ├── hal_sdl.c      Windows / Linux 桌面模拟（SDL2，开发期用）
 *     └── hal_fbdev.c    Linux 真机（/dev/fb0 + /dev/input/eventX，上板用）
 *
 * 换平台 = 换一个 hal_xxx.c + CMake 里改一行，ui/ 与 main.c 一行不动。
 */
#ifndef LV_VSCODE_HAL_H
#define LV_VSCODE_HAL_H

#include "lvgl/lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化显示 + 输入设备，返回创建的 display 对象。
 * 实现由构建系统选择（CMake: -DLVGL_HAL=sdl|fbdev）。
 *
 * @param w  屏幕宽度（像素）
 * @param h  屏幕高度（像素）
 */
lv_display_t * hal_init(int32_t w, int32_t h);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_VSCODE_HAL_H*/
