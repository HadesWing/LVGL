/**
 * @file app_lock.h
 * @brief 最小互斥封装 —— 跨线程交接数据时用
 *
 * 【为什么不用 std::mutex / C11 threads.h】
 *   这是个 C11 工程，要能在 MSYS2(gcc)、Windows、板子的交叉工具链上编。
 *   C11 的 <threads.h> 在 MinGW 上时有时无（依赖 winpthreads 配置），
 *   与其赌工具链，不如两行平台分支 —— 这层薄得几乎不会引入 bug。
 *
 * 【什么时候该用】
 *   只在**跨线程交接**时用：SIP/RTP 线程把数据交给 LVGL 主线程。
 *   LVGL 对象只允许主线程碰（官方红线），所以线程边界必须收口在
 *   事件总线 + 这种交接锁上，而不是散落在各个模块里。
 *
 * 【什么时候不该用】
 *   同线程内不要上锁 —— 本工程 UI 逻辑全在主线程，加锁纯属自找麻烦
 *   （还可能因为回调里重入而自己锁死自己）。
 */
#ifndef APP_LOCK_H
#define APP_LOCK_H

#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION app_lock_t;
  static inline void app_lock_init(app_lock_t *l) { InitializeCriticalSection(l); }
  static inline void app_lock_take(app_lock_t *l) { EnterCriticalSection(l); }
  static inline void app_lock_give(app_lock_t *l) { LeaveCriticalSection(l); }
#else
  #include <pthread.h>
  typedef pthread_mutex_t app_lock_t;
  static inline void app_lock_init(app_lock_t *l) { pthread_mutex_init(l, NULL); }
  static inline void app_lock_take(app_lock_t *l) { pthread_mutex_lock(l); }
  static inline void app_lock_give(app_lock_t *l) { pthread_mutex_unlock(l); }
#endif

#endif /* APP_LOCK_H */
