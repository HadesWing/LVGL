/**
 * @file ui_nav.c
 * @brief 页面栈实现
 */
#include "ui_nav.h"

#include <stdio.h>

#define NAV_MAX 8

static lv_obj_t *s_stack[NAV_MAX];
static int       s_depth;

void ui_nav_init(void)
{
    s_depth = 0;
    s_stack[s_depth++] = lv_screen_active();   /* 根页 = 当前 active screen */
}

int ui_nav_depth(void) { return s_depth; }

bool ui_nav_push(ui_page_build_cb_t build, void *arg)
{
    if (s_depth >= NAV_MAX || !build) return false;

    lv_obj_t *scr = lv_obj_create(NULL);
    build(scr, arg);

    s_stack[s_depth++] = scr;
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
    return true;
}

bool ui_nav_pop(void)
{
    if (s_depth <= 1) return false;

    s_depth--;
    /* auto_del=true：把当前最上层 screen 销毁，返回上一页 */
    lv_screen_load_anim(s_stack[s_depth - 1], LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
    return true;
}
