/**
 * @file vdec.h
 * @brief 视频解码抽象层 —— 上层只认这个接口，不认具体解码器
 *
 * 为什么必须抽象：
 *   PC 上是 libavcodec 软解（H.264 CABAC + 运动补偿纯软件跑）；
 *   板子 RK3308 上是 Rockchip MPP 硬解（走 VPU，功耗差一个数量级）。
 *   两者的 API 毫无相似之处，但"喂一个访问单元、取一帧 YUV"这件事是一样的。
 *   把这层抽出来，`app_video.c` 一行条件编译都不用写 —— 换设备只换链接的实现文件。
 *
 * 为什么输出 YUV420P 而不是 RGB：
 *   上屏那一步才需要 RGB（LVGL 的 lv_image 吃 RGB565）。
 *   解码器原生输出就是 YUV420P，先转 RGB 再落盘等于多做一次有损转换，
 *   拿它当"解码是否正确"的判据就不干净了。落盘用 YUV，上屏前再转。
 *
 * 依赖：本头文件**不包含任何 FFmpeg 头**。没装 ffmpeg 的机器上主工程照样能编
 *       （走 vdec_stub.c），只是 `vdec_available()` 返回 false。
 */
#ifndef VDEC_H
#define VDEC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct vdec vdec_t;

/* 一帧解码结果。plane/linesize 的布局同 AVFrame，YUV420P 时 3 个平面都有值。 */
typedef struct {
    const uint8_t *plane[3];
    int            linesize[3];
    int            w, h;
    bool           keyframe;    /* 是否 IDR（I 帧）。丢包后要等到下一个 I 帧才恢复 */
    int64_t        pts;         /* 透传 vdec_feed 给的 pts，单位由调用方约定 */
} vframe_t;

typedef struct {
    uint32_t fed;        /* 送进去的访问单元数 */
    uint32_t frames;     /* 解出来的帧数 */
    uint32_t reorder;    /* 因 B 帧重排暂存的帧数（本工程用 baseline，应为 0） */
    uint32_t errors;     /* 解码错误次数 */
} vdec_stats_t;

/* 当前编译进来的后端是否能真解码（没装 ffmpeg 时是 false） */
bool vdec_available(void);

/* 后端名字，打印日志用："ffmpeg" / "mpp" / "none" */
const char *vdec_backend_name(void);

vdec_t *vdec_open(void);
void    vdec_close(vdec_t *v);

/**
 * 喂一个完整访问单元（Annex-B，含起始码）。
 * pts 原样带出来，用来对齐"这帧是哪一帧"——排查丢帧时靠它。
 * 返回 0 成功；<0 失败（解码器通常会吞掉错误继续，不会因此死掉）。
 */
int vdec_feed(vdec_t *v, const uint8_t *au, int len, int64_t pts);

/**
 * 取一帧。返回 1 = 取到，0 = 暂时没有（需要继续 feed），<0 = 错误。
 *
 * 返回的 vframe_t 指向解码器内部缓冲，**在下一次 vdec_get/vdec_feed 之前有效**。
 * 调用方要用就当场用完（落盘/转 RGB/上屏），不要存指针。
 */
int vdec_get(vdec_t *v, vframe_t *out);

const vdec_stats_t *vdec_stats(const vdec_t *v);

/* 丢包后（FU-A 断裂）重置内部参考帧状态：不重置的话后续 P 帧会一直解出花屏，
   要等码流自己发下一个 IDR 才恢复，中间几秒全是烂画面。 */
void vdec_flush(vdec_t *v);

#endif /* VDEC_H */
