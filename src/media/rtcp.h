/**
 * @file rtcp.h
 * @brief 最小 RTCP：只做一件事 —— 发 PLI（Picture Loss Indication）请求关键帧
 *
 * 【为什么需要它】
 *   丢包之后我们只能"等下一个 IDR"。可 IDR 什么时候来由发送端决定：
 *   GOP 60 @30fps 就是每 2 秒一个，运气不好要黑 2 秒。
 *   PLI 是在说"我这帧丢了，请立刻编一个 I 帧给我" —— 对端支持的话，
 *   恢复时间从"最多一个 GOP"降到"一个 RTT + 一帧编码时间"（几十 ms）。
 *
 * 【为什么自己拼 12 个字节，不引 libavformat 的 rtcp 或 libosip】
 *   RFC 3550 + RFC 4585 里 PLI 的报文固定 12 字节、没有变长部分，
 *   拼起来就 4 个字段。为这 12 个字节引进一个库，交叉编译和固件体积
 *   都不划算（本项目已经为省 500KB 把 avformat 摘掉了）。
 *   拼错的代价也不大：对端只是听不懂、忽略，不会崩 —— 有统计可查。
 *
 * 报文格式（RFC 4585 §6.3.1，PSFB = Payload-Specific Feedback）：
 *
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |V=2|P|   FMT   |       PT      |          length               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                  SSRC of packet sender                        |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |                  SSRC of media source                         |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *   FMT=1（PLI）、PT=206（PSFB）、length = 2（单位是 32bit 字减一 → 12 字节）
 */
#ifndef RTCP_H
#define RTCP_H

#include <stdint.h>

#define RTCP_PLI_BYTES 12

/**
 * 拼一个 PLI 报文。
 *   sender_ssrc  我们自己的 SSRC（谁在抱怨）
 *   media_ssrc   对端的 SSRC（抱怨的是哪一路流）—— 必须填对，
 *                对端同时推多路时填 0 会被当成"未知流"而忽略。
 * 返回写入的字节数（固定 12），缓冲不够返回 -1。
 */
int rtcp_build_pli(uint8_t *out, int cap, uint32_t sender_ssrc, uint32_t media_ssrc);

#endif /* RTCP_H */
