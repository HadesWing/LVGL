/**
 * @file ut_wav.c
 * @brief WAV 头读写用例
 *
 * 为什么这个模块值得单独测：WAV 头里字段写错**不会让文件打不开**。
 * 播放器照样播，只是音调不对、速度不对、或者右耳没声。
 * 音频通路自测时"能听见"是最弱的一层判据（见 docs/音频通路自测方案.md），
 * 所以头里每一个字段都必须能从文件里读回来验证。
 *
 * 后半部分专门构造**畸形但真实存在的**头：
 *   - data 长度写 0（推流工具边录边写的常规做法）
 *   - data 长度写 0xFFFFFFFF（占位没回填）
 *   - 中间夹 LIST 块、且长度为奇数（要跳过填充字节，否则后面全部错位）
 *   - fmt 块带 cbSize 扩展（18 字节而不是 16）
 * 这些都是"在别人电脑上能播、在我们这里解析炸"的典型来源。
 */
#include "ut.h"
#include "wav.h"

#include <stdlib.h>

/*---------------------------------------------------------------------------
 * 构造字节流的小工具
 *---------------------------------------------------------------------------*/
typedef struct { uint8_t b[8192]; size_t n; } buf_t;

static void b_tag(buf_t *b, const char *tag)
{
    memcpy(b->b + b->n, tag, 4);
    b->n += 4;
}

static void b_u32(buf_t *b, uint32_t v)
{
    b->b[b->n++] = (uint8_t)v;
    b->b[b->n++] = (uint8_t)(v >> 8);
    b->b[b->n++] = (uint8_t)(v >> 16);
    b->b[b->n++] = (uint8_t)(v >> 24);
}

static void b_u16(buf_t *b, uint16_t v)
{
    b->b[b->n++] = (uint8_t)v;
    b->b[b->n++] = (uint8_t)(v >> 8);
}

static void b_zeros(buf_t *b, size_t n)
{
    memset(b->b + b->n, 0, n);
    b->n += n;
}

/* 标准 fmt 块（16 字节） */
static void b_fmt16(buf_t *b, uint16_t ch, uint32_t rate, uint16_t bits)
{
    b_tag(b, "fmt ");
    b_u32(b, 16);
    b_u16(b, 1);                        /* PCM */
    b_u16(b, ch);
    b_u32(b, rate);
    b_u32(b, rate * ch * (bits / 8));   /* byte rate */
    b_u16(b, (uint16_t)(ch * (bits / 8)));
    b_u16(b, bits);
}

/* 把缓冲区写成临时文件，返回可读 FILE* */
static FILE *to_file(const buf_t *b)
{
    FILE *fp = fopen("ut_tmp.wav", "wb");
    if (!fp) return NULL;
    fwrite(b->b, 1, b->n, fp);
    fclose(fp);
    return fopen("ut_tmp.wav", "rb");
}

static void drop_tmp(void) { remove("ut_tmp.wav"); }

/*---------------------------------------------------------------------------
 * 1. 写出去的头，读回来必须是同一个意思
 *
 * 这条把 wav_write_header 和 wav_parse 互锁：任何一个改了字段位置都会挂。
 * data_offset 必须是 44 —— 标准 PCM 头长度。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, write_then_parse_roundtrip)
{
    uint8_t h[WAV_HEADER_BYTES];
    wav_write_header(h, 8000, 1, 8000);     /* 0.5 秒的数据 */

    buf_t b = {0};
    memcpy(b.b, h, sizeof(h));
    b.n = sizeof(h);
    b_zeros(&b, 8000);                      /* 数据体也得真写，否则解析方按文件长度收口 */

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(WAV_HEADER_BYTES, wi.data_offset);
    UT_EQ_INT(8000, wi.data_bytes);
    UT_EQ_INT(1, wi.channels);
    UT_EQ_INT(8000, wi.rate);
    UT_EQ_INT(16, wi.bits);
}

/*---------------------------------------------------------------------------
 * 2. data 长度写 0 → 按文件实际剩余长度
 *
 * app_audio 落盘时就是这么写的（边写边回填），进程被 kill 时文件里
 * data 长度可能停在 0。这种情况必须还能读。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, zero_data_size_falls_back_to_file_length)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");
    b_fmt16(&b, 1, 8000, 16);
    b_tag(&b, "data");
    b_u32(&b, 0);                     /* 长度未知 */
    b_zeros(&b, 1000);                /* 实际有 1000 字节 */

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(1000, wi.data_bytes);
}

/*---------------------------------------------------------------------------
 * 3. data 长度写 0xFFFFFFFF（占位没回填）→ 收口到实际长度
 *
 * 比"写 0"更阴险：按 4GB 去 malloc / 去读，直接 OOM 或读到堆外。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, oversized_data_size_is_clamped)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");
    b_fmt16(&b, 1, 8000, 16);
    b_tag(&b, "data");
    b_u32(&b, 0xFFFFFFFFu);
    b_zeros(&b, 320);                 /* 实际只有 320 字节 */

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(320, wi.data_bytes);
}

