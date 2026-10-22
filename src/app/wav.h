/**
 * @file wav.h
 * @brief 最小 WAV（RIFF/WAVE）读写 —— 从 app_audio.c 里抽出来的
 *
 * 为什么单独一个模块：
 *   1) 这段逻辑原来是 app_audio.c 里的静态函数，而 app_audio.c 依赖 SDL。
 *      想给它写单元测试就得把 SDL 拖进测试工程 —— 测试纯逻辑却要装声卡库，
 *      结果就是没人写测试。抽出来之后它只依赖 stdio。
 *   2) 落盘/读盘是"看不见的错"高发区：头里 ch/rate/bits 写错一个字段，
 *      用播放器打开可能照样播放（只是音调不对），光靠耳朵听不出来。
 *      有了 wav_parse 就能断言"我写出去的头，读回来必须是这个意思"。
 *
 * 只支持 PCM 8/16bit、单/双声道 —— 对讲通路就这两种，多余的不写。
 */
#ifndef WAV_H
#define WAV_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define WAV_HEADER_BYTES 44   /* 标准 PCM WAV 头长度 */

typedef struct {
    uint32_t data_offset;   /* data 块正文字节起点（相对文件开头） */
    uint32_t data_bytes;    /* data 块正文长度（已按文件实际长度修正） */
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;
} wav_info_t;

/**
 * 解析 WAV 头。成功返回 true 并填 out；不是 WAV（或头残缺）返回 false。
 *
 * 调用返回 false 后 fp 的位置**没有保证** —— 需要复用的调用方自己 rewind。
 * 这样设计是因为两类调用方（本模块内部、app_audio 的裸 PCM 回退分支）
 * 都需要重新定位，与其猜一个位置不如明确"你自己来"。
 */
bool wav_parse(FILE *fp, wav_info_t *out);

/**
 * 生成 44 字节标准 PCM WAV 头。
 * data_bytes 可以先传 0，边写边回填（app_audio 的落盘就是这么用的）。
 */
void wav_write_header(uint8_t h[WAV_HEADER_BYTES], uint32_t data_bytes,
                      uint16_t channels, uint32_t rate);

#endif /* WAV_H */
