/**
 * @file main.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#ifndef _DEFAULT_SOURCE
  #define _DEFAULT_SOURCE /* needed for usleep() */
#endif

#include <stdlib.h>
#include <stdio.h>
#ifdef _WIN32
  #include <windows.h>   /* SetConsoleOutputCP：让 printf 的中文不乱码 */
#endif
#ifdef _MSC_VER
  #include <Windows.h>
#else
  #include <unistd.h>
  #include <pthread.h>
#endif
#include "lvgl/lvgl.h"
#include "lvgl/examples/lv_examples.h"
#include "lvgl/demos/lv_demos.h"
#include <SDL.h>

#include "hal/hal.h"
#include "ui/ui_indoor.h"
#include "app/app_stats.h"
#include "app/app_mock.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

#if LV_USE_OS != LV_OS_FREERTOS

int main(int argc, char **argv)
{
  (void)argc; /*Unused*/
  (void)argv; /*Unused*/

#ifdef _WIN32
  /* 源码是 UTF-8，Windows 控制台默认 GBK(936)，不切换就会打出乱码 */
  SetConsoleOutputCP(CP_UTF8);
#endif

  /*Initialize LVGL*/
  lv_init();

  /*Initialize the HAL (display, input devices, tick) for LVGL*/
  hal_init(320, 480);

  /* Run the default demo */
  /* To try a different demo or example, replace this with one of: */
  /* - lv_demo_benchmark(); */
  /* - lv_demo_stress(); */
  /* - lv_example_label_1(); */
  /* - etc. */
  /* lv_demo_widgets();            ← 官方 demo（学习对照用） */
  ui_indoor_init();               /* ← 自研界面：室内机主界面 */
  app_stats_init();               /* ← 内存/帧率/循环耗时埋点 */
  app_mock_init();                /* ← 设备事件模拟（无板联调用） */

  while(1) {
    /* 测一次 lv_timer_handler 的耗时：这是"渲染是否吃满 CPU"的直接证据 */
    uint64_t t0 = app_stats_now_us();
    uint32_t sleep_time_ms = lv_timer_handler();
    app_stats_note_loop((uint32_t)(app_stats_now_us() - t0));

    if(sleep_time_ms == LV_NO_TIMER_READY){
	sleep_time_ms =  LV_DEF_REFR_PERIOD;
    }
    if(sleep_time_ms < 1) sleep_time_ms = 1;   /* 防 0 → 忙等吃满一个核 */
#ifdef _MSC_VER
    Sleep(sleep_time_ms);
#else
    usleep(sleep_time_ms * 1000);
#endif
  }

  return 0;
}


#endif

/**********************
 *   STATIC FUNCTIONS
 **********************/

