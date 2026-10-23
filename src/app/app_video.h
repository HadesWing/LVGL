/**
 * @file app_video.h
 * @brief 视频通路的路由与统计（对标 app_audio 的 src/sink 抽象）
 *
 * 为什么视频也要一套 src/sink 抽象：
 *   音频那边已经证明了这个做法的价值 —— 把"设备存不存在"和"编解码对不对"
 *   彻底解耦，就能在没有摄像头、没有屏幕、没有板子的情况下验收。
 *   视频这边更极端：目标板上根本还没写 fbdev/VPU，但解码正确性现在就能验。
 *
 * 【接收方向的三条出口】
 *   SIP_VIDEO_SINK=disp  解码 → YUV420P→RGB565 缩放 → 交给 LVGL 上屏（默认）
 *   SIP_VIDEO_SINK=file  解码后落盘成裸 YUV420P，用来和参考帧逐字节比对
 *   SIP_VIDEO_SINK=drop  什么也不做（显式声明这通电话不要视频，不 bind 视频端口）
 *
 * 【上屏为什么要经过"槽位"】
 *   解码在 RTP 线程，LVGL 只能在主线程碰（官方红线）。
 *   直接在主线程回调里解码会把 UI 卡死（H.264 软解一帧 2~5ms，25fps 就是
 *   12% 的 CPU 全压在渲染线程上），而且解码器内部状态不能被两个线程碰。
 *   所以做成**生产者/消费者**：RTP 线程填好 3 个槽之一 → 发事件 → 主线程取走。
 *   3 个槽是算出来的：1 个正在显示（LVGL 还在用，不能写）+ 1 个已备好等主线程取
 *   + 1 个空闲可写。少于 3 个就会在 25fps/33fps 这两个节拍错开时零星丢帧。
 *
 * 【丢包恢复】（这一版的重点）
 *   见 src/media/resync.h。要点：丢一个包就要整帧丢弃并停手，
 *   然后发 RTCP PLI 请求关键帧，收到 IDR 才恢复上屏。
 *   在 resync 之下还有一层"加速层"：rtp_reorder 按序化缓冲 + RTCP NACK
 *   重传请求（见 src/media/rtp_reorder.h）—— 单包丢失在 20ms 窗口内被
 *   重传补上，根本不会走到"整帧丢弃 + 等 IDR"那一步。参数：
 *     SIP_VIDEO_RESYNC = 0          关掉慢路径（对照实验用）
 *     SIP_VIDEO_PLI_MS = 500        等待期间多久重发一次 PLI
 *     SIP_VIDEO_RESYNC_TIMEOUT = 3  等多少秒还没 IDR 就放弃（单位秒，0 = 一直等）
 *     SIP_VIDEO_REORDER_MS = 20     重排缓冲等多久（0 = 关掉按序化）
 *     SIP_VIDEO_NACK = 0            关掉重传请求（默认开）
 *     SIP_VIDEO_NACK_MS = 40        重传请求多久没补上就重发
 *     SIP_VIDEO_NACK_RETRY = 5      每个洞最多请求几次
 *     SIP_VIDEO_FRAME_LOG = <路径>  每帧写一行 CRC，供 scripts/loss_verify.py 比对
 *
 * 【发送方向不存在】
 *   室内机只解码显示，不编码视频（目标板上没有视频编码的需求）。
 *   所以这一版只做接收，不预留发送接口 —— 留空壳只会让人误以为它能用。
 */
#ifndef APP_VIDEO_H
#define APP_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

/**
 * 按环境变量解析路由并打开资源：
 *   SIP_VIDEO_SINK = disp | file | drop     （默认 disp：收到视频就上屏 —— 默认值要跟产品行为一致）
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
 * seq 是 RTP 序号 —— **必须传**，它是"丢了包没有"的唯一判据。
 * **在 RTP 线程调用**，内部会做按序化、解码和转换，最后 post 事件让主线程取画面。
 */
void app_video_on_rtp(const uint8_t *payload, int len, uint32_t ts, uint16_t seq);

/**
 * 无包时的时钟驱动：重排缓冲的洞超时放行、NACK 重发都靠它推进。
 * 收包循环的固定 tick（20ms）里调一次，通话外调了也没副作用。
 */
void app_video_tick(void);

/**
 * 注册"请求关键帧"回调（由 SIP/RTP 层提供，内部发 RTCP PLI）。
 * 只在通话期间有效，挂断时传 NULL 清掉。
 */
void app_video_set_key_req_cb(void (*cb)(void));

/**
 * 注册"请求重传"回调（由 SIP/RTP 层提供，内部发 RTCP Generic NACK）。
 * 参数是缺失的 RTP 序号。挂断时传 NULL 清掉。
 */
void app_video_set_nack_cb(void (*cb)(uint16_t seq));

/**
 * 上屏目标框（不含边框）。UI 建好视频控件后调用一次，转换时按这个框等比缩放。
 * 不调也行，默认 288x216。**必须在主线程、第一次收到视频之前调用**
 * （运行中改尺寸会让正在显示的槽失效）。
 */
void app_video_set_box(int w, int h);

/**
 * 主线程取一帧待显示的画面。
 *   slot    EV_VIDEO_FRAME 的 arg0
 *   rgb565  输出：RGB565 像素，连续 w*h 个 uint16_t（本机字节序）
 *   返回 false = 该槽已被别的帧覆盖/尚未就绪，直接跳过这一帧即可。
 *
 * **取到之后这块内存就归 LVGL 用，解码线程不会再来写**；直到下一次
 * app_video_disp_acquire() 才把上一帧的槽还给解码线程。
 * 所以调用方只要保证"拿到就立刻 lv_image_set_src"，不需要自己拷贝。
 */
bool app_video_disp_acquire(int slot, const uint8_t **rgb565, int *w, int *h);

/* 已上屏的帧数（进 LVGL 的，不含被丢的） */
uint32_t app_video_disp_frames(void);

/* 挂断/结束时的收尾统计，日志里能直接看出"问题出在收包还是解码还是上屏" */
void app_video_stats_print(const char *why);

/* 路由描述，打印在通话建立那行日志里（和音频并排，一眼看清这通电话是什么配置） */
const char *app_video_route_desc(void);

#endif /* APP_VIDEO_H */
