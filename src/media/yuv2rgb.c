/**
 * @file yuv2rgb.c
 * @brief YUV420P → RGB565 + 等比适配的实现
 */
#include "yuv2rgb.h"

/*---------------------------------------------------------------------------
 * 等比适配
 *---------------------------------------------------------------------------*/
bool yuv2rgb_fit(int sw, int sh, int bw, int bh, int *ow, int *oh)
{
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0 || !ow || !oh) return false;

    /* 比较 sw/bw 与 sh/bh 的大小：交叉相乘，避免除法舍入
       sw/bw >= sh/bh  <=>  sw*bh >= bw*sh
       （右边更大的意思是"宽度是更紧的约束"，按宽铺满） */
    if ((int64_t)sw * bh >= (int64_t)bw * sh) {
        *ow = bw;
        *oh = (int)(((int64_t)sh * bw + sw / 2) / sw);      /* +sw/2 = 四舍五入 */
    } else {
        *oh = bh;
        *ow = (int)(((int64_t)sw * bh + sh / 2) / sh);
    }

    /* 四舍五入可能把结果顶出框 1 像素（比如 1.0 舍成 1.0004），夹一下 */
    if (*ow > bw) *ow = bw;
    if (*oh > bh) *oh = bh;
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
    return true;
}

/*---------------------------------------------------------------------------
 * 像素转换
 *---------------------------------------------------------------------------*/
static inline uint8_t clamp255(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

uint16_t yuv2rgb_565_px(int y, int u, int v)
{
    /* BT.601 有限量程的整数近似（系数放大 256 倍，最后右移 8）：
         R = 1.164(Y-16) + 1.596(V-128)      → 298, 409
         G = 1.164(Y-16) - 0.391(U-128) - 0.813(V-128)  → 298, -100, -208
         B = 1.164(Y-16) + 2.018(U-128)      → 298, 516
       +128 是移位前的四舍五入。 */
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;

    uint8_t r = clamp255((298 * c + 409 * e + 128) >> 8);
    uint8_t g = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8);
    uint8_t b = clamp255((298 * c + 516 * d + 128) >> 8);

    /* 截断而非四舍五入：截断是"可逆"的（Python 侧用同样的 >>3 就能还原），
       四舍五入在 5→8→5 往返时会因为两次舍入出现 ±1 抖动。 */
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/*---------------------------------------------------------------------------
 * 整帧转换
 *---------------------------------------------------------------------------*/
bool yuv2rgb_565(const vframe_t *f, uint16_t *dst, int dw, int dh)
{
    if (!f || !dst || dw <= 0 || dh <= 0) return false;
    if (f->w <= 0 || f->h <= 0) return false;
    if (!f->plane[0] || !f->plane[1] || !f->plane[2]) return false;

    const int sw = f->w, sh = f->h;
    const uint8_t *yp = f->plane[0];
    const uint8_t *up = f->plane[1];
    const uint8_t *vp = f->plane[2];
    const int ys = f->linesize[0], us = f->linesize[1], vs = f->linesize[2];
    if (ys <= 0 || us <= 0 || vs <= 0) return false;

    for (int y = 0; y < dh; y++) {
        /* 像素中心对齐：映射到源图 [0, sh) 内的整数行 */
        int sy = (int)(((int64_t)(2 * y + 1) * sh) / (2 * dh));
        if (sy >= sh) sy = sh - 1;              /* 防御：极端比例下夹住 */

        const uint8_t *yrow = yp + (size_t)sy * (size_t)ys;
        const uint8_t *urow = up + (size_t)(sy >> 1) * (size_t)us;
        const uint8_t *vrow = vp + (size_t)(sy >> 1) * (size_t)vs;

        uint16_t *orow = dst + (size_t)y * (size_t)dw;

        for (int x = 0; x < dw; x++) {
            int sx = (int)(((int64_t)(2 * x + 1) * sw) / (2 * dw));
            if (sx >= sw) sx = sw - 1;

            /* YUV420P：U/V 分辨率为 Y 的一半，所以源坐标右移一位 */
            orow[x] = yuv2rgb_565_px(yrow[sx], urow[sx >> 1], vrow[sx >> 1]);
        }
    }
    return true;
}
