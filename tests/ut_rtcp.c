/**
 * @file ut_rtcp.c
 * @brief RTCP PLI 拼装用例
 *
 * PLI 发出去没人收也不会崩，所以**拼错了不会当场报错** ——
 * 只会表现为"对端怎么都不给我发 I 帧"。这种"静默失效"最需要 UT 兜底。
 *
 * 期望值全部按 RFC 4585 §6.3.1 手算，不抄现成实现。
 */
#include "ut.h"
#include "rtcp.h"

/*---------------------------------------------------------------------------
 * 1. 长度固定 12 字节，缓冲不够要报 -1
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, pli_is_12_bytes_and_rejects_small_buffer)
{
    uint8_t buf[RTCP_PLI_BYTES];

    UT_EQ_INT(RTCP_PLI_BYTES, rtcp_build_pli(buf, (int)sizeof(buf), 0x11223344u, 0xAABBCCDDu));
    UT_EQ_INT(-1, rtcp_build_pli(buf, RTCP_PLI_BYTES - 1, 1, 2));
    UT_EQ_INT(-1, rtcp_build_pli(NULL, RTCP_PLI_BYTES, 1, 2));
}

/*---------------------------------------------------------------------------
 * 2. 逐字节核对报文
 *
 *   V=2,P=0,FMT=1 → 0x81     PT=206（PSFB）→ 0xCE
 *   length=2（3 个 32bit 字减一）→ 0x00 0x02
 *   然后是 sender SSRC、再是 media source SSRC（大端）
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, pli_bytes_match_rfc_4585)
{
    uint8_t b[RTCP_PLI_BYTES];
    rtcp_build_pli(b, (int)sizeof(b), 0x01020304u, 0x05060708u);

    UT_EQ_INT(0x81, b[0]);            /* V=2 P=0 FMT=1 */
    UT_EQ_INT(206,  b[1]);            /* PT = PSFB */
    UT_EQ_INT(0x00, b[2]);            /* length 高字节 */
    UT_EQ_INT(0x02, b[3]);            /* length 低字节 = 2 → 12 字节 */

    UT_EQ_INT(0x01, b[4]);
    UT_EQ_INT(0x02, b[5]);
    UT_EQ_INT(0x03, b[6]);
    UT_EQ_INT(0x04, b[7]);            /* sender SSRC   = 0x01020304 */

    UT_EQ_INT(0x05, b[8]);
    UT_EQ_INT(0x06, b[9]);
    UT_EQ_INT(0x07, b[10]);
    UT_EQ_INT(0x08, b[11]);           /* media SSRC    = 0x05060708 */
}

/*---------------------------------------------------------------------------
 * 3. 两个 SSRC 必须区分开（顺序写反是最常见的低级错误）
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, sender_and_media_ssrc_are_not_swapped)
{
    uint8_t b[RTCP_PLI_BYTES];
    rtcp_build_pli(b, (int)sizeof(b), 0xDEADBEEFu, 0xCAFEBABEu);

    uint32_t s1 = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                  ((uint32_t)b[6] << 8)  |  (uint32_t)b[7];
    uint32_t s2 = ((uint32_t)b[8] << 24) | ((uint32_t)b[9] << 16) |
                  ((uint32_t)b[10] << 8) |  (uint32_t)b[11];

    UT_EQ_INT(0xDEADBEEFu, s1);        /* 先 sender */
    UT_EQ_INT(0xCAFEBABEu, s2);        /* 后 media source */
}

/*---------------------------------------------------------------------------
 * 4. Generic NACK：固定 16 字节，缓冲不够要报 -1
 *    报文格式 RFC 4585 §6.2.1：PT=205（RTPFB）、FMT=1、FCI = PID + BLP
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, nack_is_16_bytes_and_rejects_small_buffer)
{
    uint8_t buf[RTCP_NACK_BYTES];

    UT_EQ_INT(RTCP_NACK_BYTES,
              rtcp_build_nack(buf, (int)sizeof(buf), 0x11223344u, 0xAABBCCDDu, 0x003Cu, 0));
    UT_EQ_INT(-1, rtcp_build_nack(buf, RTCP_NACK_BYTES - 1, 1, 2, 1, 0));
    UT_EQ_INT(-1, rtcp_build_nack(NULL, RTCP_NACK_BYTES, 1, 2, 1, 0));
}

/*---------------------------------------------------------------------------
 * 5. NACK 逐字节核对
 *
 *   V=2,P=0,FMT=1 → 0x81     PT=205（RTPFB）→ 0xCD
 *   length=3（4 个 32bit 字减一）→ 0x00 0x03
 *   sender SSRC、media SSRC，然后 FCI：PID、BLP（各 2 字节大端）
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, nack_bytes_match_rfc_4585)
{
    uint8_t b[RTCP_NACK_BYTES];
    rtcp_build_nack(b, (int)sizeof(b), 0x01020304u, 0x05060708u, 0x003Cu, 0x8001u);

    UT_EQ_INT(0x81, b[0]);            /* V=2 P=0 FMT=1 —— 和 PLI 一样的 FMT */
    UT_EQ_INT(205,  b[1]);            /* PT = RTPFB（注意不是 PLI 的 206） */
    UT_EQ_INT(0x00, b[2]);
    UT_EQ_INT(0x03, b[3]);            /* length = 3 → 16 字节 */

    UT_EQ_INT(0x01, b[4]);
    UT_EQ_INT(0x02, b[5]);
    UT_EQ_INT(0x03, b[6]);
    UT_EQ_INT(0x04, b[7]);            /* sender SSRC */

    UT_EQ_INT(0x05, b[8]);
    UT_EQ_INT(0x06, b[9]);
    UT_EQ_INT(0x07, b[10]);
    UT_EQ_INT(0x08, b[11]);           /* media SSRC */

    UT_EQ_INT(0x00, b[12]);
    UT_EQ_INT(0x3C, b[13]);           /* PID = 60 */
    UT_EQ_INT(0x80, b[14]);
    UT_EQ_INT(0x01, b[15]);           /* BLP：61、76 丢 */
}

/*---------------------------------------------------------------------------
 * 6. NACK 的 PID 必须能表达回绕后的序号（65535 之后是 0）
 *---------------------------------------------------------------------------*/
UT_TEST(rtcp, nack_pid_handles_wraparound)
{
    uint8_t b[RTCP_NACK_BYTES];
    rtcp_build_nack(b, (int)sizeof(b), 1u, 2u, 0xFFFF, 0x0001u);

    uint16_t pid = (uint16_t)((b[12] << 8) | b[13]);
    uint16_t blp = (uint16_t)((b[14] << 8) | b[15]);
    UT_EQ_INT(0xFFFF, pid);
    UT_EQ_INT(0x0001, blp);           /* bit0 = 0x0000（回绕后的第一个序号） */
}
