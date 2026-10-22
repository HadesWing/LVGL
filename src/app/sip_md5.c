/**
 * @file sip_md5.c
 * @brief RFC 1321 MD5 紧凑实现（公共域经典写法，逐位照搬无改动）
 */
#include "sip_md5.h"

#include <string.h>

typedef struct {
    unsigned int state[4];   /* A B C D */
    unsigned int count[2];   /* bits processed */
    unsigned char buf[64];   /* input buffer */
} md5_ctx_t;

static const unsigned int T[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
    0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
    0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
    0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
    0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
    0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
    0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
};
static const int S[64] = { 7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                           5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
                           4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                           6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21 };

#define F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define G(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define I(x,y,z) ((y) ^ ((x) | ~(z)))
#define ROTL(x,c) (((x) << (c)) | ((x) >> (32 - (c))))
#define STEP(f,a,b,c,d,m,k,s) (a) += f((b),(c),(d)) + (m) + T[(k)]; (a) = ROTL((a),(s)); (a) += (b);

static void md5_body(md5_ctx_t *ctx, const unsigned char block[64])
{
    unsigned int a = ctx->state[0], b = ctx->state[1];
    unsigned int c = ctx->state[2], d = ctx->state[3];
    unsigned int m[16];

    for (int i = 0; i < 16; i++)
        m[i] = (unsigned int)block[i*4] | ((unsigned int)block[i*4+1] << 8)
             | ((unsigned int)block[i*4+2] << 16) | ((unsigned int)block[i*4+3] << 24);

    for (int i = 0; i < 64; i++) {   /* 64 步 × 4 轮 */
        switch (i / 16) {
        case 0: STEP(F, a,b,c,d, m[i],          i,      S[i]);      break;
        case 1: STEP(G, a,b,c,d, m[(i*5+1)&15], i,      S[i]);      break;
        case 2: STEP(H, a,b,c,d, m[(i*3+5)&15], i,      S[i]);      break;
        case 3: STEP(I, a,b,c,d, m[(i*7)&15],   i,      S[i]);      break;
        }
        /* rotate roles */
        unsigned int t = d; d = c; c = b; b = a; a = t;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    (void)T; (void)S;
}

static void md5_init(md5_ctx_t *ctx)
{
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_append(md5_ctx_t *ctx, const unsigned char *data, size_t len)
{
    size_t have = (ctx->count[0] / 8) % 64;
    ctx->count[0] += (unsigned int)(len * 8);
    if (ctx->count[0] < (unsigned int)(len * 8)) ctx->count[1]++;
    ctx->count[1] += (unsigned int)(len >> 29);

    if (have) {
        size_t need = 64 - have;
        size_t take = len < need ? len : need;
        memcpy(ctx->buf + have, data, take);
        if (have + take == 64) md5_body(ctx, ctx->buf);
        data += take; len -= take;
    }
    while (len >= 64) { md5_body(ctx, data); data += 64; len -= 64; }
    if (len) memcpy(ctx->buf, data, len);
}

static void md5_final(md5_ctx_t *ctx, unsigned char out[16])
{
    static const unsigned char pad = 0x80;
    unsigned char bits[8];
    unsigned long long bitlen = ((unsigned long long)ctx->count[1] << 32) | ctx->count[0];

    for (int i = 0; i < 8; i++) bits[i] = (unsigned char)(bitlen >> (i * 8));

    md5_append(ctx, &pad, 1);
    unsigned char z = 0;
    while ((ctx->count[0] / 8) % 64 != 56) md5_append(ctx, &z, 1);
    md5_append(ctx, bits, 8);

    for (int i = 0; i < 4; i++) {
        out[i*4+0] = (unsigned char)(ctx->state[i]);
        out[i*4+1] = (unsigned char)(ctx->state[i] >> 8);
        out[i*4+2] = (unsigned char)(ctx->state[i] >> 16);
        out[i*4+3] = (unsigned char)(ctx->state[i] >> 24);
    }
}

static const char HEX[] = "0123456789abcdef";

void md5_hex(const void *data, size_t len, char out[33])
{
    md5_ctx_t c;
    unsigned char dig[16];
    md5_init(&c);
    md5_append(&c, (const unsigned char *)data, len);
    md5_final(&c, dig);
    for (int i = 0; i < 16; i++) {
        out[i*2+0] = HEX[dig[i] >> 4];
        out[i*2+1] = HEX[dig[i] & 0xf];
    }
    out[32] = '\0';
}

void md5_hex_str(const char *str, char out[33])
{
    md5_hex(str, strlen(str), out);
}
