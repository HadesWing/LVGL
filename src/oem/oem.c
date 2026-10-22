/**
 * @file oem.c
 * @brief OEM 层实现：profile 选择 + oem.ini 覆盖 + 查询接口
 */
#include "oem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 各 profile 的实体（只含数据，见 oem_default.c / oem_alt.c） */
extern const oem_profile_t oem_profile_default;
extern const oem_profile_t oem_profile_alt;

static const oem_profile_t *s_profiles[] = {
    &oem_profile_default,
    &oem_profile_alt,
};
#define PROFILE_N (sizeof(s_profiles) / sizeof(s_profiles[0]))

static oem_profile_t s_cur;
static bool          s_ready;

/* ini 原文（现场配置文件都很小，整个读进内存足够） */
static char s_ini[8192];
static bool s_ini_loaded;

/* ini 覆盖文案的常驻缓冲：profile.text 指向这里，生命周期覆盖整个进程 */
static char s_text_buf[OEM_TXT_COUNT][96];
static bool s_text_overridden[OEM_TXT_COUNT];

/*---------------------------------------------------------------------------
 * ini 读取（极简 key=value；不引入第三方解析器，嵌入式环境最稳）
 *---------------------------------------------------------------------------*/
static bool ini_load(void)
{
    if (s_ini_loaded) return s_ini[0] != '\0';
    s_ini_loaded = true;

    const char *cands[4];
    int n = 0;
    const char *env = getenv("OEM_INI");
    if (env && *env) cands[n++] = env;
    cands[n++] = "oem.ini";            /* 可执行文件目录（部署时最常用） */
    cands[n++] = "../config/oem.ini";  /* 源码树里调试 */
    cands[n++] = "config/oem.ini";

    for (int i = 0; i < n; i++) {
        FILE *f = fopen(cands[i], "rb");
        if (!f) continue;
        size_t rd = fread(s_ini, 1, sizeof(s_ini) - 1, f);
        fclose(f);
        s_ini[rd] = '\0';
        printf("[oem] ini: %s (%u bytes)\n", cands[i], (unsigned)rd);
        return true;
    }
    return false;
}

/* 找 key 并复制 value（去掉行尾空白与引号）；返回是否找到 */
static bool ini_get(const char *key, char *out, size_t outsz)
{
    const char *p = s_ini;
    size_t klen = strlen(key);

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* 跳过注释与空行 */
        const char *line = p;
        size_t i = 0;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < len && line[i] != '#' && line[i] != ';' && line[i] != '\r') {
            const char *l = line + i;
            if (strncmp(l, key, klen) == 0) {
                const char *q = l + klen;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '=') {
                    q++;
                    while (*q == ' ' || *q == '\t') q++;
                    const char *end = p + len;
                    while (end > q && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
                    size_t vlen = (size_t)(end - q);
                    if (vlen >= outsz) vlen = outsz - 1;
                    memcpy(out, q, vlen);
                    out[vlen] = '\0';
                    return true;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return false;
}

static int ini_get_int(const char *key, int def)
{
    char v[32];
    if (!ini_get(key, v, sizeof(v))) return def;
    return (int)strtol(v, NULL, 0);
}

static bool ini_get_bool(const char *key, bool def)
{
    char v[16];
    if (!ini_get(key, v, sizeof(v))) return def;
    if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "no")) return false;
    if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes")) return true;
    return def;
}

/*---------------------------------------------------------------------------
 * 键名表：新增颜色/文案时在这里加一行即可被 ini 覆盖
 *---------------------------------------------------------------------------*/
static const struct { const char *key; oem_color_id_t id; } s_color_keys[] = {
    { "color.primary",    OEM_COLOR_PRIMARY    },
    { "color.bg",         OEM_COLOR_BG         },
    { "color.surface",    OEM_COLOR_SURFACE    },
    { "color.card",       OEM_COLOR_CARD       },
    { "color.bar",        OEM_COLOR_BAR        },
    { "color.bar_text",   OEM_COLOR_BAR_TEXT   },
    { "color.text",       OEM_COLOR_TEXT       },
    { "color.text_muted", OEM_COLOR_TEXT_MUTED },
    { "color.border",     OEM_COLOR_BORDER     },
    { "color.ok",         OEM_COLOR_OK         },
    { "color.danger",     OEM_COLOR_DANGER     },
};

