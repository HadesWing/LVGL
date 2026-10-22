/**
 * @file app_stats.h
 * @brief 运行时指标采集 —— 内存 / 帧率 / 主循环耗时
 *
 * 为什么需要它（对应 JD 的"界面、动画、内存优化"）：
 *   - LVGL 的"性能"不是感觉，是三组数：帧率、单次 lv_timer_handler 耗时、内存池水位与碎片率。
 *   - 嵌入式设备没有任务管理器，必须自己埋点；这套埋点在 PC 上跑出来的趋势，
 *     与在 RK3308 上跑出来的趋势同构（绝对值不同，拐点一致）—— 这就是无板调优的底气。
 *
 * 三种用法：
 *   1) 默认：启动即工作，控制台每 5 秒打印一行摘要；
 *   2) 环境变量 LVGL_STATS=1：同时把每秒采样写入 stats.csv（可直接画曲线）；
 *   3) 环境变量 LVGL_RUN_SEC=N：N 秒后自动打印汇总并退出（长稳/回归用）。
 *
 * 【跨平台】Windows 用 QueryPerformanceCounter，Linux 用 clock_gettime，均在内部封装。
 */
#ifndef APP_STATS_H
#define APP_STATS_H

#include <stdint.h>

/* 启动采集：注册渲染完成回调 + 1 秒采样定时器 */
void app_stats_init(void);

/* 主循环每次调用：传入本次 lv_timer_handler() 的耗时（微秒） */
void app_stats_note_loop(uint32_t loop_us);

/* 单调时钟（微秒），供主循环测耗时 */
uint64_t app_stats_now_us(void);

/* 打印最终汇总（长稳测试结束、或退出前调用） */
void app_stats_summary(void);

#endif
