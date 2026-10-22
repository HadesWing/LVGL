/**
 * @file app_devices.c
 * @brief 设备模型实现：默认设备表 + 统一执行入口 + ini 持久化
 */
#include "app_devices.h"
#include "app_events.h"
#include "lvgl.h"       /* 只为 LV_SYMBOL_* 图标常量 */

#include <stdio.h>
#include <string.h>

/*---------------------------------------------------------------------------
 * 默认设备表（真实产品里来自网关发现/云端同步，这里静态定义 + ini 覆盖状态）
 *---------------------------------------------------------------------------*/
static device_t s_devices[] = {
/*  id               name        room         type        on     value online */
    { "light_living",  "客厅灯",   ROOM_LIVING, DEV_LIGHT,  false,  80,  true },
    { "curt_living",   "客厅窗帘", ROOM_LIVING, DEV_CURTAIN, false, 100,  true },
    { "ac_living",     "客厅空调", ROOM_LIVING, DEV_AC,      false,  26,  true },
    { "fresh_living",  "新风机",   ROOM_LIVING, DEV_FRESH,   false,  40,  true },
    { "plug_living",   "智能插座", ROOM_LIVING, DEV_PLUG,    false,   0,  true },
    { "cam_living",    "门口摄像头",ROOM_LIVING,DEV_CAM,     false,   0,  true },
    { "light_bed",     "卧室灯",   ROOM_BED,    DEV_LIGHT,   false,  60,  true },
    { "curt_bed",      "卧室窗帘", ROOM_BED,    DEV_CURTAIN, false, 100,  true },
    { "ac_bed",        "卧室空调", ROOM_BED,    DEV_AC,      false,  26,  true },
    { "plug_bed",      "床头插座", ROOM_BED,    DEV_PLUG,    false,   0,  true },
};
#define DEV_N (sizeof(s_devices) / sizeof(s_devices[0]))

/*---------------------------------------------------------------------------
 * 场景 = 预置命令序列（真实产品里存 SQLite/云端，这里静态表）
 *---------------------------------------------------------------------------*/
typedef struct {
    const char *name;
    const device_cmd_t *cmds;
    int n;
} scene_t;

static const device_cmd_t SCENE_HOME[] = {            /* 回家 */
    { "light_living", CMD_SET, 80 }, { "light_living", CMD_ON },
    { "curt_living",  CMD_ON },      { "ac_living",    CMD_SET, 26 }, { "ac_living", CMD_ON },
    { "fresh_living", CMD_ON },
};
static const device_cmd_t SCENE_AWAY[] = {            /* 离家：全关 */
    { NULL, CMD_OFF, 0 },
};
static const device_cmd_t SCENE_SLEEP[] = {           /* 睡眠 */
    { NULL, CMD_OFF, 0 }, { "fresh_living", CMD_SET, 30 }, { "fresh_living", CMD_ON },
};
static const device_cmd_t SCENE_MOVIE[] = {           /* 观影 */
    { "curt_living", CMD_OFF }, { "light_living", CMD_SET, 20 }, { "light_living", CMD_ON },
    { "ac_living", CMD_SET, 24 }, { "ac_living", CMD_ON },
};
static const scene_t s_scenes[] = {
    { "回家", SCENE_HOME,  6 },
    { "离家", SCENE_AWAY,  1 },
    { "睡眠", SCENE_SLEEP, 3 },
    { "观影", SCENE_MOVIE, 5 },
};
#define SCENE_N (sizeof(s_scenes) / sizeof(s_scenes[0]))

/*---------------------------------------------------------------------------
 * 持久化：本机 ini（板子上换 SQLite，接口不变）
 *   文件按 cwd 落在 bin/ 下，格式：id<TAB>on<TAB>value
 *---------------------------------------------------------------------------*/
#define DEV_INI "devices.ini"

static void devices_save(void)
{
    FILE *f = fopen(DEV_INI, "w");
    if (!f) return;
    for (unsigned i = 0; i < DEV_N; i++) {
        fprintf(f, "%s\t%d\t%d\n", s_devices[i].id,
                s_devices[i].on ? 1 : 0, s_devices[i].value);
    }
    fclose(f);
}

