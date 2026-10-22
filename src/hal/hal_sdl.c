/**
 * @file hal_sdl.c
 * @brief HAL 实现之一：SDL2 桌面窗口（Windows / Linux 通用）
 *
 * 为什么开发期用 SDL：
 *   - 一套代码在 Windows 和 Linux 桌面都能弹窗口，不用先有板子；
 *   - 键鼠直接映射成触摸/编码器，调试界面交互最快；
 *   - LVGL v9/v10 已内置 SDL 驱动（lv_sdl_window / lv_sdl_mouse / lv_sdl_keyboard），
 *     所以这里只是"组装"，不需要自己写 flush_cb —— 但要知道它内部干了什么：
 *     lv_sdl_window_create 内部就是 SDL_CreateWindow + 注册 flush_cb（把 LVGL 的
 *     draw buffer 贴到 SDL texture），跟我们上板时写 fbdev flush_cb 是同一个套路。
 *
 * 上板时换成 hal_fbdev.c，本文件在 Linux 上依然可用于调试。
 */
#include "hal.h"

void hal_sdl_init_impl(void) { /* 保留给未来的 SDL 专属初始化（如窗口图标） */ }

lv_display_t * hal_init(int32_t w, int32_t h)
{
    /* 输入设备组：鼠标 / 键盘 / 滚轮要归到同一个 group，
     * 这样编码器、焦点切换（lv_group）才能正常工作 */
    lv_group_set_default(lv_group_create());

    /* 1. 显示：一块窗口 + 内部 draw buffer */
    lv_display_t * disp = lv_sdl_window_create(w, h);

    /* 2. 指针输入（鼠标模拟触摸屏） */
    lv_indev_t * mouse = lv_sdl_mouse_create();
    lv_indev_set_group(mouse, lv_group_get_default());
    lv_indev_set_display(mouse, disp);

    lv_display_set_default(disp);

    /* 3. 鼠标光标：一张内置图片对象，挂到 indev 上 */
    LV_IMAGE_DECLARE(mouse_cursor_icon);
    lv_obj_t * cursor_obj = lv_image_create(lv_screen_active());
    lv_image_set_src(cursor_obj, &mouse_cursor_icon);
    lv_indev_set_cursor(mouse, cursor_obj);

    /* 4. 滚轮 + 键盘（中控屏调试列表/输入法时会用到） */
    lv_indev_t * mousewheel = lv_sdl_mousewheel_create();
    lv_indev_set_display(mousewheel, disp);
    lv_indev_set_group(mousewheel, lv_group_get_default());

    lv_indev_t * kb = lv_sdl_keyboard_create();
    lv_indev_set_display(kb, disp);
    lv_indev_set_group(kb, lv_group_get_default());

    return disp;
}
