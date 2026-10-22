/**
 * @file app_mock.c
 * @brief 设备事件模拟层实现
 *
 * 【日志一律英文】Windows 控制台默认 GBK，源码是 UTF-8，中文 printf 会乱码；
 * 嵌入式现场也常用英文日志，这里统一用英文便于 grep 与交叉比对。
 */
#include "app_mock.h"
#include "ui_indoor.h"
#include "ui_theme.h"
#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*---------------------------------------------------------------------------
 * 模拟的语音指令表（真实设备里这些来自思必驰 ASR 的语义结果）
 *---------------------------------------------------------------------------*/
typedef struct {
    const char *asr_text;   /* ASR 识别文本（日志用） */
    const char *intent;     /* 语义意图 */
    const char *ui_hint;    /* 回显到界面状态行 */
} mock_voice_t;

static const mock_voice_t s_voices[] = {
    { "turn on living room light", "LIGHT_ON",    "Voice: LIGHT_ON"    },
    { "close the curtain",         "CURTAIN_CLOSE","Voice: CURTAIN_CLOSE" },
    { "set ac to 26 degree",       "AC_26",       "Voice: AC_26"       },
    { "call the property office",  "CALL_GUARD",  "Voice: CALL_GUARD"  },
    { "unlock the door",           "DOOR_UNLOCK", "Voice: DOOR_UNLOCK" },
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
    ui_indoor_simulate_call();
}

/*---------------------------------------------------------------------------
 * 定时器：模拟语音识别结果（真实来源 = 思必驰 SDK 回调）
 *---------------------------------------------------------------------------*/
static void voice_timer_cb(lv_timer_t *t)
{
    (void)t;
    const mock_voice_t *v = &s_voices[s_voice_idx++ % (sizeof(s_voices) / sizeof(s_voices[0]))];
    printf("[mock] asr=\"%s\" -> intent=%s\n", v->asr_text, v->intent);
    fflush(stdout);
    ui_indoor_set_status_text(v->ui_hint);
}

/*---------------------------------------------------------------------------
 * 压测模式：反复创建/销毁对象，暴露内存泄漏与碎片增长
 *   这是嵌入式 7x24 里最常见的一类崩溃来源：界面刷新时重建控件
 *---------------------------------------------------------------------------*/
static void stress_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 1) 建 8 个临时控件，立刻删掉 —— 若内存池 used 长期上涨，就是泄漏 */
    for (int i = 0; i < 8; i++) {
        lv_obj_t *tmp = lv_button_create(lv_screen_active());
        lv_obj_set_size(tmp, 40, 20);
        lv_obj_set_ignore_layout(tmp, true);
        lv_obj_delete(tmp);          /* 立即销毁，等价于"页面重建" */
    }

    /* 2) 主题来回切 —— 检查样式热替换有没有残留 */
    ui_theme_toggle();

    /* 3) 触发一次来电 —— 检查弹窗动画重复触发 */
    ui_indoor_simulate_call();

    printf("[stress] churn 8 objects + theme toggle + call\n");
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
        printf("[mock] STRESS MODE: object churn + theme toggle + call\n");
        lv_timer_create(stress_timer_cb, 200, NULL);
        return;                       /* 压测时不叠加常规事件，避免干扰读数 */
    }

    printf("[mock] device simulator on: call / asr events\n");
    lv_timer_create(call_timer_cb, 12000, NULL);
    lv_timer_create(voice_timer_cb, 9000, NULL);
}
