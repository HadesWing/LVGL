/**
 * @file ut_rtp_reorder.c
 * @brief RTP 按序交付缓冲用例 —— NACK 重传能生效的前提
 *
 * 这个模块的用例全部用"脚本化时间"驱动（now_ms 由用例给），
 * 洞等多久、NACK 何时重发都能钉死到毫秒 —— 这些时序靠端到端脚本没法稳定复现。
 *
 * 三个曾经出过/差点出问题的边界，专门有用例压着：
 *   1) seq == next_seq 的重传包不能被判成"流重启"；
 *   2) push 连发时不给已缓冲的序号虚报洞；
 *   3) 缓冲满走保险丝而不是静默丢。
 */
#include "ut.h"
#include "rtp_reorder.h"

#include <string.h>

/* --- 测试脚手架：8 字节假 payload，首字节 = 序号低 8 位，方便核对顺序 --- */
#define PL 8

static void mk(uint8_t *buf, uint16_t seq)
{
    for (int i = 0; i < PL; i++) buf[i] = (uint8_t)(seq + i);
}

/* 收集 NACK 回调的序号 */
static uint16_t nack_log[64];
static int      nack_n;
static void     nack_sink(uint16_t seq, void *user)
{
    (void)user;
    if (nack_n < 64) nack_log[nack_n++] = seq;
}

/* pop 一个包并核对 seq/payload */
static int pop_one(rtp_reorder_t *ro, uint16_t want_seq, uint64_t now)
{
    uint16_t s = 0;
    uint32_t ts = 0;
    const uint8_t *p = NULL;
    int n = rtp_reorder_pop(ro, &s, &ts, &p, now);
    if (n > 0) {
        UT_EQ_INT(want_seq, (int)s);
        if (n == PL) {
            uint8_t exp[PL];
            mk(exp, want_seq);
            UT_MEM_EQ(exp, p, PL);
        }
    }
    return n;
}