/*---------------------------------------------------------------------------
 * 4. 跳过未知块：LIST 长度是奇数，后面还有 1 个填充字节
 *
 * 不跳填充字节的话，data 块起点会偏 1 字节 —— 表现是"整段音频全是噪声"，
 * 因为每个采样都被拆成了两个字节的前后错位叠加。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, skips_odd_sized_unknown_chunk)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");
    b_fmt16(&b, 1, 8000, 16);

    b_tag(&b, "LIST");
    b_u32(&b, 5);                     /* 奇数长度 */
    b_zeros(&b, 5);
    b_zeros(&b, 1);                   /* 填充字节，对齐到偶数 */

    b_tag(&b, "data");
    b_u32(&b, 160);
    b_zeros(&b, 160);

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(160, wi.data_bytes);
    /* 12（RIFF/WAVE）+ 8 + 16（fmt 块）+ 8 + 5 + 1（LIST 块含填充）+ 8（data 块头）
       = 58。注意不是 44：44 是"RIFF头+fmt+data头"的理想长度，中间夹了 LIST 就不是了。 */
    UT_EQ_INT(58, wi.data_offset);
}

/*---------------------------------------------------------------------------
 * 5. fmt 块带扩展（18 字节，cbSize=0）
 *
 * 微软的 WAVEFORMATEX 就有 cbSize，很多录音软件按 18 字节写。
 * 不多跳这 2 字节，后面的 "data" 标签就对不上，整个文件被判成非 WAV。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, fmt_chunk_with_cbsize_extension)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");

    b_tag(&b, "fmt ");
    b_u32(&b, 18);
    b_u16(&b, 1);                     /* PCM */
    b_u16(&b, 2);                     /* 双声道 */
    b_u32(&b, 16000);
    b_u32(&b, 16000 * 4);
    b_u16(&b, 4);
    b_u16(&b, 16);
    b_u16(&b, 0);                     /* cbSize = 0 */

    b_tag(&b, "data");
    b_u32(&b, 640);
    b_zeros(&b, 640);

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(2, wi.channels);
    UT_EQ_INT(16000, wi.rate);
    UT_EQ_INT(640, wi.data_bytes);
    /* 12 + 8 + 18（fmt 带 cbSize）+ 8（data 块头） = 46。
       少跳那 2 字节的 cbSize，这里会读到 44 —— 差 2 个字节，
       后面每个采样都错位半个，听感是"整段是噪声"。 */
    UT_EQ_INT(46, wi.data_offset);
}

/*---------------------------------------------------------------------------
 * 6. 非 WAV 必须老实说"不是"，不能误判
 *
 * 裸 PCM 回退分支靠这个返回值决定走哪条路。误判 = 把 PCM 当 WAV 解析，
 * 结果是一个字节都读不出来。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, rejects_non_wav)
{
    struct { const char *name; const uint8_t *data; size_t n; } v[] = {
        { "空文件",     (const uint8_t *)"",            0  },
        { "全零",       (const uint8_t *)"\0\0\0\0\0\0\0\0\0\0\0\0", 12 },
        { "PCM 裸数据", (const uint8_t *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C", 12 },
        { "RIFF 但非 WAVE", (const uint8_t *)"RIFF\x10\0\0\0AVI ", 12 },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        buf_t b = {0};
        if (v[i].n) memcpy(b.b, v[i].data, v[i].n);
        b.n = v[i].n;

        FILE *fp = to_file(&b);
        if (!fp) { UT_FAIL("%s: 建临时文件失败", v[i].name); continue; }
        wav_info_t wi;
        bool ok = wav_parse(fp, &wi);
        fclose(fp);
        drop_tmp();
        if (ok) UT_FAIL("%s 被误判成 WAV", v[i].name);
    }
}

/*---------------------------------------------------------------------------
 * 7. 头里没有 data 块 → 必须返回 false（而不是返回 data_bytes=0 的"成功"）
 *---------------------------------------------------------------------------*/
UT_TEST(wav, missing_data_chunk_is_rejected)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");
    b_fmt16(&b, 1, 8000, 16);

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(!ok);
}

/*---------------------------------------------------------------------------
 * 8. 头是对的但 8bit / 单声道字段不能被写成 16bit
 *
 * 这条防"硬编码 16bit"：8bit WAV 的采样是无符号的，按 16bit 读会得到
 * 完全错误的结果（每个字节被当成半个采样）。
 *---------------------------------------------------------------------------*/
UT_TEST(wav, reads_8bit_mono_correctly)
{
    buf_t b = {0};
    b_tag(&b, "RIFF"); b_u32(&b, 0); b_tag(&b, "WAVE");
    b_fmt16(&b, 1, 8000, 8);
    b_tag(&b, "data");
    b_u32(&b, 800);
    b_zeros(&b, 800);

    FILE *fp = to_file(&b);
    UT_ASSERT(fp != NULL);
    if (!fp) return;

    wav_info_t wi;
    bool ok = wav_parse(fp, &wi);
    fclose(fp);
    drop_tmp();

    UT_ASSERT(ok);
    UT_EQ_INT(8, wi.bits);
    UT_EQ_INT(800, wi.data_bytes);
}

/*---------------------------------------------------------------------------
 * 9. 空指针不许崩
 *---------------------------------------------------------------------------*/
UT_TEST(wav, null_arguments_are_safe)
{
    wav_info_t wi;
    UT_ASSERT(!wav_parse(NULL, &wi));
    UT_ASSERT(!wav_parse(NULL, NULL));
}
