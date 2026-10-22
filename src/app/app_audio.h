/**
 * @file app_audio.h
 * @brief 音频采集/播放（8kHz 单声道 S16）—— RTP 的"嘴和耳朵"
 *
 * 【线程模型】
 *   SDL 的采集回调与播放回调各自在**独立线程**里跑，不能碰 LVGL、不能碰 SIP 状态。
 *   本模块只做两件事：把麦克风数据塞进"采集环"，把"播放环"里的数据喂给声卡。
 *   RTP 线程只跟两个环打交道 —— 三层之间没有共享的可变业务状态，
 *   换到板子上（ALSA）时只要重新实现本文件，上层一行不改。
 *
 * 【为什么要有环 + 预缓冲】
 *   网络抖包是常态。收到一帧就立刻播，遇到一次抖动就是"咔"的一声（underrun）。
 *   预缓冲到 ~60ms 再开播，用几十毫秒延迟换掉爆音 —— 对讲场景的标准取舍。
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* 打开采集 + 播放。返回 false 表示本机没有可用声卡（CI/无麦克风场景），
   此时调用方应退回 RTP 回声模式，而不是把呼叫判为失败。 */
bool app_audio_open(void);
void app_audio_close(void);
bool app_audio_is_open(void);
bool app_audio_can_record(void);   /* 有麦克风才发上行 RTP */
bool app_audio_can_play(void);     /* 有喇叭才把下行 RTP 送去播放 */

/* 取采集到的 PCM（最多 n 个采样），返回实际取到的采样数；
   不足一帧返回 0 —— 由调用方决定补静音还是不发包。 */
int app_audio_read(int16_t *pcm, int n);

/* 把远端 PCM 放入播放环。 */
void app_audio_write(const int16_t *pcm, int n);

/* 运行统计：播放欠载（爆音风险）、采集溢出（丢音）次数 */
void app_audio_stats(uint32_t *underrun, uint32_t *overrun);

#endif /* APP_AUDIO_H */
