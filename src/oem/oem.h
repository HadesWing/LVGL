/**
 * @file oem.h
 * @brief OEM 定制层 —— 一套代码供多个客户/项目复用
 *
 * 【为什么室内机一定要有这一层】
 *   室内机是典型的"白牌"产品：同一套软硬件平台，卖给 A 客户叫"智慧屏"，
 *   卖给 B 客户叫"智能中控"，配色、开机名、文案、甚至首页是宫格还是列表
 *   都不一样。如果把这些差异写死在 ui_*.c 里，每接一个客户就要开一条
 *   长期维护的代码分支 —— 这是嵌入式项目最常见的维护地狱。
 *
 * 【本工程的三层覆盖，从粗到细】
 *   ① 编译期多 profile：oem_default.c / oem_alt.c ... 每个客户一个只含数据的 .c；
 *      互相不干扰，编译时全部编进去（每个才几 KB 常量）。
 *   ② 运行期选择：环境变量 OEM_PROFILE=alt，或 oem.ini 里 profile=alt
 *      —— 现场验证不用重新编译。
 *   ③ 运行期微调：oem.ini 覆盖任意 color./text./layout. 键
 *      —— 客户说"这个蓝色再深一点"，改一行 ini 重启就能看。
 *   定型之后把这套值固化成一个新的 profile .c，ini 只留现场差异。
 *
 * 【约定】
 *   - UI 层禁止出现任何品牌的字面量：颜色用 oem_color()，文案用 oem_text()，
 *     尺寸用 oem()->layout。
 *   - profile 只放数据，不放逻辑；需要"结构不同"（宫格/列表）用 layout.home_style
 *     选择构建器，而不是在 profile 里塞回调 —— 保持 profile 可被 ini 覆盖。
 */
#ifndef OEM_H
#define OEM_H

#include <stdbool.h>
#include <stdint.h>

/*---------------------------------------------------------------------------
 * 颜色槽位（增删槽位时同步 oem.c 的 key 表）
 *---------------------------------------------------------------------------*/
typedef enum {
    OEM_COLOR_PRIMARY = 0,   /* 品牌主色：图标、选中态、强调 */
    OEM_COLOR_BG,            /* 页面底色 */
    OEM_COLOR_SURFACE,       /* 视频区/大面积面板 */
    OEM_COLOR_CARD,          /* 卡片/按钮底色 */
    OEM_COLOR_BAR,           /* 状态栏/底部条 */
    OEM_COLOR_BAR_TEXT,      /* 状态栏文字 */
    OEM_COLOR_TEXT,          /* 主文字 */
    OEM_COLOR_TEXT_MUTED,    /* 次要文字 */
    OEM_COLOR_BORDER,        /* 描边/分隔 */
    OEM_COLOR_OK,            /* 语义色：接听/成功（一般不随品牌改） */
    OEM_COLOR_DANGER,        /* 语义色：拒接/危险 */
    OEM_COLOR_COUNT
} oem_color_id_t;

/*---------------------------------------------------------------------------
 * 文案槽位
 *   注意：这里出现的每个字都会被 scripts/gen_font.py 收进字库；
 *   若在 ini 里新增了字库里没有的字，需重跑 gen_font.py（文档有说明）。
 *---------------------------------------------------------------------------*/
typedef enum {
    OEM_TXT_PRODUCT = 0,     /* 首页标题，如"我的设备" */
    OEM_TXT_BRAND,           /* 品牌名（标题右侧/关于页） */
    OEM_TXT_SIP_OFFLINE,
    OEM_TXT_SIP_ONLINE,
    OEM_TXT_SIP_CALLING,     /* 呼出中 */
    OEM_TXT_SIP_INCOMING,    /* 来电 */
    OEM_TXT_SIP_INCALL,      /* 通话中 */
    OEM_TXT_SCENE_0,         /* 场景名，与 device_run_scene 下标对应 */
    OEM_TXT_SCENE_1,
    OEM_TXT_SCENE_2,
    OEM_TXT_SCENE_3,
    OEM_TXT_ROOM_0,          /* 房间页签，与 room_t 对应 */
    OEM_TXT_ROOM_1,
    OEM_TXT_ROOM_ALL,
    OEM_TXT_CAT_LIGHT,       /* 分类名，与 device_type_t 对应 */
    OEM_TXT_CAT_CURTAIN,
    OEM_TXT_CAT_AC,
    OEM_TXT_CAT_FRESH,
    OEM_TXT_CAT_PLUG,
    OEM_TXT_CAT_CAM,
    OEM_TXT_COUNT
} oem_text_id_t;

/* 首页结构：不同客户的信息架构可能完全不同 */
typedef enum {
    OEM_HOME_GRID = 0,       /* 宫格（卡片）—— 大屏、触摸 */
    OEM_HOME_LIST,           /* 列表（整行）—— 小屏、旋钮/遥控操作 */
} oem_home_style_t;

typedef struct {
    /* 结构类 */
    oem_home_style_t home_style;
    int16_t bar_h;           /* 状态栏高度 */
    int16_t title_h;         /* 标题行高度 */
    int16_t scene_h;         /* 场景条高度 */
    int16_t card_w;          /* 宫格卡片宽（LIST 风格时表示行高参考） */
    int16_t card_h;
    int16_t gap;             /* 卡片间距 */
    int16_t radius;          /* 圆角 */
    int16_t pad;             /* 页面内边距 */

    /* 开关类：有的客户不要场景条/不要时钟 */
    bool show_scene_bar;
    bool show_clock;
    bool show_brand;
    bool show_voice_bar;
} oem_layout_t;

/* 功能开关（与 layout 的显示开关区分：这里决定"有没有这个功能"） */
#define OEM_CAP_SIP      (1u << 0)
#define OEM_CAP_VOICE    (1u << 1)
#define OEM_CAP_VIDEO    (1u << 2)

typedef struct {
    const char     *id;          /* "default" / "alt"，OEM_PROFILE 用它选 */
    const char     *vendor;      /* 公司名（关于页/日志） */
    const char     *model;       /* 型号 */
    const char     *version;     /* 软件版本 */
    uint32_t        colors[OEM_COLOR_COUNT];
    const char     *text[OEM_TXT_COUNT];
    oem_layout_t    layout;
    uint32_t        caps;
} oem_profile_t;

/*---------------------------------------------------------------------------
 * API
 *---------------------------------------------------------------------------*/

/* 初始化：选 profile（OEM_PROFILE 环境变量 > oem.ini 的 profile= > 编译默认），
   再套用 oem.ini 里的 color./text./layout. 覆盖。界面创建前调用一次。 */
void oem_init(void);

const oem_profile_t *oem(void);
uint32_t             oem_color(oem_color_id_t id);
const char          *oem_text(oem_text_id_t id);
const char          *oem_id(void);
bool                 oem_has(uint32_t cap);

/* 把当前生效配置打印出来 —— 现场验客户机器是哪套配置时非常有用 */
void oem_dump(void);

/* 供测试/OEM 工具使用：按 id 切换（会重新读 ini），返回是否成功 */
bool oem_select(const char *id);

#endif /* OEM_H */
