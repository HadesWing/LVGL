/**
 * @file app_stats.c
 * @brief 内存 / 帧率 / 主循环耗时采集实现
 */
#include "app_stats.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <time.h>
#endif

/*---------------------------------------------------------------------------
 * 采样状态
 *---------------------------------------------------------------------------*/
static uint32_t s_frames;        /* 累计渲染帧数（RENDER_READY 事件） */
static uint32_t s_frames_last;   /* 上一秒的帧数，用于算 FPS */
static uint32_t s_loop_us_sum;   /* 本秒内主循环耗时总和 */
static uint32_t s_loop_us_max;   /* 本秒内主循环耗时峰值 */
static uint32_t s_loop_cnt;      /* 本秒内主循环次数 */
static uint32_t s_sec;           /* 已运行秒数 */
static size_t   s_peak_used;     /* 内存池历史峰值（跨秒记录） */
static FILE    *s_csv;           /* stats.csv（LVGL_STATS=1 时打开） */
static int      s_run_sec;       /* LVGL_RUN_SEC：到点自动退出 */

/*---------------------------------------------------------------------------
 * 单调时钟
 *---------------------------------------------------------------------------*/
uint64_t app_stats_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER now;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1000000.0 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

/*---------------------------------------------------------------------------
 * 渲染帧计数：display 的 RENDER_READY 事件每刷完一帧触发一次
 *---------------------------------------------------------------------------*/
static void on_render_ready(lv_event_t *e)
{
    (void)e;
    s_frames++;
}

/*---------------------------------------------------------------------------
 * 每秒采样
 *---------------------------------------------------------------------------*/
static void sample_cb(lv_timer_t *t)
{
    (void)t;
    s_sec++;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    uint32_t fps   = s_frames - s_frames_last;
    s_frames_last  = s_frames;
    uint32_t avg   = s_loop_cnt ? (s_loop_us_sum / s_loop_cnt) : 0;
    uint32_t max   = s_loop_us_max;
    size_t   used  = mon.total_size - mon.free_size;

    if (mon.max_used > s_peak_used) s_peak_used = mon.max_used;

    if (s_csv) {
        fprintf(s_csv, "%u,%u,%u,%u,%zu,%zu,%u,%u,%zu\n",
                s_sec, fps, avg, max, used, mon.max_used,
                mon.used_pct, mon.frag_pct, mon.free_biggest_size);
        fflush(s_csv);
    }

    if (s_sec % 5 == 0) {
        printf("[stats] %3us  FPS %3u  loop avg %3u us / max %4u us  "
               "mem %zu/%zu B (%u%%)  peak %zu  frag %u%%\n",
               s_sec, fps, avg, max, used, mon.total_size,
               mon.used_pct, s_peak_used, mon.frag_pct);
        fflush(stdout);
    }

    /* 长稳测试：到点收工 */
    if (s_run_sec > 0 && (int)s_sec >= s_run_sec) {
        app_stats_summary();
        exit(0);
    }

    s_loop_us_sum = 0;
    s_loop_us_max = 0;
    s_loop_cnt    = 0;
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
void app_stats_init(void)
{
    /* 渲染完成事件挂在 display 上：这是 LVGL 唯一可靠的"帧边界" */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, NULL);
    }

    const char *csv_env = getenv("LVGL_STATS");
    if (csv_env && strcmp(csv_env, "0") != 0) {
        s_csv = fopen("stats.csv", "w");
        if (s_csv) {
            fprintf(s_csv, "sec,fps,loop_avg_us,loop_max_us,mem_used,mem_max_used,"
                           "used_pct,frag_pct,free_biggest\n");
            printf("[stats] 采样输出 -> stats.csv\n");
        }
    }

    const char *run_env = getenv("LVGL_RUN_SEC");
    s_run_sec = run_env ? atoi(run_env) : 0;

    lv_timer_create(sample_cb, 1000, NULL);
}

void app_stats_note_loop(uint32_t loop_us)
{
    s_loop_us_sum += loop_us;
    if (loop_us > s_loop_us_max) s_loop_us_max = loop_us;
    s_loop_cnt++;
}

void app_stats_summary(void)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    printf("\n========== 运行汇总 ==========\n");
    printf("运行时长      : %u s\n", s_sec);
    printf("总渲染帧数    : %u\n", s_frames);
    printf("内存池总大小  : %zu B\n", mon.total_size);
    printf("内存历史峰值  : %zu B (%zu%% of pool)\n",
           mon.max_used, mon.total_size ? mon.max_used * 100 / mon.total_size : 0);
    printf("当前占用      : %zu B (%u%%)\n", mon.total_size - mon.free_size, mon.used_pct);
    printf("碎片率        : %u%%\n", mon.frag_pct);
    printf("最大连续空闲  : %zu B\n", mon.free_biggest_size);
    printf("==============================\n");
    fflush(stdout);

    if (s_csv) { fclose(s_csv); s_csv = NULL; }
}
