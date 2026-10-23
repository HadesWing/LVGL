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
#include <string.h>
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
#include "ui/ui_home.h"
#include "ui/ui_call.h"
#include "ui/ui_theme.h"
#include "app/app_events.h"
#include "app/app_devices.h"
#include "app/app_stats.h"
#include "app/app_mock.h"
#include "app/app_version.h"
#include "oem/oem.h"
#ifdef APP_WITH_SIP
#include "app/app_sip.h"
#endif

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
#ifdef _WIN32
  /* 源码是 UTF-8，Windows 控制台默认 GBK(936)，不切换就会打出乱码 */
  SetConsoleOutputCP(CP_UTF8);
#endif

  /* stdout 改成行缓冲。
     默认行为：输出到终端是行缓冲，重定向到文件/管道就变成**全缓冲**（4KB）。
     嵌入式上日志基本都是重定向（journald、串口、nohup），于是最关键的
     那几行 —— 挂断统计、崩溃前的最后几条 —— 还在缓冲区里，进程一被 kill
     就永远看不到。排查"为什么挂断时统计是空的"能查半天。
     代价：每行一次 write。本程序的日志都是状态变化时才打，不是每帧打，
     这点开销可以忽略。 */
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);

  /* 参数处理必须在 lv_init / hal_init **之前**：
     板上排障经常是 ssh 进去敲一行命令，此时没有显示设备。
     如果先初始化 LVGL/SDL 再解析参数，--version 会因为打不开 framebuffer 直接失败，
     结果想查个版本号还得先把屏幕接上。 */
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!a) continue;
    if (a[0] != '-') continue;             /* 忽略非选项参数，别因为多打一个词就退出 */

    if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
      printf("indoor-unit %s\n", app_build_info());
      return 0;
    }
    if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
      printf("用法: indoor-unit [--version] [--help]\n\n");
      printf("室内机主程序（带屏可视对讲）。无参数 = 正常启动。\n\n");
      printf("配置优先级：环境变量 > sip.ini > 编译默认值\n");
      printf("  SIP_SERVER / SIP_USER / SIP_PASSWORD   SIP 服务器与账号\n");
      printf("  SIP_LOCAL_PORT      本地 SIP 端口（默认 5062，RTP 由它派生）\n");
      printf("  SIP_NO_REGISTER=1   点对点直呼，不注册\n");
      printf("  SIP_AUTOANSWER=1    自动接听；SIP_AUTOCALL=sip:1001@IP:PORT 自动呼出\n");
      printf("  SIP_VIDEO_SINK      disp|file|drop  画面去向（默认 drop 不收视频）\n");
      printf("  SIP_VIDEO_RESYNC=0  关闭丢包恢复（默认开启，做 A/B 对照用）\n");
      printf("  OEM_PROFILE         白牌配置档：default|alt；OEM_DUMP=1 打印生效值\n");
      printf("  LVGL_STATS=1        每 5 秒打印帧率/内存/循环耗时\n");
      printf("\n详细说明见 docs/ 下的文档。\n");
      return 0;
    }
    fprintf(stderr, "未知参数：%s（用 --help 看支持项）\n", a);
    return 2;
  }

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
  /* OEM：选 profile + 套 oem.ini 覆盖，必须最先做 —— 主题、文案、布局都读它 */
  oem_init();
  if (getenv("OEM_DUMP")) oem_dump();

  ui_theme_init(UI_THEME_LIGHT);  /* ← 主题/命名样式（先于一切界面） */
  app_events_init();              /* ← 事件总线（主线程分发 timer） */
  app_devices_init();             /* ← 设备模型（加载持久化状态） */
  ui_home_init();                 /* ← M1：智能家居中控台主页 */
  ui_call_init();                 /* ← M3：对讲页（来电/通话中，靠事件驱动） */
  app_stats_init();               /* ← 内存/帧率/循环耗时埋点 */
  app_mock_init();                /* ← 设备事件模拟（无板联调用） */

#ifdef APP_WITH_SIP
  /* SIP 未配置时保持 UI-only；设置了 SIP_SERVER 才真正注册 */
  if(app_sip_init_from_env()) {
    app_sip_start();
  }
#endif

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

