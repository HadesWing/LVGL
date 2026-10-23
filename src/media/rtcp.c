/**
 * @file rtcp.c
 * @brief 最小 RTCP：PLI（要关键帧）+ Generic NACK（要重传），见 rtcp.h
 */
#include "rtcp.h"

int rtcp_build_pli(uint8_t *out, int cap, uint32_t sender_ssrc, uint32_t media_ssrc)
{
    if (!out || cap < RTCP_PLI_BYTES) return -1;

    out[0] = 0x81;          /* V=2, P=0, FMT=1 → PLI */
    out[1] = 206;           /* PT = PSFB */
    out[2] = 0x00;          /* length = 2（32bit 字计，减一），即整包 12 字节 */
    out[3] = 0x02;

    /* 网络字节序（大端）。这里手写而不是 htonl，是为了让这个文件不依赖
       socket 头 —— 单元测试要在没有网络栈的机器上跑。 */
    out[4]  = (uint8_t)(sender_ssrc >> 24);
    out[5]  = (uint8_t)(sender_ssrc >> 16);
    out[6]  = (uint8_t)(sender_ssrc >> 8);
    out[7]  = (uint8_t)(sender_ssrc);

    out[8]  = (uint8_t)(media_ssrc >> 24);
    out[9]  = (uint8_t)(media_ssrc >> 16);
    out[10] = (uint8_t)(media_ssrc >> 8);
    out[11] = (uint8_t)(media_ssrc);

    return RTCP_PLI_BYTES;
}

int rtcp_build_nack(uint8_t *out, int cap, uint32_t sender_ssrc, uint32_t media_ssrc,
                    uint16_t pid, uint16_t blp)
{
    if (!out || cap < RTCP_NACK_BYTES) return -1;

    out[0] = 0x81;          /* V=2, P=0, FMT=1 → Generic NACK */
    out[1] = 205;           /* PT = RTPFB（注意和 PLI 的 206/PSFB 不是一族） */
    out[2] = 0x00;          /* length = 3（4 个 32bit 字减一），整包 16 字节 */
    out[3] = 0x03;

    out[4]  = (uint8_t)(sender_ssrc >> 24);
    out[5]  = (uint8_t)(sender_ssrc >> 16);
    out[6]  = (uint8_t)(sender_ssrc >> 8);
    out[7]  = (uint8_t)(sender_ssrc);

    out[8]  = (uint8_t)(media_ssrc >> 24);
    out[9]  = (uint8_t)(media_ssrc >> 16);
    out[10] = (uint8_t)(media_ssrc >> 8);
    out[11] = (uint8_t)(media_ssrc);

    /* FCI：PID + BLP 位图（RFC 4585 §6.2.1） */
    out[12] = (uint8_t)(pid >> 8);
    out[13] = (uint8_t)(pid & 0xFF);
    out[14] = (uint8_t)(blp >> 8);
    out[15] = (uint8_t)(blp & 0xFF);

    return RTCP_NACK_BYTES;
}
