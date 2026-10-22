/**
 * @file g711.c
 * @brief G.711 编解码实现（Sun Microsystems 经典算法的最简 C 版）
 *
 * A-law 结构：1 符号位 + 3 段号 + 4 量化位 = 8 bit
 *   - 欧洲/中国电话网用 A-law，静音附近的量化台阶更细（小信号更干净）
 * μ-law 结构：同样的分段，但对数曲线不同，北美/日本用。
 * 两者都是"对数压缩"：人耳对响度的感知本身就是对数的，所以 8 bit 听起来
 * 接近 13~14 bit 线性 PCM —— 这就是窄带语音只用 64kbps 的原因。
 */
#include "g711.h"

#define SEG_SHIFT   4
#define QUANT_MASK  0x0F
#define SEG_MASK    0x70
#define SIGN_BIT    0x80

/*---------------------------------------------------------------------------
 * A-law
 *---------------------------------------------------------------------------*/
/* 段边界。A-law 这段用 13bit 域（16bit 右移 3 位后的值），
   μ-law 用 16bit 域 —— 两者边界数值不同，别混用。 */
static const int16_t seg_aend[8] = { 0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF };

static int search_seg(int val, const int16_t *table, int size)
{
    for (int i = 0; i < size; i++)
        if (val <= table[i]) return i;
    return size;
}

uint8_t g711_linear2alaw(int16_t pcm)
{
    int mask;
    int val = pcm >> 3;              /* 16bit → 13bit */

    if (val >= 0) {
        mask = 0xD5;                 /* 正数：异或后符号位置 1 */
    } else {
        mask = 0x55;
        /* 负半轴取绝对值后要减 1（等价于 16bit 域的 -pcm-8）。
           少这一下，负数段边界会整体错 1 个量化级。
           这一行与 ffmpeg 的实现在段边界仍会差 1 LSB（双方取整策略不同），
           实测 65536 个输入里差 964 个、且只差 1 级 —— 人耳不可辨，
           互通无影响。别为了"跟 ffmpeg 完全一致"去改它：Sun 这套才是
           网关/IPPBX 里跑的那套。 */
        val = -val - 1;
    }

    int seg = search_seg(val, seg_aend, 8);
    if (seg >= 8) return (uint8_t)(0x7F ^ mask);

    uint8_t aval = (uint8_t)(seg << SEG_SHIFT);
    if (seg < 2) aval |= (uint8_t)((val >> 1) & QUANT_MASK);
    else         aval |= (uint8_t)((val >> seg) & QUANT_MASK);
    return (uint8_t)(aval ^ mask);
}

int16_t g711_alaw2linear(uint8_t a)
{
    a ^= 0x55;
    int t = (a & QUANT_MASK) << 4;
    int seg = (a & SEG_MASK) >> SEG_SHIFT;

    switch (seg) {
    case 0:  t += 8; break;
    case 1:  t += 0x108; break;
    default: t += 0x108; t <<= seg - 1; break;
    }
    return (int16_t)((a & SIGN_BIT) ? t : -t);
}

/*---------------------------------------------------------------------------
 * μ-law
 *---------------------------------------------------------------------------*/
#define BIAS   0x84
#define CLIP   32635

uint8_t g711_linear2ulaw(int16_t pcm)
{
    int sign = (pcm >> 8) & 0x80;
    int val  = sign ? -pcm : pcm;
    if (val > CLIP) val = CLIP;
    val += BIAS;

    int seg = search_seg(val, (const int16_t[]){ 0xFF, 0x1FF, 0x3FF, 0x7FF,
                                                 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF }, 8);
    uint8_t uval = (uint8_t)((seg << 4) | ((val >> (seg + 3)) & QUANT_MASK));
    return (uint8_t)(uval ^ (sign ? 0x7F : 0xFF));
}

int16_t g711_ulaw2linear(uint8_t u)
{
    u = (uint8_t)~u;
    int t = ((u & QUANT_MASK) << 3) + BIAS;
    t <<= (u & SEG_MASK) >> SEG_SHIFT;
    return (int16_t)((u & SIGN_BIT) ? (BIAS - t) : (t - BIAS));
}

/*---------------------------------------------------------------------------
 * 整帧
 *---------------------------------------------------------------------------*/
void g711_encode_alaw(const int16_t *pcm, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) out[i] = g711_linear2alaw(pcm[i]);
}

void g711_decode_alaw(const uint8_t *in, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) pcm[i] = g711_alaw2linear(in[i]);
}

void g711_encode_ulaw(const int16_t *pcm, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) out[i] = g711_linear2ulaw(pcm[i]);
}

void g711_decode_ulaw(const uint8_t *in, int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++) pcm[i] = g711_ulaw2linear(in[i]);
}
