/**
 * @file app_video.h
 * @brief 视频通路的路由与统计（对标 app_audio 的 src/sink 抽象）
 *
 * 为什么视频也要一套 src/sink 抽象：
 *   音频那边已经证明了这个做法的价值 —— 把"设备存不存在"和"编解码对不对"
 *   彻底解耦，就能在没有摄像头、没有屏幕、没有板子的情况下验收。
 *   视频这边更极端：目标板上根本还没写 fbdev/VPU，但解码正确性现在就能验。
 *
 * 本轮只做**接收方向**（室内机收视频）：
 *   收：RTP → h264_depack → vdec → SIP_VIDEO_SINK（file 落盘 / drop 丢弃）
 *
 * 发送方向（板子把摄像头画面推给门口机）留了接口但没实现 —— 那属于 M6 上板之后
 * 的事，而且要等 MPP 编码器可用。现在写个空壳只会让人误以为它能用。
 */
#ifndef APP_VIDEO_H
#define APP_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

/**
 * 按环境变量解析路由并打开资源：
 *   SIP_VIDEO_SINK = disp | file | drop     （默认 drop：不处理视频，通话只有声音）
 *   SIP_VIDEO_SINK_FILE = <路径>             （默认 video_rx.yuv）
 *   SIP_VIDEO_NODEC = 1                     只做 RTP 重组、不解码（分离"收包"和"解码"两个问题）
 *
 * 返回 false 表示视频不可用（比如没编 ffmpeg），这时通话照常进行，只是没画面。
 */
bool app_video_open(void);
void app_video_close(void);
bool app_video_is_open(void);

/* 收不收集视频？drop 模式下返回 false —— 告诉 RTP 层"别把包转给我"，
   省掉一次 depack 和一帧 100KB 的内存搬运。 */
bool app_video_wants_rx(void);

/**
 * 收一个 RTP 视频 payload（不含 RTP 头）。
 * ts 是 RTP 时间戳（90 kHz），用来判 AU 边界。
 */
void app_video_on_rtp(const uint8_t *payload, int len, uint32_t ts);

/* 挂断/结束时的收尾统计，日志里能直接看出"问题出在收包还是解码" */
void app_video_stats_print(const char *why);

/* 路由描述，打印在通话建立那行日志里（和音频并排，一眼看清这通电话是什么配置） */
const char *app_video_route_desc(void);

#endif /* APP_VIDEO_H */
