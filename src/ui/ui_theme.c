/**
 * @file ui_theme.c
 * @brief 主题实现：两套调色板 + 一份样式表
 *
 * 【设计要点（面试可讲）】
 *   - 调色板（palette）与样式（style）分离：换皮只改调色板，控件代码零改动。
 *   - 样式只初始化一次；切换主题时改样式的颜色属性，再让 LVGL 通知所有引用它的对象
 *     重算样式（lv_obj_report_style_change），最后 invalidate 触发重绘。
 *   - 语义色（成功绿/危险红）不随主题变 —— 用户对"绿=接听"的认知不能因为夜间模式改变。
 */
#include "ui_theme.h"

/*---------------------------------------------------------------------------
 * 样式对象实体定义（头文件里只做 extern 声明，实体在这里，全局唯一一份）
 *---------------------------------------------------------------------------*/
lv_style_t st_screen;
lv_style_t st_bar;
lv_style_t st_bar_text;
lv_style_t st_surface;
lv_style_t st_surface_calling;
lv_style_t st_card;
lv_style_t st_text_primary;
lv_style_t st_text_muted;
lv_style_t st_title;
lv_style_t st_btn_common;
lv_style_t st_btn_pressed;
lv_style_t st_btn_ok;
lv_style_t st_btn_danger;
lv_style_t st_btn_label;

typedef struct {
    uint32_t bg;        /* 屏幕底色 */
    uint32_t surface;   /* 卡片/视频区 */
    uint32_t card;      /* 弹窗卡片 */
    uint32_t bar;       /* 状态栏 */
    uint32_t bar_text;  /* 状态栏文字 */
    uint32_t text;      /* 主文字 */
    uint32_t text_muted;/* 次要文字 */
    uint32_t border;    /* 分隔/描边 */
} ui_palette_t;

static const ui_palette_t s_palettes[2] = {
    [UI_THEME_LIGHT] = {
        .bg         = 0xECEFF1,
        .surface    = 0x37474F,
        .card       = 0xFFFFFF,
        .bar        = 0x263238,
        .bar_text   = 0xFFFFFF,
        .text       = 0x263238,
        .text_muted = 0x607D8B,
        .border     = 0xCFD8DC,
    },
    [UI_THEME_DARK] = {
        .bg         = 0x121212,
        .surface    = 0x1E1E1E,
        .card       = 0x263238,
        .bar        = 0x000000,
        .bar_text   = 0xE0E0E0,
        .text       = 0xE0E0E0,
        .text_muted = 0x9E9E9E,
        .border     = 0x333333,
    },
};

/* 语义色（不随主题变化） */
#define COLOR_OK      0x2E7D32   /* 接听 / 成功 */
#define COLOR_DANGER  0xC62828   /* 拒接 / 危险 */

static ui_theme_t s_current = UI_THEME_LIGHT;
static bool s_initialized = false;

/*---------------------------------------------------------------------------
 * 把调色板刷进样式表
 *---------------------------------------------------------------------------*/
static void apply_palette(ui_theme_t theme)
{
    const ui_palette_t *p = &s_palettes[theme];

    lv_style_set_bg_color(&st_screen, lv_color_hex(p->bg));

    lv_style_set_bg_color(&st_bar, lv_color_hex(p->bar));
    lv_style_set_text_color(&st_bar_text, lv_color_hex(p->bar_text));

    lv_style_set_bg_color(&st_surface, lv_color_hex(p->surface));
    lv_style_set_border_color(&st_surface, lv_color_hex(p->border));

    lv_style_set_bg_color(&st_card, lv_color_hex(p->card));

    lv_style_set_text_color(&st_text_primary, lv_color_hex(p->text));
    lv_style_set_text_color(&st_text_muted, lv_color_hex(p->text_muted));
    lv_style_set_text_color(&st_title, lv_color_hex(p->text));

    lv_style_set_border_color(&st_btn_common, lv_color_hex(p->border));

    /* 通知 LVGL：这些样式变了，凡是引用它们的对象都要重算样式 */
    lv_obj_report_style_change(&st_screen);
    lv_obj_report_style_change(&st_bar);
    lv_obj_report_style_change(&st_bar_text);
    lv_obj_report_style_change(&st_surface);
    lv_obj_report_style_change(&st_card);
    lv_obj_report_style_change(&st_text_primary);
    lv_obj_report_style_change(&st_text_muted);
    lv_obj_report_style_change(&st_title);
    lv_obj_report_style_change(&st_btn_common);
}

