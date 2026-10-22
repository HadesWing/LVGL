/**
 * @file app_sip.h
 * @brief SIP 对讲 —— 基于 oSIP（libosip2）的注册/呼叫状态机
 *
 * 【分工】
 *   oSIP 负责"SIP 消息"这件最繁琐的事：URI/头域/SDP 的编解码与校验；
 *   传输（UDP socket）、事务时序、重发由本层自己做（后续可换 oSIP 的
 *   transaction 层或 eXosip，接口不变）。
 *
 * 【线程模型】收包在独立线程，任何状态变化只 post 事件到主线程 ——
 *   与 LVGL 的交互永远发生在主线程（见 app_events）。
 */
#ifndef APP_SIP_H
#define APP_SIP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SIP_IDLE = 0,
    SIP_REGISTERING,
    SIP_ONLINE,
    SIP_INCOMING,
    SIP_INCALL,
    SIP_ERROR,
} sip_state_t;

typedef struct {
    const char *server;      /* SIP 服务器地址（本机 miniSIPServer/Asterisk） */
    uint16_t    server_port; /* 默认 5060 */
    const char *user;        /* 本机账号，如 1001 */
    const char *pass;        /* 认证密码（401 时用） */
    uint16_t    local_port;  /* 本机监听端口，默认 5062 */
} sip_cfg_t;

/* 从环境变量读取配置：SIP_SERVER / SIP_PORT / SIP_USER / SIP_PASS / SIP_LOCAL_PORT
   未设置 SIP_SERVER 时不启用（保持纯 UI demo 可跑） */
bool app_sip_init_from_env(void);

void       app_sip_start(void);
void       app_sip_stop(void);
sip_state_t app_sip_state(void);

/* 呼出 / 接听 / 挂断（M3 接对讲页按钮） */
int  app_sip_call(const char *target_uri);
int  app_sip_answer(void);
int  app_sip_hangup(void);

#endif /* APP_SIP_H */
