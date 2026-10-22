/**
 * @file app_mock.c
 * @brief 设备事件模拟层实现
 *
 * 【日志一律英文】Windows 控制台默认 GBK，源码是 UTF-8，中文 printf 会乱码；
 * 嵌入式现场也常用英文日志，这里统一用英文便于 grep 与交叉比对。
 *
 * 【M1 变化】mock 不再直接碰 UI，全部走事件总线/设备层 —— 与真实来源
 * （思必驰回调 / SIP INVITE）在同一条数据通路上，将来换实现 UI 无感。
 */
#include "app_mock.h"
#include "app_events.h"
#include "app_devices.h"
#include "ui_theme.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*---------------------------------------------------------------------------
 * 模拟的语音指令表（真实设备里这些来自思必驰 ASR 的语义结果）
 *---------------------------------------------------------------------------*/
typedef struct {
    const char *asr;        /* ASR 识别文本（事件里回显） */
    const char *id;         /* 目标设备 */
    device_action_t act;
    int16_t val;
} mock_voice_t;

static const mock_voice_t s_voices[] = {
    { "打开客厅灯",   "light_living", CMD_ON,   0  },
    { "关闭客厅灯",   "light_living", CMD_OFF,  0  },
    { "打开窗帘",     "curt_living",  CMD_ON,   0  },
    { "空调调到26度", "ac_living",    CMD_SET,  26 },
    { "打开新风机",   "fresh_living", CMD_ON,   0  },
};
static uint32_t s_voice_idx;

/*---------------------------------------------------------------------------
 * 定时器：模拟门口机呼叫（真实来源 = SIP INVITE）
 *---------------------------------------------------------------------------*/
static void call_timer_cb(lv_timer_t *t)
{
    (void)t;
    printf("[mock] door station 101 -> CALL (simulated SIP INVITE)\n");
    fflush(stdout);
    app_events_post(EV_SIP_STATE, 2, 0, NULL);   /* 2 = incoming，M3 接对讲页 */
}

/*---------------------------------------------------------------------------
 * 定时器：模拟语音识别结果（真实来源 = 思必驰/sherpa-onnx SDK 回调）
 *---------------------------------------------------------------------------*/
static void voice_timer_cb(lv_timer_t *t)
{
    (void)t;
    const mock_voice_t *v = &s_voices[s_voice_idx++ % (sizeof(s_voices) / sizeof(s_voices[0]))];
    printf("[mock] asr=\"%s\" -> device %s action %d\n", v->asr, v->id, (int)v->act);
    fflush(stdout);

    app_events_post(EV_VOICE_TEXT, 0, 0, v->asr);            /* 字幕 */
    device_cmd_t cmd = { v->id, v->act, v->val };            /* 执行 */
    device_execute(&cmd);
}

/*---------------------------------------------------------------------------
 * 压测模式：反复创建/销毁对象，暴露内存泄漏与碎片增长
 *   这是嵌入式 7x24 里最常见的一类崩溃来源：界面刷新时重建控件
 *---------------------------------------------------------------------------*/
static void stress_timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t round;

    /* 1) 建 8 个临时控件，立刻删掉 —— 若内存池 used 长期上涨，就是泄漏 */
    for (int i = 0; i < 8; i++) {
        lv_obj_t *tmp = lv_button_create(lv_screen_active());
        lv_obj_set_size(tmp, 40, 20);
        lv_obj_set_ignore_layout(tmp, true);
        lv_obj_delete(tmp);          /* 立即销毁，等价于"页面重建" */
    }

    /* 2) 主题来回切 —— 检查样式热替换有没有残留 */
    ui_theme_toggle();

    /* 3) 翻转一个设备 —— 全链路压测：执行→落盘→事件→UI 刷新 */
    device_toggle_by_index((int)(round++ % 4));

    printf("[stress] churn 8 objects + theme toggle + device toggle\n");
    fflush(stdout);
}

/*---------------------------------------------------------------------------
 * 入口
 *---------------------------------------------------------------------------*/
void app_mock_init(void)
{
    const char *mock_env = getenv("LVGL_MOCK");
    if (mock_env && strcmp(mock_env, "0") == 0) {
        printf("[mock] disabled by LVGL_MOCK=0\n");
        return;
    }

    const char *stress_env = getenv("LVGL_STRESS");
    if (stress_env && strcmp(stress_env, "0") != 0) {
        printf("[mock] STRESS MODE: object churn + theme toggle + device churn\n");
        lv_timer_create(stress_timer_cb, 200, NULL);
        return;                       /* 压测时不叠加常规事件，避免干扰读数 */
    }

    printf("[mock] device simulator on: call / asr events\n");
    app_events_post(EV_SIP_STATE, 1, 0, NULL);   /* 1 = online */
    lv_timer_create(call_timer_cb, 12000, NULL);
    lv_timer_create(voice_timer_cb, 9000, NULL);
}
