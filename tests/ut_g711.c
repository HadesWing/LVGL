/**
 * @file ut_g711.c
 * @brief G.711 A-law / μ-law 编解码用例
 *
 * 判据分三层，故意不重写一遍实现来对比（那是同义反复，只能证明"两次写的一样错"）：
 *
 *   1) 标准常数：ITU-T G.711 表里的确定值（静音码、满幅码）。抓符号位错、段号错。
 *   2) 幂等性：dec(enc(dec(c))) == dec(c)。量化器的必要条件 ——
 *      表里任何一个码字写错、或者段边界的 offset 少加，这条必挂。
 *   3) 单调性：同符号内码字递增 → 解码绝对值递增。抓查表次序颠倒。
 *
 * 第 2 条是最有价值的：它不需要参考实现，却能覆盖全部 256 个码字。
 */
#include "ut.h"
#include "g711.h"

#include <math.h>

/*---------------------------------------------------------------------------
 * 1. 标准常数（Sun 参考实现口径，与 ITU G.711 表一致）
 *
 * A-law 的码字不是数值顺序对应幅度：要先 XOR 0x55 才是"线性序号"。
 *   0x2A / 0xAA  → ∓32256  最大幅度
 *   0xD5 / 0x55  → ±8       最小非零幅度（0xD5 是 A-law 的"静音码"）
 * μ-law：
 *   0x00 / 0x80  → ∓32124  最大幅度
 *   0xFF         → 0       静音码
 *---------------------------------------------------------------------------*/
UT_TEST(g711, alaw_standard_limits)
{
    UT_EQ_INT(-32256, g711_alaw2linear(0x2A));
    UT_EQ_INT( 32256, g711_alaw2linear(0xAA));
    UT_EQ_INT(     8, g711_alaw2linear(0xD5));
    UT_EQ_INT(    -8, g711_alaw2linear(0x55));
}

UT_TEST(g711, ulaw_standard_limits)
{
    UT_EQ_INT(-32124, g711_ulaw2linear(0x00));
    UT_EQ_INT( 32124, g711_ulaw2linear(0x80));
    UT_EQ_INT(     0, g711_ulaw2linear(0xFF));
}

/*---------------------------------------------------------------------------
 * 2. 幂等性：覆盖全部 256 个码字
 *---------------------------------------------------------------------------*/
UT_TEST(g711, alaw_quantizer_is_idempotent)
{
    for (int c = 0; c < 256; c++) {
        int16_t once = g711_alaw2linear((uint8_t)c);
        uint8_t back = g711_linear2alaw(once);
        int16_t twice = g711_alaw2linear(back);
        /* 解码→编码→解码，必须回到同一个值：否则说明量化区间有重叠或空洞 */
        if (once != twice) {
            UT_FAIL("码字 0x%02X: dec=%d → enc=0x%02X → dec=%d", c, once, back, twice);
            return;   /* 一个就够说明问题，别刷 256 行 */
        }
    }
}

UT_TEST(g711, ulaw_quantizer_is_idempotent)
{
    for (int c = 0; c < 256; c++) {
        int16_t once = g711_ulaw2linear((uint8_t)c);
        uint8_t back = g711_linear2ulaw(once);
        int16_t twice = g711_ulaw2linear(back);
        if (once != twice) {
            UT_FAIL("码字 0x%02X: dec=%d → enc=0x%02X → dec=%d", c, once, back, twice);
            return;
        }
    }
}

/*---------------------------------------------------------------------------
 * 3. 双射性：每个码字必须对应一个唯一的输出值
 *
 * 这里**刻意不用单调性** —— A-law 的码字本来就是不单调的：
 *   0x80 → 5504,  0x81 → 5248,  0x82 → 6016,  0x83 → 5760 …
 * 这是 XOR 0x55 的直接结果，也是 G.711 的设计意图：让相邻码字（最容易被
 * 噪声翻转的那一位）对应**幅度接近**的值，而不是把误差放大成满幅咔哒声。
 * 换句话说：查到 A-law 不单调就断定实现错了，是错的结论。
 *
 * 真正恒成立的是双射：256 个码字 → 256 个互不相同的量化电平。
 * 只要表里漏一个、重一个，这条必挂。
 *---------------------------------------------------------------------------*/
