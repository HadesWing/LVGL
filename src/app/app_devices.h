/**
 * @file app_devices.h
 * @brief 设备模型 —— 中控台的业务核心
 *
 * 【设计要点（面试讲法）】
 *   - UI 只画 device_t，不认识任何具体协议；将来接 MQTT/RS485/涂鸦
 *     只是在 device_execute() 的"真正下发"那一行换实现；
 *   - 所有动作（点击/语音/场景/SIP开锁）统一走 device_execute() 一个入口，
 *     状态变化后统一发 EV_DEVICE_CHANGED —— 单一数据流，好调试好回放；
 *   - 持久化：断电重启恢复现场（本机=ini 文件，板子=SQLite，接口不变）。
 */
#ifndef APP_DEVICES_H
#define APP_DEVICES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    DEV_LIGHT = 0,     /* 灯：value = 亮度 1~100 */
    DEV_CURTAIN,       /* 窗帘：value = 开度 0~100 */
    DEV_AC,            /* 空调：value = 温度 16~30 */
    DEV_FRESH,         /* 新风/加湿：value = 风量 */
    DEV_PLUG,          /* 插座 */
    DEV_CAM,           /* 摄像头 */
    DEV_TYPE_COUNT
} device_type_t;

typedef enum {
    ROOM_LIVING = 0,   /* 客厅 */
    ROOM_BED,          /* 卧室 */
    ROOM_COUNT
} room_t;

typedef struct {
    const char *id;      /* 稳定标识（持久化/语音匹配用） */
    const char *name;    /* 界面显示名 */
    uint8_t     room;
    uint8_t     type;
    bool        on;
    int16_t     value;   /* 类型相关的参数 */
    bool        online;
} device_t;

typedef enum {
    CMD_TOGGLE = 0,    /* 翻转（UI 点击） */
    CMD_ON,
    CMD_OFF,
    CMD_SET            /* 设 value（语音"调到26度"/滑条） */
} device_action_t;

typedef struct {
    const char     *id;      /* NULL = 全部设备 */
    device_action_t action;
    int16_t         value;   /* CMD_SET 时有效 */
} device_cmd_t;

void         app_devices_init(void);            /* 构建默认设备表 + 加载持久化 */
int          device_count(void);
const device_t *device_get(int idx);
int          device_find(const char *id);       /* 返回下标，-1 不存在 */
const char  *device_type_icon(uint8_t type);    /* LVGL symbol */
const char  *device_state_text(const device_t *d);  /* "已开启"/"26℃"/"80%"... */

/* 统一动作入口：改状态 → 发 EV_DEVICE_CHANGED → 落盘。返回受影响设备数 */
int          device_execute(const device_cmd_t *cmd);

/* UI 便捷封装：按下标翻转 */
void         device_toggle_by_index(int idx);

/* 场景：预置命令序列 */
void         device_run_scene(int scene);       /* 0=回家 1=离家 2=睡眠 3=观影 */
int          device_scene_count(void);
const char  *device_scene_name(int scene);

#endif /* APP_DEVICES_H */
