/* test_wake_norm.c - 用真实识别结果验证 C 版归一逻辑与 Python 版一致
 * 编译: gcc -O2 -I. -o test_wake_norm test_wake_norm.c wake_norm.c
 * 只输出 ASCII 结果，避免终端编码问题 */
#include "wake_norm.h"
#include <stdio.h>
#include <string.h>

/* 词表与 Python 侧 keys 列表语义一致：任一命中即算唤醒 */
static const char *const W_XZ[] = {"\xE4\xBD\xA0\xE5\xA5\xBD\xE5\xB0\x8F\xE6\x99\xBA",
                                   "\xE5\xB0\x8F\xE6\x99\xBA"};           /* 你好小智 / 小智 */
static const char *const W_XX[] = {"\xE4\xBD\xA0\xE5\xA5\xBD\xE5\x85\xAE\xE5\x85\xAE",
                                   "\xE5\x85\xAE\xE5\x85\xAE"};           /* 你好兮兮 / 兮兮 */

typedef struct {
    const char *text;
    int which;          /* 0 = 小智词表, 1 = 兮兮词表 */
    int expect;
    const char *note;
} tc_t;

static const tc_t CASES[] = {
    /* --- 小智：真实识别结果 --- */
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xE5\xB0\x8F\xE5\xBF\x97", 0, 1, "nizhi: xiao-zhi -> xiao-zhi (同音)"},
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xE5\xB0\x8F\xE5\x90\x83", 0, 1, "xiaochi -> xiaozhi (锚点近音)"},
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xE6\x83\xB3\xE5\x90\x83", 0, 1, "xiangchi -> xiaozhi (锚点近音)"},
    {"\xE9\x82\xA3\xE5\x8F\xAB\xE5\xB0\x8F\xE6\x99\xBA", 0, 1, "na jiao xiao-zhi (已正确)"},
    /* --- 小智：锚点保护，不能误唤醒 --- */
    {"\xE6\x88\x91\xE6\x83\xB3\xE5\x90\x83\xE9\xA5\xAD", 0, 0, "wo xiang chi fan -> 不能命中"},
    {"\xE6\x89\x93\xE5\xBC\x80\xE5\xAE\xA2\xE5\x8E\x85\xE7\x9A\x84\xE7\x81\xAF", 0, 0, "负样本指令"},
    /* --- 兮兮：真实识别结果 --- */
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE8\xA5\xBF\xE8\xA5\xBF\xE3\x80\x82", 1, 1, "nihao, xixi. -> 命中（带标点）"},
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE5\x98\xBB\xE5\x98\xBB\xE3\x80\x82", 1, 1, "nihao, xixi(嘻) -> 命中"},
    {"\xE4\xBD\xA0\xE8\xA1\x8C\xE5\x98\xBB\xE5\x98\xBB\xE3\x80\x82", 1, 1, "nixing xixi -> nihao xixi (锚点)"},
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE8\xB0\xA2\xE8\xB0\xA2\xE3\x80\x82", 1, 1, "nihao, xiexie -> 命中（锚点）"},
    {"\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xBA\xB2\xE3\x80\x82", 1, 0, "nihao qin -> 真失败"},
    {"", 1, 0, "空串"},
    {".", 1, 0, "只有标点"},
    /* --- 边界：长文本里夹唤醒词 --- */
    {"\xE5\x96\x82\xEF\xBC\x8C\xE4\xBD\xA0\xE5\xA5\xBD\xE5\xB0\x8F\xE5\xBF\x97\xEF\xBC\x8C\xE5\xB8\xAE\xE6\x88\x91\xE5\xBC\x80\xE7\x81\xAF", 0, 1, "长句中间夹唤醒词"},
    {"\xE6\x82\xA8\xE5\xA5\xBD\xE5\xB0\x8F\xE5\x90\x83", 0, 1, "\u60a8\u597d\u5c0f\u5403 -> \u4f60\u597d\u5c0f\u667a(\u6d41\u5f0fVAD\u5b9e\u6d4b)"},
    {"\xE6\x82\xA8\xE5\xA5\xBD\xE5\xAD\xA6\xE4\xB9\xA0", 1, 1, "\u60a8\u597d\u5b66\u4e60 -> \u4f60\u597d\u516e\u516e"},
};

int main(void)
{
    int pass = 0, fail = 0;
    const size_t n = sizeof(CASES) / sizeof(CASES[0]);
    for (size_t i = 0; i < n; ++i) {
        const char *const *words = CASES[i].which == 0 ? W_XZ : W_XX;
        const size_t nw = CASES[i].which == 0 ? sizeof(W_XZ) / sizeof(W_XZ[0])
                                              : sizeof(W_XX) / sizeof(W_XX[0]);
        int got = wake_is_wake_any(CASES[i].text, words, nw);
        if (got == CASES[i].expect) {
            ++pass;
        } else {
            ++fail;
            printf("FAIL #%u expect=%d got=%d  [%s]\n",
                   (unsigned)i, CASES[i].expect, got, CASES[i].note);
        }
        /* 打印归一后的文本，用 UTF-8 十六进制，避免终端编码干扰 */
        char buf[256];
        const size_t len = strlen(CASES[i].text);
        if (len < sizeof(buf)) {
            memcpy(buf, CASES[i].text, len + 1);
            wake_norm_inplace(buf);
            printf("case %2u got=%d hex_after=", (unsigned)i, got);
            for (size_t k = 0; buf[k]; ++k) printf("%02X", (unsigned char)buf[k]);
            printf("\n");
        }
    }
    printf("\nRESULT: pass=%d fail=%d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
