/**
 * @file app_events.h
 * @brief 应用层事件总线 —— 模块解耦 + 跨线程到主线程的收口
 *
 * 【为什么要有事件总线】
 *   1. 解耦：UI 不认识设备协议，设备层不认识 LVGL。设备状态变了发 EV_DEVICE_CHANGED，
 *      谁关心谁订阅 —— 以后加语音、加 SIP、加日志，都不用改已有代码。
 *   2. 线程收口：语音 SDK / SIP 栈的回调都在**它们自己的线程**里触发，
 *      而 LVGL 对象**只允许主线程碰**（官方红线）。事件总线是唯一的跨线程桥：
 *      任何线程 post() → 环形队列 → 主线程 lv_timer 统一分发。
 *      这是嵌入式 UI 并发纪律的标准答案（面试必讲）。
 *
 * 【实现说明】
 *   - 订阅表：静态数组 + 线性查找（事件种类少，性能足够；动态注册只增不删）；
 *   - 队列：定长环形缓冲，满了丢最旧并打日志（监控上宁可丢事件也不能阻塞音频线程）；
 *   - 锁：CRITICAL_SECTION(Win) / pthread_mutex(Linux)，见 .c 的平台分支。
 */
#ifndef APP_EVENTS_H
#define APP_EVENTS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EV_DEVICE_CHANGED = 0,  /* arg0 = 设备下标；UI 刷新对应卡片 */
    EV_VOICE_TEXT,          /* ptr  = ASR 文本(const char*)；语音浮层字幕 */
    EV_VOICE_STATE,         /* arg0 = 语音状态机状态（M2 细化） */
    EV_SIP_STATE,           /* arg0 = 呼叫状态（M3：0=idle 1=incoming 2=answered） */
    EV_TYPE_COUNT
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    int32_t   arg0;
    int32_t   arg1;
    const void *ptr;        /* 只允许指向静态/长生命周期数据，队列不做深拷贝 */
} app_event_t;

typedef void (*app_event_cb_t)(const app_event_t *e, void *user_data);

/* 初始化（创建分发 timer，须在 lv_init() 之后调用） */
void app_events_init(void);

/* 订阅：回调一定在主线程（LVGL 线程）执行 */
bool app_events_subscribe(app_event_type_t type, app_event_cb_t cb, void *user_data);

/* 退订：页面销毁时必须调用，否则回调会打到已删除的控件上（野指针崩溃） */
void app_events_unsubscribe(app_event_type_t type, app_event_cb_t cb);

/* 发布：任意线程可调（内部加锁入队），主线程统一分发 */
void app_events_post(app_event_type_t type, int32_t arg0, int32_t arg1, const void *ptr);

#endif /* APP_EVENTS_H */