static void devices_load(void)
{
    FILE *f = fopen(DEV_INI, "r");
    if (!f) return;                          /* 首次运行无文件，用默认值 */
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char id[64]; int on, val;
        if (sscanf(line, "%63s\t%d\t%d", id, &on, &val) != 3) continue;
        int idx = device_find(id);
        if (idx < 0) continue;
        s_devices[idx].on    = (on != 0);
        s_devices[idx].value = (int16_t)val;
    }
    fclose(f);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
void app_devices_init(void)
{
    devices_load();
    printf("[devices] %u devices ready\n", (unsigned)DEV_N);
    fflush(stdout);
}

int device_count(void)              { return (int)DEV_N; }

const device_t *device_get(int idx) { return (idx >= 0 && idx < (int)DEV_N) ? &s_devices[idx] : NULL; }

int device_find(const char *id)
{
    if (!id) return -1;
    for (unsigned i = 0; i < DEV_N; i++) {
        if (strcmp(s_devices[i].id, id) == 0) return (int)i;
    }
    return -1;
}

const char *device_type_icon(uint8_t type)
{
    switch ((device_type_t)type) {
    case DEV_LIGHT:   return LV_SYMBOL_POWER;
    case DEV_CURTAIN: return LV_SYMBOL_LIST;      /* 竖条 ≈ 百叶 */
    case DEV_AC:      return LV_SYMBOL_SETTINGS;
    case DEV_FRESH:   return LV_SYMBOL_LOOP;
    case DEV_PLUG:    return LV_SYMBOL_CHARGE;
    case DEV_CAM:     return LV_SYMBOL_VIDEO;
    default:          return LV_SYMBOL_SETTINGS;
    }
}

const char *device_state_text(const device_t *d)
{
    static char buf[32];
    if (!d->on) {
        return (d->type == DEV_CURTAIN) ? "已合上" : "已关闭";
    }
    switch ((device_type_t)d->type) {
    case DEV_LIGHT:   snprintf(buf, sizeof(buf), "亮度 %d%%", d->value); break;
    case DEV_CURTAIN: snprintf(buf, sizeof(buf), "开度 %d%%", d->value); break;
    case DEV_AC:      snprintf(buf, sizeof(buf), "%d℃ 制冷", d->value); break;
    case DEV_FRESH:   snprintf(buf, sizeof(buf), "风量 %d",  d->value); break;
    default:          return "已开启";
    }
    return buf;
}

int device_execute(const device_cmd_t *cmd)
{
    int changed = 0;
    for (unsigned i = 0; i < DEV_N; i++) {
        device_t *d = &s_devices[i];
        if (cmd->id && strcmp(d->id, cmd->id) != 0) continue;

        switch (cmd->action) {
        case CMD_TOGGLE: d->on = !d->on; break;
        case CMD_ON:     d->on = true;  break;
        case CMD_OFF:    d->on = false; break;
        case CMD_SET:    d->value = cmd->value; d->on = true; break;
        }
        changed++;

        /* 状态变化 → 事件总线（UI 在主线程刷新）→ 落盘 */
        app_events_post(EV_DEVICE_CHANGED, (int32_t)i, 0, NULL);
    }
    if (changed) devices_save();
    return changed;
}

void device_toggle_by_index(int idx)
{
    const device_t *d = device_get(idx);
    if (!d) return;
    device_cmd_t cmd = { d->id, CMD_TOGGLE, 0 };
    device_execute(&cmd);
}

void device_run_scene(int scene)
{
    if (scene < 0 || scene >= (int)SCENE_N) return;
    const scene_t *s = &s_scenes[scene];
    for (int i = 0; i < s->n; i++) {
        device_execute(&s->cmds[i]);
    }
    printf("[devices] scene '%s' executed\n", s->name);
    fflush(stdout);
}

int         device_scene_count(void) { return (int)SCENE_N; }
const char *device_scene_name(int scene)
{
    return (scene >= 0 && scene < (int)SCENE_N) ? s_scenes[scene].name : "";
}
