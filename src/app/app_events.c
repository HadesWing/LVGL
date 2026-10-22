/**
 * @file app_events.c
 * @brief 事件总线实现：订阅表 + 环形队列 + 主线程分发
 */
#include "app_events.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
#endif

/*---------------------------------------------------------------------------
 * 订阅表
 *---------------------------------------------------------------------------*/
#define SUB_MAX 16
typedef struct {
    bool        used;
    app_event_type_t type;
    app_event_cb_t   cb;
    void      *user_data;
} subscriber_t;

static subscriber_t s_subs[SUB_MAX];
static int s_sub_count;

/*---------------------------------------------------------------------------
 * 环形队列（跨线程）
 *---------------------------------------------------------------------------*/
#define Q_SIZE 32
typedef struct {
    app_event_t buf[Q_SIZE];
    int head;      /* 写位置 */
    int tail;      /* 读位置 */
    int dropped;
} event_queue_t;

static event_queue_t s_queue;

/* 平台锁 */
#ifdef _WIN32
static CRITICAL_SECTION s_lock;
static void lock_init(void)    { InitializeCriticalSection(&s_lock); }
static void lock_take(void)    { EnterCriticalSection(&s_lock); }
static void lock_give(void)    { LeaveCriticalSection(&s_lock); }
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_init(void)    { pthread_mutex_init(&s_lock, NULL); }
static void lock_take(void)    { pthread_mutex_lock(&s_lock); }
static void lock_give(void)    { pthread_mutex_unlock(&s_lock); }
#endif

/*---------------------------------------------------------------------------
 * 主线程分发：30ms 一次，把队列里的事件按序派给订阅者
 *---------------------------------------------------------------------------*/
static void dispatch_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (;;) {
        app_event_t e;
        lock_take();
        if (s_queue.tail == s_queue.head) {      /* 空 */
            lock_give();
            return;
        }
        e = s_queue.buf[s_queue.tail];
        s_queue.tail = (s_queue.tail + 1) % Q_SIZE;
        lock_give();

        for (int i = 0; i < s_sub_count; i++) {
            if (s_subs[i].used && s_subs[i].type == e.type) {
                s_subs[i].cb(&e, s_subs[i].user_data);
            }
        }
    }
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
void app_events_init(void)
{
    lock_init();
    memset(s_subs, 0, sizeof(s_subs));
    memset(&s_queue, 0, sizeof(s_queue));
    s_sub_count = 0;
    /* 30ms：比人眼感知快一个量级，又远低于 LVGL 默认 33ms 刷新周期带来的额外负载 */
    lv_timer_create(dispatch_timer_cb, 30, NULL);
}

bool app_events_subscribe(app_event_type_t type, app_event_cb_t cb, void *user_data)
{
    if (s_sub_count >= SUB_MAX) return false;
    s_subs[s_sub_count].used      = true;
    s_subs[s_sub_count].type      = type;
    s_subs[s_sub_count].cb        = cb;
    s_subs[s_sub_count].user_data = user_data;
    s_sub_count++;
    return true;
}

void app_events_unsubscribe(app_event_type_t type, app_event_cb_t cb)
{
    int w = 0;
    for (int r = 0; r < s_sub_count; r++) {
        if (s_subs[r].type == type && s_subs[r].cb == cb) continue;   /* 丢弃 */
        if (w != r) s_subs[w] = s_subs[r];
        w++;
    }
    s_sub_count = w;
}

void app_events_post(app_event_type_t type, int32_t arg0, int32_t arg1, const void *ptr)
{
    lock_take();
    int next = (s_queue.head + 1) % Q_SIZE;
    if (next == s_queue.tail) {                  /* 满了：丢最旧，保音频线程不被阻塞 */
        s_queue.tail = (s_queue.tail + 1) % Q_SIZE;
        s_queue.dropped++;
        printf("[events] queue full, dropped oldest (total %d)\n", s_queue.dropped);
    }
    s_queue.buf[s_queue.head].type = type;
    s_queue.buf[s_queue.head].arg0 = arg0;
    s_queue.buf[s_queue.head].arg1 = arg1;
    s_queue.buf[s_queue.head].ptr  = ptr;
    s_queue.head = next;
    lock_give();
}
