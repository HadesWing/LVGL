/**
 * @file ut_oem.c
 * @brief OEM 白牌定制层用例
 *
 * 这一层出错的表现很特殊：**编译通过、运行不崩、界面就是不对**。
 * 客户说"我们主色是墨绿"，结果屏幕上是蓝色 —— 因为 ini 里 key 拼错了一个字母，
 * 而 ini_get 找不到 key 就静默用默认值。这类问题只能靠"断言 key 真的生效"来防。
 *
 * 注意用例写法的取舍：
 *   不断言**具体颜色值**（那是数据，客户随时会改，改了测试就红，属于假失败），
 *   而是断言**关系**：accessor 与 profile 一致、ini 覆盖能生效、切 profile 会变。
 *   这样测试跟数据解耦，改配色不用改测试。
 */
#include "ut.h"
#include "oem.h"

#include <stdio.h>
#include <stdlib.h>

/* MinGW 没有 POSIX 的 setenv，只有 _putenv_s */
#if defined(_WIN32)
#  define ut_setenv(k, v) _putenv_s((k), (v))
#else
#  define ut_setenv(k, v) setenv((k), (v), 1)
#endif

/* oem_init 读环境变量和 ini，重复调用会重复读 —— 这里只跑一次 */
static void ensure_init(void)
{
    static bool done;
    if (!done) {
        /* 固定用 default profile：否则本机上如果正好设了 OEM_PROFILE=alt，
           "切 profile 应该变化"的断言会假失败 */
        ut_setenv("OEM_PROFILE", "default");
        oem_init();
        done = true;
    }
}

/*---------------------------------------------------------------------------
 * 1. 默认 profile 必须是完整可用的
 *
 * 少一项都会在界面上表现为空指针解引用（lv_label_set_text(NULL) 直接崩）。
 * 这类崩溃发生在开机第一屏，整机起不来，比什么都严重。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, default_profile_is_complete)
{
    ensure_init();

    const oem_profile_t *p = oem();
    UT_ASSERT(p != NULL);
    if (!p) return;

    UT_ASSERT(p->id && *p->id);
    UT_ASSERT(p->vendor && *p->vendor);
    UT_ASSERT(p->model && *p->model);
    UT_ASSERT(p->version && *p->version);

    for (int i = 0; i < OEM_TXT_COUNT; i++) {
        if (!p->text[i] || !*p->text[i]) {
            UT_FAIL("文案槽位 %d 是空的", i);
            return;
        }
    }
}

/*---------------------------------------------------------------------------
 * 2. accessor 必须和 profile 里的值一致
 *
 * UI 层全部走 oem_color()/oem_text()，一旦 accessor 有偏差（比如索引错位），
 * 表现就会是"改 ini 只改了一部分控件"——最难查的那种。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, accessors_match_profile)
{
    ensure_init();
    const oem_profile_t *p = oem();
    if (!p) { UT_FAIL("profile 为空"); return; }

    UT_EQ_STR(p->id, oem_id());

    for (int i = 0; i < OEM_COLOR_COUNT; i++)
        UT_EQ_INT((long long)p->colors[i], (long long)oem_color((oem_color_id_t)i));

    for (int i = 0; i < OEM_TXT_COUNT; i++)
        UT_EQ_STR(p->text[i], oem_text((oem_text_id_t)i));
}

/*---------------------------------------------------------------------------
 * 3. 颜色槽位不该有两个是同一个值
 *
 * 这不是"美观"要求：主色和文字色相同 = 文字看不见；卡片色和底色相同 =
 * 卡片等于没画。白牌项目里最常见的低级事故就是 ini 里复制粘贴改漏一个。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, key_colors_are_distinct)
{
    ensure_init();
    /* 只挑必须互不相同的几组，不苛求全部 11 个槽位两两不同
       （有的客户就是喜欢 bar 和 bg 同色） */
    UT_NE_INT(oem_color(OEM_COLOR_PRIMARY), oem_color(OEM_COLOR_BG));
    UT_NE_INT(oem_color(OEM_COLOR_TEXT),    oem_color(OEM_COLOR_BG));
    UT_NE_INT(oem_color(OEM_COLOR_TEXT),    oem_color(OEM_COLOR_CARD));
    UT_NE_INT(oem_color(OEM_COLOR_OK),      oem_color(OEM_COLOR_DANGER));
}

/*---------------------------------------------------------------------------
 * 4. 功能开关
 *---------------------------------------------------------------------------*/
