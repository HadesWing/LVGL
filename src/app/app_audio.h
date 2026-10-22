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
 *
 * 【音源/音宿可换挡：这是自测的关键】
 *   真实设备（mic / 喇叭）带进来两个不可控变量：设备存不存在、驱动好不好。
 *   本机实测 WASAPI 连采集设备都没有，靠它测音频永远测不清。
 *   所以把两端都做成多选一，让"文件"顶替"房间"：
 *
 *     SIP_AUDIO_SRC  = mic | file | tone | none
 *     SIP_AUDIO_SINK = spk | file | drop
 *
 *   于是两条通路可以各自独立验证，一次只坏一处：
 *
 *     发：室内机 src=file  ──RTP──►  PC sink=spk   （人耳听）
 *     收：PC src=file      ──RTP──►  室内机 sink=file（存盘回放）
 *
 *   注意：G.711 是有损压缩，"存下来能听"只说明通路通了，**不能说明没出错**。
 *   客观验收要拿存下来的 wav 跟源文件做样本级比对（scripts/audio_compare.py），
 *   字节序反了、采样率错、SRTP 当明文解，这几种错误人耳都不一定立刻听出来，
 *   但 SNR 会从 ~38dB 掉到个位数。
 */
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

/* 上行音源 */
typedef enum {
    AUDIO_SRC_MIC = 0,   /* 真实麦克风（设备不存在则 can_record=false） */
    AUDIO_SRC_FILE,      /* 从 WAV 循环读，按 20ms/帧 匀速吐出 */
    AUDIO_SRC_TONE,      /* 合成 1kHz 正弦（老开关 SIP_TONE=1 的等价物） */
    AUDIO_SRC_NONE       /* 关掉上行：只收不发 */
} audio_src_mode_t;

/* 下行音宿 */
typedef enum {
    AUDIO_SINK_SPK = 0,  /* 真实喇叭 */
    AUDIO_SINK_FILE,     /* 写 WAV 落盘：一次通话一个文件 */
    AUDIO_SINK_DROP      /* 关掉下行：只发不收（仍统计收到了多少） */
} audio_sink_mode_t;

/* 打开音频子系统。file/tone/none 模式下**不打开任何声卡设备**。
   返回 false 表示既没有可用音源、也没有落盘手段（调用方退回 RTP 回声）。 */
bool app_audio_open(void);
void app_audio_close(void);
bool app_audio_is_open(void);

/* 能不能发上行（音源取得到数据） / 能不能把下行交出去（喇叭或文件） */
bool app_audio_can_record(void);
bool app_audio_can_play(void);

/* 取采集到的 PCM（最多 n 个采样），返回实际取到的采样数。
   mic 模式不足一帧返回 0 —— 由调用方决定补静音还是不发包；
   file / tone 模式总是返回满帧。 */
int app_audio_read(int16_t *pcm, int n);

/* 把远端 PCM 交出去：进播放环，或写进 wav（按当前音宿） */
void app_audio_write(const int16_t *pcm, int n);

/* 运行统计：播放欠载（爆音风险）、采集溢出（丢音）次数 */
void app_audio_stats(uint32_t *underrun, uint32_t *overrun);

/* 当前路由的文字描述，给日志留证（"到底有没有开麦 / 开喇叭"） */
const char *app_audio_route_desc(void);

#endif /* APP_AUDIO_H */