/*---------------------------------------------------------------------------
 * 样式表定义（结构类属性只设一次）
 *---------------------------------------------------------------------------*/
static void styles_init_once(void)
{
    /* 屏幕：只负责底色 */
    lv_style_init(&st_screen);

    /* 状态栏：通栏、无圆角、无边框、无内边距 */
    lv_style_init(&st_bar);
    lv_style_set_radius(&st_bar, 0);
    lv_style_set_border_width(&st_bar, 0);
    lv_style_set_pad_all(&st_bar, 0);
    lv_style_set_pad_left(&st_bar, 8);
    lv_style_set_pad_right(&st_bar, 8);

    lv_style_init(&st_bar_text);
    lv_style_set_text_font(&st_bar_text, &lv_font_montserrat_14);

    /* 卡片/视频区：圆角 + 细描边 */
    lv_style_init(&st_surface);
    lv_style_set_radius(&st_surface, 8);
    lv_style_set_border_width(&st_surface, 1);

    /* 通话中：绿边框 + 更亮描边（语义状态，用"换样式"而不是改属性） */
    lv_style_init(&st_surface_calling);
    lv_style_set_border_width(&st_surface_calling, 3);
    lv_style_set_border_color(&st_surface_calling, lv_color_hex(COLOR_OK));

    /* 弹窗卡片：大圆角 + 阴影（阴影是"纸片感"的来源） */
    lv_style_init(&st_card);
    lv_style_set_radius(&st_card, 12);
    lv_style_set_border_width(&st_card, 0);
    lv_style_set_shadow_width(&st_card, 40);
    lv_style_set_shadow_opa(&st_card, LV_OPA_40);

    /* 文字 */
    lv_style_init(&st_text_primary);
    lv_style_init(&st_text_muted);
    lv_style_set_text_font(&st_text_muted, &lv_font_montserrat_14);

    lv_style_init(&st_title);
    lv_style_set_text_font(&st_title, &lv_font_montserrat_16);

    /* 按钮公共部分 */
    lv_style_init(&st_btn_common);
    lv_style_set_radius(&st_btn_common, 12);
    lv_style_set_border_width(&st_btn_common, 0);
    lv_style_set_shadow_width(&st_btn_common, 8);
    lv_style_set_shadow_opa(&st_btn_common, LV_OPA_20);

    /* 按下态：下沉 2px —— 状态样式最好的示例（不用写任何事件代码） */
    lv_style_init(&st_btn_pressed);
    lv_style_set_translate_y(&st_btn_pressed, 2);
    lv_style_set_shadow_opa(&st_btn_pressed, LV_OPA_0);

    /* 语义色 */
    lv_style_init(&st_btn_ok);
    lv_style_set_bg_color(&st_btn_ok, lv_color_hex(COLOR_OK));

    lv_style_init(&st_btn_danger);
    lv_style_set_bg_color(&st_btn_danger, lv_color_hex(COLOR_DANGER));

    lv_style_init(&st_btn_label);
    lv_style_set_text_color(&st_btn_label, lv_color_hex(0xFFFFFF));
    lv_style_set_text_font(&st_btn_label, &lv_font_montserrat_14);
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
void ui_theme_init(ui_theme_t theme)
{
    if (!s_initialized) {
        styles_init_once();
        s_initialized = true;
    }
    s_current = theme;
    apply_palette(theme);
}

void ui_theme_toggle(void)
{
    ui_theme_t next = (s_current == UI_THEME_LIGHT) ? UI_THEME_DARK : UI_THEME_LIGHT;
    s_current = next;
    apply_palette(next);

    /* 已经被创建的控件不会自动重绘，主动 invalidate 整个活动屏幕 */
    lv_obj_invalidate(lv_screen_active());
}

ui_theme_t ui_theme_get(void)
{
    return s_current;
}