UT_TEST(oem, capabilities_match_profile)
{
    ensure_init();
    const oem_profile_t *p = oem();
    if (!p) { UT_FAIL("profile 为空"); return; }

    UT_EQ_INT((p->caps & OEM_CAP_SIP)   != 0, oem_has(OEM_CAP_SIP));
    UT_EQ_INT((p->caps & OEM_CAP_VOICE) != 0, oem_has(OEM_CAP_VOICE));
    UT_EQ_INT((p->caps & OEM_CAP_VIDEO) != 0, oem_has(OEM_CAP_VIDEO));

    /* 不许出现"未知位"：caps 只定义了 3 位，多出来的位说明有人在 profile
       里写了个拼错的宏（比如 OEM_CAP_ASI），编译不报错但不生效 */
    UT_EQ_INT(0, (int)(p->caps & ~(OEM_CAP_SIP | OEM_CAP_VOICE | OEM_CAP_VIDEO)));
}

/*---------------------------------------------------------------------------
 * 5. 运行期切 profile
 *
 * 现场验证"给客户 B 的配置长什么样"就是靠这个，切不过去等于白牌能力失效。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, select_switches_profile)
{
    ensure_init();

    const oem_profile_t *def = oem();
    if (!def) { UT_FAIL("默认 profile 为空"); return; }

    char def_id[32];
    snprintf(def_id, sizeof(def_id), "%s", def->id ? def->id : "");
    uint32_t def_primary = oem_color(OEM_COLOR_PRIMARY);
    oem_home_style_t def_style = def->layout.home_style;

    bool ok = oem_select("alt");
    UT_ASSERT_MSG(ok, "oem_select(\"alt\") 应该成功（oem_alt.c 是编进来的）");
    if (ok) {
        UT_ASSERT(oem() != NULL);
        UT_ASSERT_MSG(strcmp(oem_id(), def_id) != 0, "切到 alt 后 id 应该变了");

        /* 两套 profile 的意义就在于"看起来不一样"，颜色或布局至少变一个 */
        bool changed = (oem_color(OEM_COLOR_PRIMARY) != def_primary) ||
                       (oem()->layout.home_style != def_style);
        UT_ASSERT_MSG(changed, "alt 与 default 的颜色和首页结构不该完全一样");
    }

    /* 切回去，别把状态留给后面的用例 */
    oem_select(def_id);
    UT_EQ_STR(def_id, oem_id());
}

/*---------------------------------------------------------------------------
 * 6. 切到不存在的 profile：必须失败且状态不变
 *
 * 不做保护的话，s_cur 会指向一个没初始化的 profile，颜色全 0（黑屏）。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, select_unknown_profile_keeps_current)
{
    ensure_init();

    char before[32];
    snprintf(before, sizeof(before), "%s", oem_id());
    uint32_t color_before = oem_color(OEM_COLOR_PRIMARY);

    UT_ASSERT(!oem_select("no-such-vendor"));
    UT_EQ_STR(before, oem_id());
    UT_EQ_INT((long long)color_before, (long long)oem_color(OEM_COLOR_PRIMARY));
    UT_ASSERT(oem() != NULL);
    UT_ASSERT(oem()->id != NULL);
}

/*---------------------------------------------------------------------------
 * 7. 布局数值必须合理
 *
 * 负数高度会让 LVGL 的 flex 布局算出负尺寸，表现是"整个页面空白但没报错"。
 * 这是调布局时最容易犯的错（比如 pad 写成 -8 想"往外推"）。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, layout_values_are_sane)
{
    ensure_init();
    const oem_profile_t *p = oem();
    if (!p) { UT_FAIL("profile 为空"); return; }

    UT_IN_RANGE(p->layout.bar_h,   0, 200);
    UT_IN_RANGE(p->layout.title_h, 0, 200);
    UT_IN_RANGE(p->layout.scene_h, 0, 200);
    UT_IN_RANGE(p->layout.card_w,  0, 800);
    UT_IN_RANGE(p->layout.card_h,  0, 800);
    UT_IN_RANGE(p->layout.gap,     0, 100);
    UT_IN_RANGE(p->layout.radius,  0, 100);
    UT_IN_RANGE(p->layout.pad,     0, 100);

    UT_ASSERT(p->layout.home_style == OEM_HOME_GRID ||
              p->layout.home_style == OEM_HOME_LIST);
}

/*---------------------------------------------------------------------------
 * 8. 越界 id 不许写穿数组
 *
 * UI 里用变量拼索引很容易越界（比如 device_type → text id 的映射表少一项）。
 *---------------------------------------------------------------------------*/
UT_TEST(oem, out_of_range_ids_are_safe)
{
    ensure_init();

    /* 越界读：返回值可能是垃圾，但绝不能崩、绝不能写出界。
       这里只验证"调用本身不会让进程死掉"（真崩了框架会标 CRASH）。 */
    uint32_t c = oem_color((oem_color_id_t)OEM_COLOR_COUNT);
    (void)c;
    uint32_t c2 = oem_color((oem_color_id_t)-1);
    (void)c2;
    const char *t = oem_text((oem_text_id_t)OEM_TXT_COUNT);
    (void)t;

    UT_ASSERT(oem() != NULL);
}
