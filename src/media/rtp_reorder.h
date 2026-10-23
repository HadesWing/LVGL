/**
 * @file rtp_reorder.h
 * @brief RTP 按序交付缓冲（mini jitter buffer）—— NACK 重传能生效的前提
 *
 * 【为什么需要它】
 *   NACK 重传的包**一定**迟到：它比正常序号晚了一个 RTT。而 H.264 解包
 *   （h264_depack）是"见洞即毒"的按序状态机 —— 迟到的包直接塞回去会被当成
 *   新的序号断档，把后面好好的帧也毒掉，越补越花。
 *   所以重传要发挥作用，必须有一个"等它一下"的按序交付点：
 *     洞出现后先 hold 住（默认 20ms），重传在窗口内到 → 无缝续上，
 *     **根本不会产生 gap**，resync/PLI 那套慢路径完全不用走；
 *     等不到 → 放行成 gap，走老路（整帧丢弃 + PLI 等 IDR）。
 *
 * 【分层关系】
 *     RTP 收包 → rtp_reorder（本模块，按序化 + NACK 请求）→ h264_depack → resync
 *   本模块是 resync 的**加速层**：它把"能救回来的丢包"在下游察觉之前救掉。
 *   resync 一行不改，行为完全兼容（hold=0 时退化为旧的立即放行语义）。
 *
 * 【NACK 重试策略】
 *   洞出现当场请求一次，之后每 retry_ms 重发，最多 max_retries 次。
 *   注意重发窗口被 hold_ms 封顶 —— hold=20ms 时只有前 20ms 里的重发有意义，
 *   所以默认 retry=8ms × 3 次，正好铺满窗口；20ms 还补不上，基本可以断定
 *   对端不响应 NACK（比如 ffmpeg 推流），剩下的交给 resync 的 PLI 慢路径。
 *
 * 【代价】
 *   常驻 64 包 × 1600 字节 ≈ 100KB + 固定 20ms 延迟。
 *   换来的是"单包丢失从 2 秒花屏/冻结变成无感" —— 这笔账怎么算都划算。
 *
 * 【序号回绕】
 *   全部比较走 (uint16_t)(a-b) 的差值语义；差值落在 0x4000~0xBFFF 视为
 *   流重启（SSRC 换了/编码器重连），清空重来，不产生洞 —— 否则一次重启
 *   会注册三万个"洞"，NACK 风暴直接把对端打懵。
 */
#ifndef RTP_REORDER_H
#define RTP_REORDER_H

#include <stdbool.h>
#include <stdint.h>

/* 统计：一条日志看清"重传到底救回多少、弃了多少" */
typedef struct {
    uint32_t pushed;      /* 收到的包（含重传/重复） */
    uint32_t delivered;   /* 按序交付给下游的包 */
    uint32_t dup;         /* 重复包（还在缓冲里没交付的第二次到达） */
    uint32_t filled;      /* 被重传补上的洞数 = NACK 成功次数 */
    uint32_t late;        /* 迟到（已交付过或洞已放弃后才到），数据只能弃 */
    uint32_t gap_pkts;    /* 判丢的包数（洞超时放弃）——交给下游走慢路径 */
    uint32_t holes;       /* 出现过的洞总数 */
    uint32_t nacks;       /* 发出的重传请求次数（含重发） */
    uint32_t overflow;    /* 洞太多登记不下、被迫立即放弃的包数 */
    uint32_t full;        /* 缓冲满（断流保险丝也没救回来）丢的包数 */
    uint32_t oversize;    /* 超过单包缓冲上限（1600B）被弃的包数 */
    uint32_t reset;       /* 大跳变判定为流重启的次数 */
} rtp_reorder_stats_t;

typedef struct rtp_reorder rtp_reorder_t;

/**
 * cap_packets：最多同时 hold 多少个包（64 ≈ 一个 IDR 分片量的零头）。
 * hold_ms：洞等多久才判丢。0 = 立即放弃（退化为"不按序化"的旧行为）。
 */
rtp_reorder_t *rtp_reorder_new(int cap_packets, uint32_t hold_ms);
void rtp_reorder_free(rtp_reorder_t *ro);
void rtp_reorder_reset(rtp_reorder_t *ro);

/**
 * 重传请求回调：洞刚出现调一次，之后按下面的策略重发。
 *   retry_ms    重发间隔（默认给 40）
 *   max_retries 每个洞最多请求几次（0 = 只请求一次）
 * 回调在"收包/取包"的调用线程里触发 —— 实现方只该做一次 sendto，别做重活。
 */
void rtp_reorder_set_nack_cb(rtp_reorder_t *ro,
                             void (*cb)(uint16_t seq, void *user), void *user);
void rtp_reorder_set_nack_policy(rtp_reorder_t *ro, bool enabled,
                                 uint32_t retry_ms, int max_retries);

/**
 * 收一个 RTP 包。洞在这里登记并（首次）触发 NACK。
 * push 之后必须循环调 pop 把就绪的包取走。
 */
void rtp_reorder_push(rtp_reorder_t *ro, uint16_t seq, uint32_t ts,
                      const uint8_t *payload, int len, uint64_t now_ms);

/**
 * 取下一个按序就绪的包。返回 payload 长度；0 = 暂时没有可交付的。
 * payload 指针在下次 push/pop 前有效 —— 下游要立刻消费（depack 是拷贝语义）。
 */
int rtp_reorder_pop(rtp_reorder_t *ro, uint16_t *seq, uint32_t *ts,
                    const uint8_t **payload, uint64_t now_ms);

/**
 * 没有新包时也要推一把时钟：洞超时放行、NACK 重发都靠时间驱动。
 * 收包循环的固定 tick（比如 20ms）里调一次即可。
 */
void rtp_reorder_tick(rtp_reorder_t *ro, uint64_t now_ms);

const rtp_reorder_stats_t *rtp_reorder_stats(const rtp_reorder_t *ro);

#endif /* RTP_REORDER_H */
