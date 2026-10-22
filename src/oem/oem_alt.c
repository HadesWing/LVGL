/**
 * @file oem_alt.c
 * @brief 第二套 OEM 配置（"极光智控"客户）—— 用来证明这套接口真能换皮
 *
 * 与默认配置的差异是**故意做得很夸张**，一次性覆盖三类定制：
 *   ① 配色：石墨底 + 暖金强调（深色系客户）
 *   ② 文案：产品名/场景/房间/分类名全换
 *   ③ 结构：首页从宫格换成列表，去掉场景条，状态栏加高、圆角加大
 *
 * 验证方式（不用改一行代码、不用重新编译）：
 *   OEM_PROFILE=alt ./bin/main.exe
 */
#include "oem.h"

const oem_profile_t oem_profile_alt = {
    .id      = "alt",
    .vendor  = "AuroraHome",
    .model   = "AH-Panel-7",
    .version = "1.2.0",

    .colors = {
        [OEM_COLOR_PRIMARY]    = 0xD4A03B,   /* 暖金 */
        [OEM_COLOR_BG]         = 0x15171A,
        [OEM_COLOR_SURFACE]    = 0x22262B,
        [OEM_COLOR_CARD]       = 0x262B31,
        [OEM_COLOR_BAR]        = 0x0E1013,
        [OEM_COLOR_BAR_TEXT]   = 0xF2E9D8,
        [OEM_COLOR_TEXT]       = 0xEDEDED,
        [OEM_COLOR_TEXT_MUTED] = 0x9AA0A6,
        [OEM_COLOR_BORDER]     = 0x33383F,
        [OEM_COLOR_OK]         = 0x43A047,
        [OEM_COLOR_DANGER]     = 0xE53935,
    },

    .text = {
        [OEM_TXT_PRODUCT]      = "全屋智控",
        [OEM_TXT_BRAND]        = "AuroraHome",
        [OEM_TXT_SIP_OFFLINE]  = "对讲未连接",
        [OEM_TXT_SIP_ONLINE]   = "对讲就绪",
        [OEM_TXT_SIP_CALLING]  = "拨号中",
        [OEM_TXT_SIP_INCOMING] = "有人呼叫",
        [OEM_TXT_SIP_INCALL]   = "对讲中",
        [OEM_TXT_SCENE_0]      = "回家",
        [OEM_TXT_SCENE_1]      = "会客",
        [OEM_TXT_SCENE_2]      = "影院",
        [OEM_TXT_SCENE_3]      = "就寝",
        [OEM_TXT_ROOM_0]       = "主厅",
        [OEM_TXT_ROOM_1]       = "次卧",
        [OEM_TXT_ROOM_ALL]     = "整宅",
        [OEM_TXT_CAT_LIGHT]    = "照明",
        [OEM_TXT_CAT_CURTAIN]  = "遮阳",
        [OEM_TXT_CAT_AC]       = "冷暖",
        [OEM_TXT_CAT_FRESH]    = "换气",
        [OEM_TXT_CAT_PLUG]     = "电源",
        [OEM_TXT_CAT_CAM]      = "安防",
    },

    .layout = {
        .home_style     = OEM_HOME_LIST,
        .bar_h          = 40,
        .title_h        = 52,
        .scene_h        = 0,
        .card_w         = 300,     /* LIST 风格下表示行宽参考 */
        .card_h         = 64,      /* 行高 */
        .gap            = 8,
        .radius         = 22,
        .pad            = 14,
        .show_scene_bar = false,
        .show_clock     = true,
        .show_brand     = true,
        .show_voice_bar = false,
    },

    .caps = OEM_CAP_SIP | OEM_CAP_VOICE | OEM_CAP_VIDEO,
};
