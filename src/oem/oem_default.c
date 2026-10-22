/**
 * @file oem_default.c
 * @brief 默认 OEM 配置（白牌基线：深色状态栏 + 浅灰底 + 蓝色强调）
 *
 * 这里**只放数据**。新客户 = 复制本文件改数值，不要动任何 ui_*.c。
 */
#include "oem.h"

const oem_profile_t oem_profile_default = {
    .id      = "default",
    .vendor  = "DeepSmart",
    .model   = "IDU-3308",
    .version = "0.9.0",

    .colors = {
        [OEM_COLOR_PRIMARY]    = 0x2F80ED,   /* 品牌蓝 */
        [OEM_COLOR_BG]         = 0xECEFF1,
        [OEM_COLOR_SURFACE]    = 0x37474F,
        [OEM_COLOR_CARD]       = 0xFFFFFF,
        [OEM_COLOR_BAR]        = 0x263238,
        [OEM_COLOR_BAR_TEXT]   = 0xFFFFFF,
        [OEM_COLOR_TEXT]       = 0x263238,
        [OEM_COLOR_TEXT_MUTED] = 0x607D8B,
        [OEM_COLOR_BORDER]     = 0xCFD8DC,
        [OEM_COLOR_OK]         = 0x2E7D32,
        [OEM_COLOR_DANGER]     = 0xC62828,
    },

    .text = {
        [OEM_TXT_PRODUCT]      = "我的设备",
        [OEM_TXT_BRAND]        = "DeepSmart",
        [OEM_TXT_SIP_OFFLINE]  = "SIP 离线",
        [OEM_TXT_SIP_ONLINE]   = "SIP 在线",
        [OEM_TXT_SIP_CALLING]  = "呼出中",
        [OEM_TXT_SIP_INCOMING] = "来电",
        [OEM_TXT_SIP_INCALL]   = "通话中",
        [OEM_TXT_SCENE_0]      = "回家",
        [OEM_TXT_SCENE_1]      = "离家",
        [OEM_TXT_SCENE_2]      = "睡眠",
        [OEM_TXT_SCENE_3]      = "观影",
        [OEM_TXT_ROOM_0]       = "客厅",
        [OEM_TXT_ROOM_1]       = "卧室",
        [OEM_TXT_ROOM_ALL]     = "全宅",
        [OEM_TXT_CAT_LIGHT]    = "灯光",
        [OEM_TXT_CAT_CURTAIN]  = "窗帘",
        [OEM_TXT_CAT_AC]       = "空调",
        [OEM_TXT_CAT_FRESH]    = "新风",
        [OEM_TXT_CAT_PLUG]     = "插座",
        [OEM_TXT_CAT_CAM]      = "摄像头",
    },

    .layout = {
        .home_style     = OEM_HOME_GRID,
        .bar_h          = 34,
        .title_h        = 46,
        .scene_h        = 56,
        .card_w         = 140,
        .card_h         = 88,
        .gap            = 10,
        .radius         = 14,
        .pad            = 10,
        .show_scene_bar = true,
        .show_clock     = true,
        .show_brand     = false,
        .show_voice_bar = true,
    },

    .caps = OEM_CAP_SIP | OEM_CAP_VOICE,
};
