/**
 * @file ui_call.h
 * @brief 对讲页（来电 + 通话中）—— M3
 *
 * 触发方式：订阅 EV_SIP_STATE。
 *   ui=2（来电）→ 压栈显示来访页（接听/拒接）
 *   ui=3（接通）→ 切"通话中"版式（计时 + RTP 收发计数）
 *   ui=0/1     → 自动出栈回到原来的页面
 */
#ifndef UI_CALL_H
#define UI_CALL_H

#include "lvgl.h"

/* 注册事件订阅（须在 app_events_init 之后、ui_nav_init 之后调用一次） */
void ui_call_init(void);

#endif /* UI_CALL_H */
