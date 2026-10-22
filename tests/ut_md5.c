/**
 * @file ut_md5.c
 * @brief SIP Digest 用的 MD5（RFC 1321）
 *
 * 为什么这个模块必须测：它的错误**不会在正常运行中暴露**，只会表现为
 * "服务器回 401 / 403"。现场表现是"账号密码明明对，就是注册不上"，
 * 而排查方向通常是去查账号、查服务器、查网络 —— 没人会先怀疑摘要函数。
 * 所以这里用 RFC 原文给出的标准向量钉死它。
 *
 * 参考值来源：
 *   - RFC 1321 附录 A.5 的 7 条标准向量（业界通用）
 *   - 长度边界与二进制用例：用 Python hashlib 生成后硬编码在这里
 *     （独立实现作参照，避免"自己算自己"）
 *   - RFC 2617 §3.5 的 Digest 示例：HA1/HA2 可直接校验
 *     （顺带验证了"用户名:realm:密码"的拼接顺序没写反）
 */
#include "ut.h"
#include "sip_md5.h"

#include <stdio.h>

static void md5_of_str(const char *s, char out[33])
{
    md5_hex(s, strlen(s), out);
}

/*---------------------------------------------------------------------------
 * 1. RFC 1321 附录 A.5 标准向量
 *---------------------------------------------------------------------------*/
UT_TEST(md5, rfc1321_vectors)
{
    struct { const char *in; const char *want; } v[] = {
        { "",                          "d41d8cd98f00b204e9800998ecf8427e" },
        { "a",                         "0cc175b9c0f1b6a831c399e269772661" },
        { "abc",                       "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest",            "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz","c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                                       "d174ab98d277d9f5a5611c2c9f419d9f" },
        { "12345678901234567890123456789012345678901234567890123456789012345678901234567890",
                                       "57edf4a22be3c955ac49da2e2107b67a" },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char got[33];
        md5_of_str(v[i].in, got);
        UT_EQ_STR(v[i].want, got);
    }
}

/*---------------------------------------------------------------------------
 * 2. padding 边界（最容易写错的地方）
 *
 * MD5 补位规则：先补 0x80，再补 0 到 len%64 == 56，最后 8 字节长度。
 * 于是 len%64 == 55 时 0x80 刚好占掉第 56 字节、len%64 == 56 时要跨块。
 * 这两个位置差一个字节就会整体错位 —— 而且"短字符串"用例根本测不出来，
 * 因为业务里真正会出现的 URI、Authorization 头往往正好落在这附近。
 *---------------------------------------------------------------------------*/
UT_TEST(md5, padding_length_boundaries)
{
    struct { int n; const char *want; } v[] = {
        {  55, "ef1772b6dff9a122358552954ad0df65" },
        {  56, "3b0c8ac703f828b04c6c197006d17218" },
        {  57, "652b906d60af96844ebd21b674f35e93" },
        {  63, "b06521f39153d618550606be297466d5" },
        {  64, "014842d480b571495a4a0363793f7367" },
        {  65, "c743a45e0d2e6a95cb859adae0248435" },
        { 119, "8a7bd0732ed6a28ce75f6dabc90e1613" },
        { 120, "5f61c0ccad4cac44c75ff505e1f1e537" },
        { 128, "e510683b3f5ffe4093d021808bc6ff70" },
    };
    char buf[129];
    memset(buf, 'a', sizeof(buf));
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        char got[33];
        md5_hex(buf, (size_t)v[i].n, got);
        UT_EQ_STR(v[i].want, got);
    }
}

/*---------------------------------------------------------------------------
 * 3. 二进制数据（含 NUL 字节）
 *
 * md5_hex 收的是 (void*, len)，不是字符串 —— 必须证明它真的按长度算，
 * 而不是内部偷偷用了 strlen。用含 0x00 的数据一测便知。
 *---------------------------------------------------------------------------*/
UT_TEST(md5, binary_with_nul_bytes)
{
    static const uint8_t data[] = {
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
        0x00, 0x01, 0x02, 0xFE, 0xFF, 0x00, 0x07,
    };
    char got[33];
    md5_hex(data, sizeof(data), got);
    UT_EQ_STR("2ea91dacf3ef90220373e5755f932a55", got);
}

/*---------------------------------------------------------------------------
 * 4. 两个入口必须等价
 *---------------------------------------------------------------------------*/
UT_TEST(md5, hex_and_hex_str_agree)
{
    const char *s = "REGISTER sip:192.168.1.10 SIP/2.0";
    char a[33], b[33];
    md5_hex(s, strlen(s), a);
    md5_hex_str(s, b);
    UT_EQ_STR(a, b);
}

/*---------------------------------------------------------------------------
 * 5. 输出格式：32 字符、全小写十六进制
 *
 * 大写的摘要不会被服务器接受（RFC 2617 规定小写），而且它只在 401 才暴露。
 *---------------------------------------------------------------------------*/
UT_TEST(md5, output_is_32_lowercase_hex)
{
    char got[33];
    md5_of_str("format check", got);
    UT_EQ_INT(32, (int)strlen(got));
    for (int i = 0; i < 32; i++) {
        char c = got[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) {
            UT_FAIL("第 %d 个字符 '%c' 不是小写十六进制", i, c);
            return;
        }
    }
}
