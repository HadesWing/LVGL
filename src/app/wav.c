/**
 * @file wav.c
 * @brief WAV 头解析与生成（见 wav.h 说明）
 */
#include "wav.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

bool wav_parse(FILE *fp, wav_info_t *out)
{
    if (!fp || !out) return false;

    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff)) return false;
    /* RIFF....WAVE：中间 4 字节是文件总长，很多工具写错/写成 0，不校验它 */
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
        return false;

    memset(out, 0, sizeof(*out));
    out->channels = 1;
    out->bits     = 16;

    bool have_fmt = false;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, sizeof(ck), fp) != sizeof(ck)) break;
        uint32_t csz = rd32(ck + 4);

        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t f[16];
            /* fmt 块至少 16 字节；cbSize 扩展（>16）直接跳过，用不到 */
            if (csz < 16 || fread(f, 1, sizeof(f), fp) != sizeof(f)) break;
            out->channels = rd16(f + 2);
            out->rate     = rd32(f + 4);
            out->bits     = rd16(f + 14);
            have_fmt      = true;
            if (csz > 16) fseek(fp, (long)(csz - 16), SEEK_CUR);

        } else if (memcmp(ck, "data", 4) == 0) {
            long start = ftell(fp);
            if (start < 0 || !have_fmt) break;

            out->data_offset = (uint32_t)start;
            out->data_bytes  = csz;

            /* data 长度不可信是常态：
                 录音/推流工具习惯写 0（"边录边写，结束才知道"），
                 也有工具写成整数上限。所以一律拿文件实际剩余长度收口，
                 否则读的时候会读到文件尾之外的想象数据。 */
            fseek(fp, 0, SEEK_END);
            long end = ftell(fp);
            if (end >= start) {
                uint32_t avail = (uint32_t)(end - start);
                if (csz == 0 || csz > avail) out->data_bytes = avail;
            }
            fseek(fp, start, SEEK_SET);
            return out->data_bytes > 0;
        } else {
            /* 未知块（LIST/INFO/fact…）：跳过。块长按偶数对齐，
               奇数长度后面会跟一个填充字节，不跳这个字节整个偏移就歪了。 */
            fseek(fp, (long)csz + (long)(csz & 1), SEEK_CUR);
        }
    }
    return false;
}

void wav_write_header(uint8_t h[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint16_t channels, uint32_t rate)
{
    uint16_t bits      = 16;
    uint32_t block     = (uint32_t)channels * (bits / 8);
    uint32_t byte_rate = rate * block;

    memcpy(h + 0, "RIFF", 4);
    wr32(h + 4,  36 + data_bytes);       /* 之后所有字节数 */
    memcpy(h + 8, "WAVE", 4);

    memcpy(h + 12, "fmt ", 4);
    wr32(h + 16, 16);                    /* fmt 块长度 */
    wr16(h + 20, 1);                     /* 1 = PCM，非压缩 */
    wr16(h + 22, channels);
    wr32(h + 24, rate);
    wr32(h + 28, byte_rate);
    wr16(h + 32, (uint16_t)block);       /* 块对齐 */
    wr16(h + 34, bits);

    memcpy(h + 36, "data", 4);
    wr32(h + 40, data_bytes);
}
