/**
 * @file h264_depack.h
 * @brief H.264 RTP 解包与访问单元（AU）重组（RFC 6184）
 *
 * 这一层是"把网络包还原成解码器能吃的东西"。它不碰 socket，也不碰解码器，
 * 输入是 RTP payload、输出是 Annex-B 格式的完整访问单元 —— 所以可以脱离
 * 网络单独跑单元测试，这是本模块设计的第一目标。
 *
 * 为什么必须有这一层而不能直接把 RTP payload 丢给解码器：
 *   一个 I 帧往往 30~100 KB，而以太网 MTU 只有 1500。发送端必须把一个大 NAL
 *   切成多个 FU-A 分片。编码器看到的是"一个 NAL"，网络看到的是"几十个小包"，
 *   中间这层转换就是这里的工作。少了它，解码器每个包都当成一个残缺的 NAL，
 *   表现为花屏、绿块、或者直接不解码。
 *
 * 支持的打包模式：packetization-mode=1
 *   - 单 NAL 单元包（最常见，P 帧小的时候）
 *   - STAP-A（聚合多个小 NAL，如 SPS+PPS 一起发）
 *   - FU-A（大 NAL 分片，I 帧必用）
 *   STAP-B / MTAP / FU-B 属于 packetization-mode=2（交织模式），
 *   实际软电话和门口机基本不用，这里明确不支持并计数，不做半吊子实现。
 *
 * 内存：AU 缓冲按需 malloc，不用静态数组 —— 目标机 BSS 段紧张，
 *       而且 256KB 的静态缓冲会让 link 出来的固件多出 256KB（那是闪存和内存两份）。
 */
#ifndef H264_DEPACK_H
#define H264_DEPACK_H

#include <stdbool.h>
#include <stdint.h>

/* 单个访问单元上限。1080p 的高质量 I 帧在极端情况下能到 200KB+，
   室内机实际是 800×480/1024×600，256KB 有充分余量。
   超了会整帧丢弃并计数 —— 宁可丢一帧，也不要把内存写穿。 */
#define H264_DEPACK_AU_MAX (256 * 1024)

/* NAL 单元类型（RFC 6184 §1.3 + H.264 表 7-1 常用部分） */
#define H264_NAL_SLICE      1
#define H264_NAL_DPA        2
#define H264_NAL_DPB        3
#define H264_NAL_DPC        4
#define H264_NAL_IDR        5
#define H264_NAL_SEI        6
#define H264_NAL_SPS        7
#define H264_NAL_PPS        8
#define H264_NAL_AUD        9
#define H264_NAL_STAP_A    24
#define H264_NAL_FU_A      28

typedef struct {
    uint32_t au_done;     /* 完成的访问单元数（= 送进解码器的帧数） */
    uint32_t nal_done;    /* 重组的完整 NAL 数（含 SPS/PPS/SEI） */
    uint32_t pkt_in;      /* 收到的 RTP payload 包数 */
    uint32_t stap_pkt;    /* STAP-A 包数 */
    uint32_t fu_seq;      /* 完成重组的 FU-A 序列数 */
    uint32_t drop_type;   /* 丢弃：不支持的 NAL 类型 */
    uint32_t drop_oversize;/* 丢弃：超过 AU 缓冲上限 */
    uint32_t drop_slow;   /* 丢弃：上一帧还没被取走（上层解码跟不上） */
    uint32_t fu_broken;   /* FU-A 序列断裂（丢片/乱序），该 NAL 已丢弃 */
    uint32_t gap;         /* RTP 序号跳跃次数（丢包/乱序的直接证据） */
    uint32_t au_dropped;  /* 因数据不完整而整帧丢弃的 AU 数 */
} h264_depack_stats_t;

typedef struct h264_depack h264_depack_t;

h264_depack_t *h264_depack_new(void);
void           h264_depack_free(h264_depack_t *d);

/**
 * 喂一个 RTP payload（**不含** RTP 头，即 payload 第一个字节是 NAL header）。
 *
 * ts 是 RTP 时间戳（90 kHz）。同一帧的所有包 ts 相同，ts 变化即意味着
 * "上一帧结束、新一帧开始" —— 这是 RFC 6184 §5.6 建议的 AU 边界判据。
 * 用 ts 判边界而不是解析 slice header，是因为前者不需要理解码流语义，
 * 对 SPS/PPS/SEI 这些不属于 slice 的 NAL 也照样成立。
 *
 * seq 是 RTP 序列号（16 位，会回绕）。**必须传对**，它是"有没有丢包"的
 * 唯一判据：
 *   - 只看 ts 看不出来。丢了一个 FU-A 中间片，ts 没变，AU 照样"完整收尾"，
 *     只是里面少了 1000 字节 —— 解码器不报错，画面撕一块。
 *   - 序号一跳，说明数据缺了，当前 AU 立刻标脏、整帧丢弃（不是只回退当前 NAL）。
 *     整帧丢是必须的：前面的 NAL 也可能依赖丢失的数据，交出去就是花屏。
 *   乱序也按丢包处理（记 gap）—— 这一版没有重排缓冲，把乱序当丢失是保守且
 *   正确的选择；将来要做重排，改的也只是这一个判断。
 *
 * 返回 0 表示接受；负数表示被丢弃（原因同时计入统计）。
 */
int h264_depack_push(h264_depack_t *d, const uint8_t *payload, int len,
                     uint32_t ts, uint16_t seq);

/**
 * 取一个已完成的访问单元（Annex-B，含 00 00 00 01 起始码）。
 *
 * 返回长度 > 0 表示取到；0 表示暂时没有完整的。
 * @p_au 指向模块内部缓冲，**在下一次 push 之前有效** —— 调用方要拷走就当场拷。
 * 典型用法是"push 若干包，然后 while(pop) 送解码器"。
 *
 * @p_has_idr 输出"这个 AU 里有没有 IDR（NAL 类型 5）"。丢包恢复靠它判断
 * "现在这帧能不能开始解码" —— 一个 AU 里有 SPS/PPS/SEI 也有 slice，
 * 光看第一个 NAL 是不够的，所以由这一层在拼装时顺手记下来。
 */
int h264_depack_pop(h264_depack_t *d, const uint8_t **p_au, uint32_t *p_ts,
                    bool *p_has_idr);

const h264_depack_stats_t *h264_depack_stats(const h264_depack_t *d);

/* 清空所有中间状态（新的一路视频开始时用），统计保留 */
void h264_depack_reset(h264_depack_t *d);

#endif /* H264_DEPACK_H */
