/**
 * @file ut_crc32.c
 * @brief CRC-32 用例
 *
 * 这个模块是"丢包恢复验收"的量具：量具错了，验收结论就是假的。
 * 所以期望值全部用**公开的标准向量**（zlib.crc32 同多项式），
 * 不拿自己的实现当基准 —— 否则系数写错时会自己跟自己对齐，测不出来。
 */
#include "ut.h"
#include "crc32.h"

/*---------------------------------------------------------------------------
 * 1. 标准向量：CRC-32("123456789") = 0xCBF43926
 *    这是 CRC-32/IEEE 的经典自检值，所有实现都必须对得上。
 *---------------------------------------------------------------------------*/
UT_TEST(crc32, matches_standard_check_value)
{
    const char *s = "123456789";

    UT_EQ_INT(0xCBF43926u, crc32_buf(s, 9));
    UT_EQ_INT(0xCBF43926u, crc32_update(0, s, 9));
}

/*---------------------------------------------------------------------------
 * 2. 空输入 = 0；空指针也不许崩
 *---------------------------------------------------------------------------*/
UT_TEST(crc32, empty_input_is_zero)
{
    UT_EQ_INT(0u, crc32_buf(NULL, 0));
    UT_EQ_INT(0u, crc32_buf("", 0));
    UT_EQ_INT(0u, crc32_update(0, NULL, 0));
}

/*---------------------------------------------------------------------------
 * 3. 分段累加 == 一次算完
 *
 * 落盘是"逐行拷"的（因为 linesize > width），所以逐行 update 这条路径
 * 才是生产路径，必须和一次性算的结果一致。
 *---------------------------------------------------------------------------*/
UT_TEST(crc32, incremental_equals_oneshot)
{
    /* 注意这里**不能**写 const：const static 会落到只读段（.rdata），
       下面往里面填数据就是写只读页 —— MinGW 上直接 SIGSEGV。
       （这个用例第一版就是这么挂的，UT 把它标成 CRASH 而不是把整轮打断。） */
    static uint8_t data[500];
    uint8_t *p = data;
    for (int i = 0; i < 500; i++) p[i] = (uint8_t)(i * 31 + 7);

    uint32_t once = crc32_buf(p, 500);

    uint32_t acc = 0;
    for (int off = 0; off < 500; off += 7) {
        int n = (500 - off >= 7) ? 7 : (500 - off);
        acc = crc32_update(acc, p + off, (size_t)n);
    }

    UT_EQ_INT(once, acc);
}

/*---------------------------------------------------------------------------
 * 4. 已知单字节值（避免实现只是"恰好"对上长向量）
 *---------------------------------------------------------------------------*/
UT_TEST(crc32, known_single_byte_values)
{
    UT_EQ_INT(0xE8B7BE43u, crc32_buf("a", 1));
    UT_EQ_INT(0x352441C2u, crc32_buf("abc", 3));
    UT_EQ_INT(0x3610A686u, crc32_buf("hello", 5));
}

/*---------------------------------------------------------------------------
 * 5. 改一个字节必须换指纹
 *
 * 这条在给"验收"当保险：如果 CRC 对差异不敏感（比如只算了前 16 字节），
 * 恢复失败时文件照样"全部命中参考"，假绿。
 *---------------------------------------------------------------------------*/
UT_TEST(crc32, one_bit_difference_changes_the_result)
{
    static uint8_t a[4096];
    static uint8_t b[4096];
    memset(a, 0x5A, sizeof(a));
    memcpy(b, a, sizeof(b));

    UT_EQ_INT(crc32_buf(a, sizeof(a)), crc32_buf(b, sizeof(b)));

    b[0]     ^= 0x01;
    UT_NE_INT(crc32_buf(a, sizeof(a)), crc32_buf(b, sizeof(b)));

    memcpy(b, a, sizeof(b));
    b[4095]  ^= 0x80;                 /* 最后一个字节也要算进去 */
    UT_NE_INT(crc32_buf(a, sizeof(a)), crc32_buf(b, sizeof(b)));
}