/*---------------------------------------------------------------------------
 * 1. 按序直通：不丢包时行为和"直接喂 depack"一模一样
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, in_order_passthrough)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    UT_ASSERT(ro != NULL);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    for (uint16_t s = 100; s < 105; s++) {
        mk(b, s);
        rtp_reorder_push(ro, s, 90000 * s / 1000, b, PL, 1000);
    }
    for (uint16_t s = 100; s < 105; s++)
        UT_EQ_INT(PL, pop_one(ro, s, 1000));

    const rtp_reorder_stats_t *st = rtp_reorder_stats(ro);
    UT_EQ_INT(5,  (int)st->delivered);
    UT_EQ_INT(0,  (int)st->holes);
    UT_EQ_INT(0,  (int)st->gap_pkts);
    UT_EQ_INT(0,  (int)st->nacks);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 2. 核心：洞被重传补上 —— 不产生 gap、按序交付、NACK 记一次成功
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, hole_filled_by_retransmit)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 3); rtp_reorder_push(ro, 3, 3, b, PL, 1000);   /* 2 丢了 */

    UT_EQ_INT(PL, pop_one(ro, 1, 1000));
    UT_EQ_INT(0,  pop_one(ro, 2, 1000));                 /* 2 没到，攥住 3 */
    UT_EQ_INT(1,  nack_n);                               /* 当场就要了一次 */
    UT_EQ_INT(2,  (int)nack_log[0]);

    mk(b, 2); rtp_reorder_push(ro, 2, 2, b, PL, 1005);   /* 重传补上 */
    UT_EQ_INT(PL, pop_one(ro, 2, 1005));
    UT_EQ_INT(PL, pop_one(ro, 3, 1005));

    const rtp_reorder_stats_t *st = rtp_reorder_stats(ro);
    UT_EQ_INT(1, (int)st->filled);
    UT_EQ_INT(0, (int)st->gap_pkts);                     /* 关键：没有判丢 */
    UT_EQ_INT(3, (int)st->delivered);                    /* 1、2、3 共 3 包 */
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 3. 边界（UT 抓出来的）：seq == next_seq 的重传包不能判成流重启
 *    场景：洞 2 在 next_seq 上等着，重传的 2 号包此时到达。
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, retransmit_at_next_seq_is_not_stream_reset)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 50);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 3); rtp_reorder_push(ro, 3, 3, b, PL, 1000);
    UT_EQ_INT(PL, pop_one(ro, 1, 1000));                 /* next_seq → 2，洞活着 */

    nack_n = 0;
    mk(b, 2); rtp_reorder_push(ro, 2, 2, b, PL, 1010);   /* d==0 的重传 */
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->reset);     /* 不是重启 */
    UT_EQ_INT(1, (int)rtp_reorder_stats(ro)->filled);    /* 是补洞 */
    UT_EQ_INT(PL, pop_one(ro, 2, 1010));
    UT_EQ_INT(PL, pop_one(ro, 3, 1010));
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 4. 边界：push 连发时不给已缓冲的序号虚报洞
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, no_fake_holes_for_buffered_packets)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    /* 连发 1..5，一次都不 pop —— 2..5 都在手里的正常包 */
    for (uint16_t s = 1; s <= 5; s++) {
        mk(b, s);
        rtp_reorder_push(ro, s, s, b, PL, 1000);
    }
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->holes);
    UT_EQ_INT(0, nack_n);

    for (uint16_t s = 1; s <= 5; s++) UT_EQ_INT(PL, pop_one(ro, s, 1000));
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 5. 洞超时 → 判丢放行，后面的包正常交付（gap 由下游 depack 看 seq 跳变去处理）
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, hole_times_out_and_releases)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 3); rtp_reorder_push(ro, 3, 3, b, PL, 1000);
    UT_EQ_INT(PL, pop_one(ro, 1, 1000));

    UT_EQ_INT(0, pop_one(ro, 0, 1010));                  /* hold 内不放弃 */
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->gap_pkts);

    UT_EQ_INT(PL, pop_one(ro, 3, 1021));                 /* 21ms > 20ms：放行 */
    const rtp_reorder_stats_t *st = rtp_reorder_stats(ro);
    UT_EQ_INT(1, (int)st->gap_pkts);
    UT_EQ_INT(2, (int)st->delivered);

    /* 迟到的 2 号包：洞已放弃，只能弃（不能喂下游 —— 会毒 AU） */
    mk(b, 2); rtp_reorder_push(ro, 2, 2, b, PL, 1030);
    UT_EQ_INT(1, (int)st->late);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 6. NACK 重发节奏：retry_ms 内不重发，到点重发，超过次数上限停手
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, nack_retry_pacing_and_cap)
{
    /* hold 必须 > retry×次数，重发才有发生的余地 —— 这里 hold=200ms 专测节奏 */
    rtp_reorder_t *ro = rtp_reorder_new(16, 200);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */
    rtp_reorder_set_nack_policy(ro, true, 40, 3);

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 3); rtp_reorder_push(ro, 3, 3, b, PL, 1000);   /* 洞 2：首发 nacks=1 */
    UT_EQ_INT(1, nack_n);

    pop_one(ro, 1, 1000);
    rtp_reorder_tick(ro, 1039);                          /* 不到 40ms */
    UT_EQ_INT(1, nack_n);
    rtp_reorder_tick(ro, 1040);                          /* 到点重发 */
    UT_EQ_INT(2, nack_n);
    rtp_reorder_tick(ro, 1080);
    UT_EQ_INT(3, nack_n);
    rtp_reorder_tick(ro, 1120);                          /* 3 次上限到了 */
    UT_EQ_INT(3, nack_n);

    UT_EQ_INT(0, pop_one(ro, 0, 1150));                  /* hold（1200）内：洞还在等 */
    UT_EQ_INT(PL, pop_one(ro, 3, 1201));                 /* hold 到期：放弃洞，放行 3 */
    UT_EQ_INT(1, (int)rtp_reorder_stats(ro)->gap_pkts);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 7. NACK 关闭开关：一个请求都不该发
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, nack_can_be_disabled)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */
    rtp_reorder_set_nack_policy(ro, false, 40, 5);

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 3); rtp_reorder_push(ro, 3, 3, b, PL, 1000);
    UT_EQ_INT(0, nack_n);
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->nacks);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 8. 序号回绕：65534 → 65535 → 0 → 1 必须按序交付
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, seq16_wraparound)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    uint16_t seqs[4] = { 65534, 65535, 0, 1 };
    for (int i = 0; i < 4; i++) {
        mk(b, seqs[i]);
        rtp_reorder_push(ro, seqs[i], (uint32_t)i, b, PL, 1000);
    }
    for (int i = 0; i < 4; i++) UT_EQ_INT(PL, pop_one(ro, seqs[i], 1000));
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->holes);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 9. 大跳变（流重启/SSRC 换了）：清空重来，不发 NACK 风暴
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, big_jump_is_stream_reset_not_holes)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    nack_n = 0;
    mk(b, 1 + 0x5000);
    rtp_reorder_push(ro, (uint16_t)(1 + 0x5000), 2, b, PL, 1000);

    UT_EQ_INT(0, nack_n);                                /* 没有风暴 */
    UT_EQ_INT(1, (int)rtp_reorder_stats(ro)->reset);
    UT_EQ_INT(PL, pop_one(ro, (uint16_t)(1 + 0x5000), 1000));
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 10. 重复包：不重复交付、不毒化顺序
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, duplicates_are_dropped)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    mk(b, 2); rtp_reorder_push(ro, 2, 2, b, PL, 1000);
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1001);   /* 已交付过的重复 */
    mk(b, 2); rtp_reorder_push(ro, 2, 2, b, PL, 1001);   /* 在缓冲里的重复 */

    UT_EQ_INT(PL, pop_one(ro, 1, 1001));
    UT_EQ_INT(PL, pop_one(ro, 2, 1001));
    UT_EQ_INT(0, pop_one(ro, 0, 1001));                  /* 没有多余的包 */

    const rtp_reorder_stats_t *st = rtp_reorder_stats(ro);
    UT_EQ_INT(2, (int)st->delivered);
    UT_ASSERT(st->dup >= 1);
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 11. 缓冲满走保险丝：强弃最老的洞腾位置，不静默丢包
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, buffer_full_fuse)
{
    /* cap=4：1 在手，2..8 全缺（7 个洞）但缓冲只装得下 4 个后来的包 */
    rtp_reorder_t *ro = rtp_reorder_new(4, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 1); rtp_reorder_push(ro, 1, 1, b, PL, 1000);
    for (uint16_t s = 3; s <= 10; s++) {
        mk(b, s);
        rtp_reorder_push(ro, s, s, b, PL, 1000);
    }
    /* 保险丝触发了：要么放弃部分洞（gap_pkts>0），要么包进来了（delivered 有料），
       但绝不能静默：full 只允许在洞保险丝也腾不出位置时出现 */
    const rtp_reorder_stats_t *st = rtp_reorder_stats(ro);
    UT_ASSERT(st->gap_pkts > 0 || st->delivered > 0 || st->full > 0);

    /* 1 号包必须还能交付（它在最前面，任何实现都不该动它） */
    UT_EQ_INT(PL, pop_one(ro, 1, 1000));
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 12. 超长包：不进缓冲、有统计，不影响后续包
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, oversize_packet_is_dropped_with_stat)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    uint8_t big[2000];
    memset(big, 0xAA, sizeof(big));

    rtp_reorder_push(ro, 1, 1, big, (int)sizeof(big), 1000);
    UT_EQ_INT(1, (int)rtp_reorder_stats(ro)->oversize);

    uint8_t b[PL];
    mk(b, 2);
    rtp_reorder_push(ro, 2, 2, b, PL, 1000);
    UT_EQ_INT(PL, pop_one(ro, 2, 1000));
    rtp_reorder_free(ro);
}

/*---------------------------------------------------------------------------
 * 13. 静默期后恢复：推流停了一会儿再进来，不能把新包错标成迟到
 *---------------------------------------------------------------------------*/
UT_TEST(reorder, resume_after_silence)
{
    rtp_reorder_t *ro = rtp_reorder_new(16, 20);
    rtp_reorder_set_nack_cb(ro, nack_sink, NULL);
    nack_n = 0;      /* 全局计数器：用例间必须清零 */

    uint8_t b[PL];
    mk(b, 10); rtp_reorder_push(ro, 10, 10, b, PL, 1000);
    UT_EQ_INT(PL, pop_one(ro, 10, 1000));

    /* 静默 2 秒（洞超时早就过了，但缓冲是空的 —— next_seq 不许动） */
    mk(b, 11); rtp_reorder_push(ro, 11, 11, b, PL, 3000);
    UT_EQ_INT(PL, pop_one(ro, 11, 3000));
    UT_EQ_INT(0, (int)rtp_reorder_stats(ro)->late);
    rtp_reorder_free(ro);
}