UT_TEST(g711, alaw_codes_map_to_distinct_levels)
{
    static uint8_t seen[65536];             /* 值 + 32768 作下标，存是哪个码字占的 */

    memset(seen, 0xFF, sizeof(seen));       /* 0xFF = 未占用 */

    for (int c = 0; c < 256; c++) {
        int v = g711_alaw2linear((uint8_t)c);
        int idx = v + 32768;
        if (idx < 1 || idx > 65534) {
            UT_FAIL("码字 0x%02X 解码值 %d 越界", c, v);
            return;
        }
        if (seen[idx] != 0xFF) {
            UT_FAIL("码字 0x%02X 和 0x%02X 都解成 %d（应一一对应）",
                    seen[idx], c, v);
            return;
        }
        seen[idx] = (uint8_t)c;
    }
}

UT_TEST(g711, ulaw_codes_map_to_distinct_levels)
{
    static uint8_t seen[65536];
    memset(seen, 0xFF, sizeof(seen));

    /* μ-law 有两个零：0x7F 和 0xFF 都解成 0（标准如此，不是 bug） */
    int zero_count = 0;
    for (int c = 0; c < 256; c++) {
        int v = g711_ulaw2linear((uint8_t)c);
        int idx = v + 32768;
        if (v == 0) { zero_count++; continue; }
        if (seen[idx] != 0xFF) {
            UT_FAIL("码字 0x%02X 和 0x%02X 都解成 %d", seen[idx], c, v);
            return;
        }
        seen[idx] = (uint8_t)c;
    }
    UT_EQ_INT(2, zero_count);
}

/*---------------------------------------------------------------------------
 * 4. 符号对称：c 与 c^0x80 必须互为相反数
 *
 * 两种编码的最高位都是符号位，且正负半轴用同一套量化级别（G.711 是对称
 * 量化器）。这条能抓住"负半轴用了 val 而不是 -val-8 那类偏移错误" ——
 * 那类错误听感上是"声音发闷 + 偶发咔哒"，很难靠耳朵定位。
 *---------------------------------------------------------------------------*/
UT_TEST(g711, alaw_is_sign_symmetric)
{
    for (int c = 0; c < 128; c++) {
        int neg = g711_alaw2linear((uint8_t)c);
        int pos = g711_alaw2linear((uint8_t)(c | 0x80));
        /* 容差 1：Sun 参考实现在负半轴用 -(val+8)，正半轴用 +val，
           两边可能差一个量化级，这是标准实现的历史约定，不是错。 */
        if (pos + neg < -1 || pos + neg > 1) {
            UT_FAIL("0x%02X(%d) 与 0x%02X(%d) 不是相反数", c, neg, c | 0x80, pos);
            return;
        }
    }
}

UT_TEST(g711, ulaw_is_sign_symmetric)
{
    for (int c = 0; c < 128; c++) {
        int neg = g711_ulaw2linear((uint8_t)c);
        int pos = g711_ulaw2linear((uint8_t)(c | 0x80));
        if (pos + neg < -1 || pos + neg > 1) {
            UT_FAIL("0x%02X(%d) 与 0x%02X(%d) 不是相反数", c, neg, c | 0x80, pos);
            return;
        }
    }
}

/*---------------------------------------------------------------------------
 * 5. μ-law 正半轴严格递减（0x80 最大 → 0xFF 归零）
 *
 * 与 A-law 相反，μ-law 的码字是"反序"的：数值越大越接近零。
 * 这条单独留着，因为"码字方向搞反"是移植 μ-law 时最常见的错误，
 * 表现是声音整体反相 + 电平阶梯倒挂。
 *---------------------------------------------------------------------------*/
UT_TEST(g711, ulaw_positive_half_strictly_decreasing)
{
    int prev = g711_ulaw2linear(0x80);
    for (int c = 0x81; c <= 0xFF; c++) {
        int cur = g711_ulaw2linear((uint8_t)c);
        if (cur >= prev) {
            UT_FAIL("0x%02X 处没有递减：前一个 %d，当前 %d", c, prev, cur);
            return;
        }
        prev = cur;
    }
    UT_EQ_INT(0, prev);   /* 0xFF 必须正好是 0 */
}

/*---------------------------------------------------------------------------
 * 4. 整帧 API 与逐样本 API 必须一致
 *
 * 这两套接口并存是为了性能（整帧省函数调用），但一旦哪天有人只改了其中一套，
 * 表现是"上行声音正常、下行声音失真"这种极难查的 bug。
 *---------------------------------------------------------------------------*/
