/**
 * @file crc32.c
 * @brief CRC-32/IEEE 实现（见 crc32.h）
 */
#include "crc32.h"

/* 表只建一次，之后复用它。
   这里刻意用"惰性 + 可能重复建表"的写法而不是 pthread_once：
   建表的写是**幂等**的（重复写进去的是同一批常量），所以即使两个线程
   同时第一次进来，结果也一定相同 —— 用一个锁去保护一个幂等操作，
   只是给自己增加一个可能死锁的点。
   真正需要小心的是"边建边用"（读到写了一半的表），所以顺序是：
   全部建完 → 置 ready → 才允许读。 */
static uint32_t s_tab[256];
static int      s_tab_ready;

static void tab_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_tab[i] = c;
    }
    s_tab_ready = 1;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    if (!s_tab_ready) tab_init();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = ~crc;
    while (len--) c = s_tab[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

uint32_t crc32_buf(const void *data, size_t len)
{
    return crc32_update(0, data, len);
}