static const struct { const char *key; oem_text_id_t id; } s_text_keys[] = {
    { "text.product",      OEM_TXT_PRODUCT      },
    { "text.brand",        OEM_TXT_BRAND        },
    { "text.sip_offline",  OEM_TXT_SIP_OFFLINE  },
    { "text.sip_online",   OEM_TXT_SIP_ONLINE   },
    { "text.sip_calling",  OEM_TXT_SIP_CALLING  },
    { "text.sip_incoming", OEM_TXT_SIP_INCOMING },
    { "text.sip_incall",   OEM_TXT_SIP_INCALL   },
    { "text.scene0",       OEM_TXT_SCENE_0      },
    { "text.scene1",       OEM_TXT_SCENE_1      },
    { "text.scene2",       OEM_TXT_SCENE_2      },
    { "text.scene3",       OEM_TXT_SCENE_3      },
    { "text.room0",        OEM_TXT_ROOM_0       },
    { "text.room1",        OEM_TXT_ROOM_1       },
    { "text.room_all",     OEM_TXT_ROOM_ALL     },
    { "text.cat_light",    OEM_TXT_CAT_LIGHT    },
    { "text.cat_curtain",  OEM_TXT_CAT_CURTAIN  },
    { "text.cat_ac",       OEM_TXT_CAT_AC       },
    { "text.cat_fresh",    OEM_TXT_CAT_FRESH    },
    { "text.cat_plug",     OEM_TXT_CAT_PLUG     },
    { "text.cat_cam",      OEM_TXT_CAT_CAM      },
};

/*---------------------------------------------------------------------------
 * 应用 ini 覆盖
 *---------------------------------------------------------------------------*/
static void apply_ini(void)
{
    if (!ini_load()) {
        printf("[oem] 未找到 oem.ini（只使用编译期 profile）\n");
        return;
    }

    /* 和 s_text_buf 同宽（96）。原来这里写 128，ini 里超过 95 字节的文案会被
       snprintf 截断在目标缓冲里 —— 而中文是 3 字节/字，截在字中间就是半个字符，
       LVGL 渲染出来是方框。宁可在读取端就限制长度，让超长的配置明确失效。 */
    char v[sizeof(s_text_buf[0])];

    for (unsigned i = 0; i < sizeof(s_color_keys) / sizeof(s_color_keys[0]); i++) {
        if (ini_get(s_color_keys[i].key, v, sizeof(v)))
            s_cur.colors[s_color_keys[i].id] = (uint32_t)strtoul(v, NULL, 0);
    }
    for (unsigned i = 0; i < sizeof(s_text_keys) / sizeof(s_text_keys[0]); i++) {
        if (ini_get(s_text_keys[i].key, v, sizeof(v))) {
            oem_text_id_t id = s_text_keys[i].id;
            snprintf(s_text_buf[id], sizeof(s_text_buf[id]), "%s", v);
            s_text_overridden[id] = true;
            s_cur.text[id] = s_text_buf[id];
        }
    }

    /* 布局：只在 ini 明确写了时才改，保持 profile 默认值 */
    s_cur.layout.bar_h          = (int16_t)ini_get_int("layout.bar_h",          s_cur.layout.bar_h);
    s_cur.layout.title_h        = (int16_t)ini_get_int("layout.title_h",        s_cur.layout.title_h);
    s_cur.layout.scene_h        = (int16_t)ini_get_int("layout.scene_h",        s_cur.layout.scene_h);
    s_cur.layout.card_w         = (int16_t)ini_get_int("layout.card_w",         s_cur.layout.card_w);
    s_cur.layout.card_h         = (int16_t)ini_get_int("layout.card_h",         s_cur.layout.card_h);
    s_cur.layout.gap            = (int16_t)ini_get_int("layout.gap",            s_cur.layout.gap);
    s_cur.layout.radius         = (int16_t)ini_get_int("layout.radius",         s_cur.layout.radius);
    s_cur.layout.pad            = (int16_t)ini_get_int("layout.pad",            s_cur.layout.pad);
    s_cur.layout.show_scene_bar = ini_get_bool("layout.show_scene_bar", s_cur.layout.show_scene_bar);
    s_cur.layout.show_clock     = ini_get_bool("layout.show_clock",     s_cur.layout.show_clock);
    s_cur.layout.show_brand     = ini_get_bool("layout.show_brand",     s_cur.layout.show_brand);
    s_cur.layout.show_voice_bar = ini_get_bool("layout.show_voice_bar", s_cur.layout.show_voice_bar);

    if (ini_get("layout.home_style", v, sizeof(v))) {
        if (!strcmp(v, "list")) s_cur.layout.home_style = OEM_HOME_LIST;
        else                    s_cur.layout.home_style = OEM_HOME_GRID;
    }

    if (ini_get("caps", v, sizeof(v))) {
        uint32_t caps = 0;
        if (strstr(v, "sip"))   caps |= OEM_CAP_SIP;
        if (strstr(v, "voice")) caps |= OEM_CAP_VOICE;
        if (strstr(v, "video")) caps |= OEM_CAP_VIDEO;
        s_cur.caps = caps;
    }
}