UT_TEST(g711, frame_api_matches_sample_api)
{
    int16_t pcm[G711_FRAME_SAMPLES];
    uint8_t enc[G711_FRAME_SAMPLES];
    for (int i = 0; i < G711_FRAME_SAMPLES; i++)
        pcm[i] = (int16_t)(12000.0 * sin(2.0 * 3.14159265358979 * i / 32.0));

    g711_encode_alaw(pcm, enc, G711_FRAME_SAMPLES);
    for (int i = 0; i < G711_FRAME_SAMPLES; i++) {
        uint8_t want = g711_linear2alaw(pcm[i]);
        if (enc[i] != want) {
            UT_FAIL("第 %d 个采样：整帧编码 0x%02X，逐样本 0x%02X", i, enc[i], want);
            return;
        }
    }

    /* 解码方向同样要对得上 */
    int16_t dec[G711_FRAME_SAMPLES];
    g711_decode_alaw(enc, dec, G711_FRAME_SAMPLES);
    for (int i = 0; i < G711_FRAME_SAMPLES; i++) {
        int16_t want = g711_alaw2linear(enc[i]);
        if (dec[i] != want) {
            UT_FAIL("第 %d 个采样：整帧解码 %d，逐样本 %d", i, dec[i], want);
            return;
        }
    }
}

/*---------------------------------------------------------------------------
 * 5. 边界与静音
 *---------------------------------------------------------------------------*/
UT_TEST(g711, full_scale_input_does_not_wrap)
{
    /* -32768 取负、abs() 都是经典溢出点；结果必须落在 int16 范围内且方向正确 */
    int16_t lo = g711_alaw2linear(g711_linear2alaw(-32768));
    int16_t hi = g711_alaw2linear(g711_linear2alaw( 32767));
    UT_IN_RANGE(lo, -32256, -30000);
    UT_IN_RANGE(hi,  30000,  32256);
}

UT_TEST(g711, digital_silence_decodes_near_zero)
{
    /* 全 0 输入 → A-law 量化后解码，应回到接近 0（A-law 的零码是 ±8，不是 0） */
    int16_t pcm[160];
    uint8_t enc[160];
    int16_t dec[160];
    memset(pcm, 0, sizeof(pcm));

    g711_encode_alaw(pcm, enc, 160);
    g711_decode_alaw(enc, dec, 160);
    for (int i = 0; i < 160; i++) {
        if (abs(dec[i]) > 16) {
            UT_FAIL("静音第 %d 个采样解码成 %d，超出 ±16", i, dec[i]);
            return;
        }
    }

    /* μ-law 的静音码正好是 0 */
    g711_encode_ulaw(pcm, enc, 160);
    for (int i = 0; i < 160; i++)
        UT_EQ_INT(0xFF, enc[i]);
}

UT_TEST(g711, zero_length_is_safe)
{
    /* n=0 时不许写越界、不许崩（调用方在打断点凑不满一帧时会传 0） */
    int16_t pcm[4] = {1, 2, 3, 4};
    uint8_t enc[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    g711_encode_alaw(pcm, enc, 0);
    UT_EQ_INT(0xAA, enc[0]);   /* 一个字节都不该动 */
    UT_EQ_INT(0xAA, enc[3]);
}

/*---------------------------------------------------------------------------
 * 6. 音质指标：满幅正弦的 SNR 必须达到 G.711 理论值下限
 *
 * 理论 ≈ 38dB。这里取 35dB 作下限 —— 低于这个数说明查表被改坏了，
 * 而不是"信号不理想"。
 *---------------------------------------------------------------------------*/
UT_TEST(g711, alaw_sine_snr_meets_spec)
{
    enum { N = 8000 };
    static int16_t src[N], dec[N];
    static uint8_t enc[N];
    double sig = 0, noise = 0;

    for (int i = 0; i < N; i++)
        src[i] = (int16_t)(20000.0 * sin(2.0 * 3.14159265358979 * 1000.0 * i / 8000.0));

    g711_encode_alaw(src, enc, N);
    g711_decode_alaw(enc, dec, N);

    for (int i = 0; i < N; i++) {
        double s = src[i], d = (double)dec[i] - s;
        sig += s * s;
        noise += d * d;
    }
    double snr = 10.0 * log10(sig / (noise > 0 ? noise : 1));
    UT_ASSERT_MSG(snr >= 35.0, "A-law 满幅正弦 SNR 应 ≥ 35dB");
    if (snr < 35.0) UT_FAIL("实测 SNR = %.2f dB", snr);
}
