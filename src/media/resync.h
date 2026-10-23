/**
 * @file resync.h
 * @brief 丢包恢复策略：丢包 → 请求关键帧 → 等 IDR → 恢复
 *
 * 【为什么"丢了一两个包"就必须整帧丢掉并等 I 帧】
 *   H.264 的 P 帧只编码"和参考帧的差异"，参考帧又参考上一帧 —— 一条链。
 *   链上任何一帧缺了数据，后面所有 P 帧都建立在一个错误的基准上，
 *   于是画面不是"缺一块"，而是从丢包点开始**持续花屏**，直到下一个 IDR
 *   把参考链重置为止（GOP=60、30fps 就是 2 秒烂画面）。
 *   所以正确的做法不是"尽量把残缺的帧解出来"，而是：
 *     立刻停手（丢包之后的帧一帧都不喂解码器）→ 请求 IDR → 收到 IDR 再恢复。
 *   宁可黑两秒，也不给人看两秒垃圾 —— 门口机上的画面是用来认人的。
 *
 * 【为什么单独抽成模块】
 *   这段逻辑全是状态和时间，跟 socket、解码器、LVGL 都没关系。
 *   抽出来的好处是可以用 UT 把"丢包三次、期间又来两次丢包、PLI 该隔多久重发、
 *   超时该放弃"这些时序全部钉死 —— 这些场景靠端到端脚本几乎不可能稳定复现。
 *
 * 【时间参数全部由调用方传 now_ms】
 *   模块自己不读时钟，测试才能精确控制时间推进 1ms 或 10 秒。
 */
#ifndef RESYNC_H
#define RESYNC_H

#include <stdbool.h>
#include <stdint.h>

/* 等待期间多久重发一次关键帧请求。
 * 500ms 的理由：一个 RTT 内没反应就重发，但又不至于把对端刷爆
 * （对端每收到一次 PLI 都要编一个 I 帧，I 帧是 P 帧的 5~10 倍大小）。 */
#define RESYNC_PLI_INTERVAL_MS_DEFAULT 500u

/* 等这么久还没等到 IDR 就放弃等待、恢复喂帧（画面会花，但至少是动的）。
 * 3 秒的依据：门口机常见的 GOP 是 2 秒（30fps×60），留一倍的余量；
 * 真的 3 秒还没 IDR，说明对端根本不响应 PLI 而且 GOP 很长，再等下去
 * 就是长时间黑屏 —— 对"看门口是谁"这个用途来说，花的画面比黑屏有用。
 * 设成 0 表示一直等（调试时用，能确认"到底有没有 IDR 进来"）。 */
#define RESYNC_GIVEUP_MS_DEFAULT 3000u

typedef enum {
    RESYNC_FEED = 0,   /* 正常：这个 AU 直接喂解码器 */
    RESYNC_SKIP,       /* 等待 IDR 中：丢掉这个 AU，不解码 */
    RESYNC_RESUME,     /* 这个 AU 是 IDR：先 flush 解码器，再正常喂 */
    RESYNC_GIVEUP      /* 等超时了：放弃等待，这个 AU 照样喂（画面将不完整） */
} resync_action_t;

typedef struct {
    bool     enabled;           /* false = 关闭，gate 永远返回 FEED（做 A/B 对照用） */
    bool     waiting;           /* 是否正处于"等 IDR"状态 */
    uint64_t wait_start_ms;     /* 本轮等待的起点（第一次丢包的时刻） */
    uint64_t last_pli_ms;       /* 上次请求关键帧的时刻 */

    uint32_t pli_interval_ms;
    uint32_t giveup_ms;         /* 0 = 一直等 */

    /* --- 统计（一条日志看清"丢了几次、跳了多少帧、等了多久"） --- */
    uint32_t events;            /* 进入等待的次数 = 丢包事件数 */
    uint32_t skipped;           /* 等待期间丢掉的 AU 总数 */
    uint32_t resumed;           /* 成功等到 IDR 的次数 */
    uint32_t gave_up;           /* 超时放弃的次数 */
    uint32_t pli;               /* 发出的关键帧请求数 */
    uint32_t last_skipped;      /* 最近一轮跳过的 AU 数 */
    uint64_t last_wait_ms;      /* 最近一轮的恢复耗时 */
    uint64_t max_wait_ms;       /* 最坏一次的恢复耗时 —— 比平均值更值得盯着 */
} resync_t;

void resync_init(resync_t *r, bool enabled, uint32_t pli_interval_ms, uint32_t giveup_ms);

/**
 * 报告一次丢包。返回 true 表示"现在就发一次关键帧请求（PLI）"。
 *
 * 已经在等待中又丢包时返回 false 且**不重置计时** —— 恢复耗时应该从
 * 第一次丢包算起，否则连续丢包会把"等了多久"一直清零，指标永远好看。
 */
bool resync_on_loss(resync_t *r, uint64_t now_ms);

/** 等待期间周期性重发关键帧请求。返回 true = 该发。每个 RTP 包都调一次即可，内部限流。 */
bool resync_tick(resync_t *r, uint64_t now_ms);

/** 每拿到一个完整 AU 调一次，决定这个 AU 喂不喂解码器。 */
resync_action_t resync_gate(resync_t *r, bool au_has_idr, uint64_t now_ms);

bool resync_waiting(const resync_t *r);

/* 清空状态（新的一通电话）。统计清零 —— 和 depack 的"统计累计"不同，
   这里的统计是"这通电话的质量"，跨通话累计没有意义。 */
void resync_reset(resync_t *r);

#endif /* RESYNC_H */