/*---------------------------------------------------------------------------
 * 选择 profile
 *---------------------------------------------------------------------------*/
static const oem_profile_t *profile_by_id(const char *id)
{
    for (unsigned i = 0; i < PROFILE_N; i++)
        if (!strcmp(s_profiles[i]->id, id)) return s_profiles[i];
    return NULL;
}

static const oem_profile_t *pick_profile(void)
{
    char v[64];

    /* ① 环境变量（现场排查/批量验证最方便，不改任何文件） */
    const char *env = getenv("OEM_PROFILE");
    if (env && *env) {
        const oem_profile_t *p = profile_by_id(env);
        if (p) return p;
        printf("[oem] OEM_PROFILE=%s 不存在，回落默认\n", env);
    }
    /* ② ini 里的 profile= */
    if (ini_load() && ini_get("profile", v, sizeof(v))) {
        const oem_profile_t *p = profile_by_id(v);
        if (p) return p;
        printf("[oem] oem.ini profile=%s 不存在，回落默认\n", v);
    }
    /* ③ 编译期默认 */
#ifdef OEM_DEFAULT_PROFILE
    {
        const oem_profile_t *p = profile_by_id(OEM_DEFAULT_PROFILE);
        if (p) return p;
    }
#endif
    return &oem_profile_default;
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
void oem_init(void)
{
    const oem_profile_t *base = pick_profile();

    s_cur = *base;                 /* 结构体整体拷贝，ini 只改这份副本 */
    memset(s_text_overridden, 0, sizeof(s_text_overridden));
    apply_ini();

    s_ready = true;
    printf("[oem] profile=%s vendor=%s model=%s\n", s_cur.id, s_cur.vendor, s_cur.model);
    fflush(stdout);
}

bool oem_select(const char *id)
{
    const oem_profile_t *p = id ? profile_by_id(id) : NULL;
    if (!p) return false;

    s_cur = *p;
    /* 不用在这里"恢复 ini 覆盖过的文案"：下面的 apply_ini() 会重新读 ini
       并把命中项重新挂到 s_text_buf 上。曾经这里有个"清零后检查标志"的循环，
       条件永远不成立，是纯死代码 —— 留着只会让后来的人以为流程有玄机。 */
    memset(s_text_overridden, 0, sizeof(s_text_overridden));
    apply_ini();
    return true;
}

const oem_profile_t *oem(void) { return &s_cur; }

uint32_t oem_color(oem_color_id_t id)
{
    if (!s_ready) oem_init();
    if ((unsigned)id >= OEM_COLOR_COUNT) return 0;
    return s_cur.colors[id];
}

const char *oem_text(oem_text_id_t id)
{
    if (!s_ready) oem_init();
    if ((unsigned)id >= OEM_TXT_COUNT) return "";
    return s_cur.text[id] ? s_cur.text[id] : "";
}

const char *oem_id(void) { return s_cur.id ? s_cur.id : "default"; }

bool oem_has(uint32_t cap) { return (s_cur.caps & cap) != 0; }

void oem_dump(void)
{
    printf("---- OEM 生效配置 ----\n");
    printf("  id      : %s\n", s_cur.id);
    printf("  vendor  : %s\n", s_cur.vendor);
    printf("  model   : %s  ver %s\n", s_cur.model, s_cur.version);
    printf("  style   : %s\n", s_cur.layout.home_style == OEM_HOME_LIST ? "list" : "grid");
    printf("  colors  : primary=%06X bg=%06X card=%06X text=%06X\n",
           (unsigned)s_cur.colors[OEM_COLOR_PRIMARY], (unsigned)s_cur.colors[OEM_COLOR_BG],
           (unsigned)s_cur.colors[OEM_COLOR_CARD],    (unsigned)s_cur.colors[OEM_COLOR_TEXT]);
    printf("  layout  : bar=%d card=%dx%d gap=%d radius=%d pad=%d scene_bar=%d\n",
           s_cur.layout.bar_h, s_cur.layout.card_w, s_cur.layout.card_h,
           s_cur.layout.gap, s_cur.layout.radius, s_cur.layout.pad,
           (int)s_cur.layout.show_scene_bar);
    printf("  caps    : sip=%d voice=%d video=%d\n",
           oem_has(OEM_CAP_SIP), oem_has(OEM_CAP_VOICE), oem_has(OEM_CAP_VIDEO));
    printf("  product : %s\n", s_cur.text[OEM_TXT_PRODUCT]);
    printf("----------------------\n");
    fflush(stdout);
}
