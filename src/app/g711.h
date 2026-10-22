/**
 * @file g711.h
 * @brief G.711 A-law / μ-law 编解码（PCMA / PCMU）
 *
 * 为什么必须支持这两种：SIP 语音对讲里 8kHz 窄带是默认档，
 * PCMA(Payload 8) 是国内门口机/海思方案最常见的一种，PCMU(Payload 0)
 * 是欧美默认。对端在 SDP 里选哪个，我们就得按哪个编解码 —— 否则声音是噪声。
 *
 * 码率固定 64 kbps：20 ms 一帧 = 160 个采样 = 160 字节。
 */
#ifndef G711_H
#define G711_H

#include <stdint.h>

#define G711_FRAME_SAMPLES 160   /* 20ms @ 8kHz */
#define G711_RATE          8000

uint8_t g711_linear2alaw(int16_t pcm);
int16_t g711_alaw2linear(uint8_t a);

uint8_t g711_linear2ulaw(int16_t pcm);
int16_t g711_ulaw2linear(uint8_t u);

/* 整帧编解码：n 个采样 ↔ n 个字节 */
void g711_encode_alaw(const int16_t *pcm, uint8_t *out, int n);
void g711_decode_alaw(const uint8_t *in, int16_t *pcm, int n);
void g711_encode_ulaw(const int16_t *pcm, uint8_t *out, int n);
void g711_decode_ulaw(const uint8_t *in, int16_t *pcm, int n);

#endif /* G711_H */
