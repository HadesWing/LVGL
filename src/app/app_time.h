/**
 * @file app_time.h
 * @brief 单调时钟（毫秒）。**只做一件事**，但必须是单调的。
 *
 * 为什么不用 time(NULL)：系统时间会被 NTP / 用户改时区往回拨，
 * 一旦回拨，所有 `now - start` 全部变成负数（在 uint64 下是天文数字）。
 * 表现是"超时永远不触发"或者"第一次检查就超时"，而且只在接了网的板子上偶发。
 * 退避重连、丢包恢复计时这类逻辑都踩不起这一下，所以统一走 CLOCK_MONOTONIC。
 */
#ifndef APP_TIME_H
#define APP_TIME_H

#include <stdint.h>

#ifdef _WIN32
  #include <windows.h>
static inline uint64_t app_now_ms(void) { return (uint64_t)GetTickCount64(); }
#else
  #include <time.h>
static inline uint64_t app_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif

#endif /* APP_TIME_H */
