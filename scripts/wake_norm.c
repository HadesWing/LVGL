#include "wake_norm.h"
#include <string.h>

/* ---------------- 规则表 ----------------
 * 约束：from 与 to 必须是等长字节串（这里全部是 2 个汉字 = 6 字节）。
 * 若新增规则长度不等，strlen 校验会在首次调用时返回 -1 并跳过。*/

typedef struct {
    const char *from;
    const char *to;
} rule_t;

/* 裸替换：这些串不会在日常对话里独立出现，替换安全 */
static const rule_t RULES_HARD[] = {
    /* 兮兮 xi-xi 的同音/近形 */
    {"西西", "兮兮"}, {"嘻嘻", "兮兮"}, {"希希", "兮兮"}, {"夕夕", "兮兮"},
    {"溪溪", "兮兮"}, {"熙熙", "兮兮"}, {"细细", "兮兮"}, {"习习", "兮兮"},
    {"西嘻", "兮兮"},
    /* 小智 zhi 的同音 */
    {"小致", "小智"}, {"小治", "小智"}, {"小志", "小智"}, {"小知", "小智"},
    {"小至", "小智"}, {"小制", "小智"}, {"小值", "小智"}, {"小只", "小智"},
    {"小止", "小智"}, {"晓智", "小智"}, {"筱智", "小智"}, {"小秩", "小智"},
};

/* 锚定近音：必须带「你好 / 你行」前缀。
 * 不加锚点会炸：「我想吃饭」->「我小智饭」-> 误唤醒。*/
static const rule_t RULES_SOFT[] = {
    /* 您 -> 你 必须排在【所有锚点规则之前】。
     * 注意它本身不是"锚点规则"，放这里纯粹是因为执行顺序：
     * 本表整体先于 RULES_HARD 执行，而下面所有规则都以"你好"为锚点。
     * 若把它放进 HARD，等 HARD 跑完"您好学习"已变成"你好学习"，
     * 但 SOFT 早已跑过一轮，锚点规则永远匹配不上（实测 2/2 漏判）。
     * 这是"两级规则 + 顺序依赖"最典型的坑，改规则时别动这个位置。 */
    {"您好", "你好"},
    {"你好小吃", "你好小智"}, {"你好想吃", "你好小智"}, {"你好小气", "你好小智"},
    {"你好谢谢", "你好兮兮"}, {"你行嘻嘻", "你好兮兮"}, {"你好星星", "你好兮兮"},
    {"你好谢西", "你好兮兮"}, {"你好学习", "你好兮兮"},
};

#define N_HARD (sizeof(RULES_HARD) / sizeof(RULES_HARD[0]))
#define N_SOFT (sizeof(RULES_SOFT) / sizeof(RULES_SOFT[0]))

/* 覆盖式原地替换，要求等长 */
static int apply_rules(char *text, const rule_t *rules, size_t n)
{
    int changed = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t fl = strlen(rules[i].from);
        if (fl != strlen(rules[i].to))
            continue; /* 长度不等会破坏缓冲区，跳过 */
        char *p = text;
        while ((p = strstr(p, rules[i].from)) != NULL) {
            memcpy(p, rules[i].to, fl); /* 等长，直接覆盖 */
            p += fl;
            changed = 1;
        }
    }
    return changed;
}

int wake_norm_inplace(char *text)
{
    if (text == NULL) return 0;
    int c = 0;
    /* 顺序关键：SOFT 必须先跑。HARD 里的 "嘻嘻"->"兮兮" 会破坏
     * SOFT 的锚点原文 "你行嘻嘻"，导致锚点规则永远匹配不上。*/
    c |= apply_rules(text, RULES_SOFT, N_SOFT);
    c |= apply_rules(text, RULES_HARD, N_HARD);
    return c;
}

/* ---------------- 去标点比较 ---------------- */

/* 需要跳过的 UTF-8 标点/空白（按字节序列精确匹配，避免误伤汉字） */
static const char *const PUNCT[] = {
    "\xEF\xBC\x8C", /* ， */
    "\xE3\x80\x82", /* 。 */
    "\xEF\xBC\x81", /* ！ */
    "\xEF\xBC\x9F", /* ？ */
    "\xE3\x80\x81", /* 、 */
    "\xEF\xBC\x9B", /* ； */
    "\xEF\xBC\x9A", /* ： */
    "\xE2\x80\x9C", /* “ */
    "\xE2\x80\x9D", /* ” */
    "\xE2\x80\x98", /* ‘ */
    "\xE2\x80\x99", /* ’ */
    "\xEF\xBC\x88", /* （ */
    "\xEF\xBC\x89", /* ） */
    "\xC2\xB7",     /* · */
    "\xE2\x80\xA6", /* … */
    "\xEF\xBC\x8D", /* － */
    " ",
    "\t",
    "\r",
    "\n",
};
#define N_PUNCT (sizeof(PUNCT) / sizeof(PUNCT[0]))

/* 返回 s 处应跳过的字节数，0 表示不是标点 */
static size_t skip_len(const char *s)
{
    for (size_t i = 0; i < N_PUNCT; ++i) {
        const size_t l = strlen(PUNCT[i]);
        if (strncmp(s, PUNCT[i], l) == 0)
            return l;
    }
    return 0;
}

int wake_contains(const char *text, const char *word)
{
    if (text == NULL || word == NULL || *word == '\0') return 0;

    const char *t = text;
    while (*t) {
        /* 从 t 起做一次"跳过标点"的匹配 */
        const char *ti = t, *wi = word;
        while (*wi) {
            size_t sk;
            while ((sk = skip_len(ti)) > 0) ti += sk; /* 跳过文本中的标点 */
            while ((sk = skip_len(wi)) > 0) wi += sk; /* 跳过关键词中的标点 */
            if (*wi == '\0') break;

            /* 比较一个 UTF-8 字符（1~4 字节） */
            unsigned char c = (unsigned char)*ti;
            size_t cl = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
            if ((unsigned char)*wi != c) break;
            if (strncmp(ti, wi, cl) != 0) break;
            ti += cl;
            wi += cl;
        }
        if (*wi == '\0') return 1; /* 关键词走完 = 命中 */
        t += ((unsigned char)*t < 0x80) ? 1 : ((unsigned char)*t < 0xE0) ? 2
                                                                        : ((unsigned char)*t < 0xF0) ? 3 : 4;
    }
    return 0;
}

/* 去掉标点/空白，输出到 out（长度只会变短）。返回写入字节数，失败返回 (size_t)-1 */
static size_t strip_punct(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    const char *p = in;
    while (*p) {
        size_t sk = skip_len(p);
        if (sk > 0) { p += sk; continue; }   /* 标点丢弃 */
        unsigned char c = (unsigned char)*p;
        size_t cl = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (w + cl + 1 > cap) return (size_t)-1;
        memcpy(out + w, p, cl);
        w += cl;
        p += cl;
    }
    out[w] = '\0';
    return w;
}

int wake_is_wake(const char *text, const char *word)
{
    return wake_is_wake_any(text, &word, 1);
}

int wake_is_wake_any(const char *text, const char *const *words, size_t n_words)
{
    if (text == NULL || words == NULL || n_words == 0) return 0;
    char buf[256];
    /* 先去标点：否则 "你好，谢谢" 永远匹配不上锚点规则 "你好谢谢" */
    if (strip_punct(text, buf, sizeof(buf)) == (size_t)-1) return 0;
    wake_norm_inplace(buf);
    for (size_t i = 0; i < n_words; ++i) {
        if (wake_contains(buf, words[i])) return 1;
    }
    return 0;
}
