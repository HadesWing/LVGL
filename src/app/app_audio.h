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

/* 通话粒度收尾（rtp_stop_media 时调）：收尾落盘 wav、停提示音。
   **不关设备** —— 室内机的喇叭/麦克风是常驻的：挂断后还要播忙音，
   来电马上又要振铃。真正的 close 只在进程退出时发生。
   （以前这里直接 close，忙音就在设备关掉之后触发，永远无声。） */
void app_audio_call_end(void);

/* 能不能发上行（音源取得到数据） / 能不能把下行交出去（喇叭或文件） */
bool app_audio_can_record(void);
bool app_audio_can_play(void);

/* 取采集到的 PCM（最多 n 个采样），返回实际取到的采样数。
   mic 模式不足一帧返回 0 —— 由调用方决定补静音还是不发包；
   file / tone 模式总是返回满帧。 */
int app_audio_read(int16_t *pcm, int n);

/* 把远端 PCM 交出去：进播放环，或写进 wav（按当前音宿） */
void app_audio_write(const int16_t *pcm, int n);

/*---------------------------------------------------------------------------
 * 本地提示音（M2）：与 RTP 上下行无关的"机器自己说话"
 *
 * 【设计】450Hz 单频 + 通/断节奏（GB/T 15279 的关键节奏，不做和弦）：
 *   RINGBACK 振铃/回铃 1s 通 + 4s 断 —— 来电和呼出共用（对讲场景听不出差别）；
 *   BUSY     忙音 0.35s 通 + 0.35s 断，默认响 2s 自动停 —— 挂断/拒接/呼叫失败。
 * 数据直接注入播放环：响铃期没有远端音频，不存在叠加冲突。
 * 只在喇叭路由（sink=spk）真的开着时出声；没声卡静默跳过，不报错。
 * SIP_NO_TONE 一键全关（CI/无声卡机器）。
 *---------------------------------------------------------------------------*/
typedef enum {
    AUDIO_TONE_RINGBACK = 0,   /* 振铃/回铃：1s 通 4s 断 */
    AUDIO_TONE_BUSY            /* 忙音：0.35s 通 0.35s 断，2s 自动停 */
} audio_tone_id_t;

/* 开始播提示音。同 id 重复调用不打断不重置（状态机反复触发是常态）；
   不同 id 会切换。谁停？RINGBACK 由状态机离开响铃态时 stop，
   BUSY 自带 2 秒上限，忘了 stop 也不会响一辈子。 */
void app_audio_tone_start(audio_tone_id_t id);
void app_audio_tone_stop(void);

/* 推时钟：由 SIP 线程的 recv_loop 每 ≤20ms 调一次，内部按真实时间
   补齐该出的 PCM（select 超时/有包两条路径都会走到） */
void app_audio_tone_tick(void);

/* 运行统计：播放欠载（爆音风险）、采集溢出（丢音）次数 */
void app_audio_stats(uint32_t *underrun, uint32_t *overrun);

/* 当前路由的文字描述，给日志留证（"到底有没有开麦 / 开喇叭"） */
const char *app_audio_route_desc(void);

#endif /* APP_AUDIO_H */
