/**
 * @file ut_resync.c
 * @brief 丢包恢复状态机用例
 *
 * 这段逻辑几乎不可能靠端到端脚本覆盖：丢包发生在哪一帧、对端什么时候
 * 发下一个 IDR，都是不可控的。所以把状态机单独抽出来（src/media/resync.c），
 * 用**手工推进的时间**把各种时序钉死。
 *
 * 时间单位统一是毫秒，由用例自己传 —— 状态机不读时钟，这是能被这样测的前提。
 */
#include "ut.h"
#include "resync.h"

/*---------------------------------------------------------------------------
 * 1. 关闭时：一切都直通，绝不拦帧，也绝不发 PLI
 *
 *  这条看着像废话，但它保证"关掉恢复功能"这条路始终可用 ——
 *  真出问题时能立刻退到"跟以前一样"的状态做对照（A/B 定位法）。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, disabled_never_blocks_and_never_asks_pli)
{
    resync_t r;
    resync_init(&r, false, 500, 3000);

    UT_ASSERT(!resync_on_loss(&r, 1000));
    UT_EQ_INT(0, r.events);
    UT_EQ_INT(0, r.pli);
    UT_ASSERT(!resync_waiting(&r));

    for (int i = 0; i < 5; i++)
        UT_EQ_INT(RESYNC_FEED, resync_gate(&r, false, 2000 + i * 100));
}

/*---------------------------------------------------------------------------
 * 2. 丢了包 → 进入等待，并当场请求一次关键帧
 *
 * "当场请求"是刻意的：PLI 走 UDP，可能丢。如果等下一个重发周期（500ms）
 * 才发，恢复时间就是 500ms + RTT + 编码时间，白白多等半秒。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, loss_enters_waiting_and_asks_pli_immediately)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);

    UT_ASSERT(resync_on_loss(&r, 1000));
    UT_ASSERT(resync_waiting(&r));
    UT_EQ_INT(1, r.events);
    UT_EQ_INT(1, r.pli);

    /* 等待中再来一次丢包：不重复计 events，也不重复请求 */
    UT_ASSERT(!resync_on_loss(&r, 1200));
    UT_EQ_INT(1, r.events);
    UT_EQ_INT(1, r.pli);
}

/*---------------------------------------------------------------------------
 * 3. 等待期间一律跳过，直到 IDR 回来
 *---------------------------------------------------------------------------*/
UT_TEST(resync, gate_skips_everything_until_idr)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);

    UT_ASSERT(resync_on_loss(&r, 1000));

    for (int i = 0; i < 30; i++)
        UT_EQ_INT(RESYNC_SKIP, resync_gate(&r, false, 1100 + i * 33));

    UT_EQ_INT(RESYNC_RESUME, resync_gate(&r, true, 2100));
    UT_ASSERT(!resync_waiting(&r));
    UT_EQ_INT(1, r.resumed);
    UT_EQ_INT(30, r.skipped);
    UT_EQ_INT(30, r.last_skipped);        /* 本轮跳过数（日志里打的就是它） */
    UT_EQ_INT(1100, r.last_wait_ms);      /* 2100 - 1000 */

    /* 恢复之后就回到正常喂帧 */
    UT_EQ_INT(RESYNC_FEED, resync_gate(&r, false, 2200));
}

/*---------------------------------------------------------------------------
 * 3b. 第二次丢包时 last_skipped 必须归零
 *
 * 累计值（skipped）会一直涨，那是对的；但"本轮跳过多少帧"要是也跟着涨，
 * 日志里就会出现"跳过 200 帧、耗时 40 毫秒"这种自相矛盾的行。
 * 这条是补一个真的漏掉的赋值 —— 端到端日志里打出 `skipped=0` 才发现。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, last_skipped_resets_per_round)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);

    UT_ASSERT(resync_on_loss(&r, 1000));
    for (int i = 0; i < 5; i++) resync_gate(&r, false, 1010 + i);
    UT_EQ_INT(5, r.last_skipped);

    UT_EQ_INT(RESYNC_RESUME, resync_gate(&r, true, 1100));

    /* 第二轮：跳过 2 帧后恢复 */
    UT_ASSERT(resync_on_loss(&r, 2000));
    UT_EQ_INT(0, r.last_skipped);          /* 新一轮从 0 开始 */
    resync_gate(&r, false, 2010);
    resync_gate(&r, false, 2020);
    UT_EQ_INT(RESYNC_RESUME, resync_gate(&r, true, 2030));

    UT_EQ_INT(2, r.last_skipped);
    UT_EQ_INT(7, r.skipped);               /* 累计值不受影响 */
}

/*---------------------------------------------------------------------------
 * 4. 恢复耗时从**第一次丢包**算起，等待中再丢包不清零
 *
 * 如果每次丢包都重置计时，连续丢包时"恢复耗时"会一直显示得很好看，
 * 而用户实际看到的是画面长时间卡住 —— 指标骗人的典型方式。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, recover_time_counts_from_first_loss)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);

    UT_ASSERT(resync_on_loss(&r, 1000));       /* 第一次丢包 */
    UT_ASSERT(!resync_on_loss(&r, 1500));       /* 等待中又丢 */
    UT_ASSERT(!resync_on_loss(&r, 2000));       /* 又丢 */

    UT_EQ_INT(1, r.events);                     /* 事件只算一次 */

    UT_EQ_INT(RESYNC_SKIP,   resync_gate(&r, false, 2050));
    UT_EQ_INT(RESYNC_RESUME, resync_gate(&r, true, 2500));
    UT_EQ_INT(1500, r.last_wait_ms);            /* 2500 - 1000，不是 500 */
    UT_EQ_INT(1500, r.max_wait_ms);
}

/*---------------------------------------------------------------------------
 * 5. PLI 重发要限流
 *
 * 一个 RTP 流 30fps 每秒几十个包，如果每个包都发一次 PLI，
 * 对端就要每秒编几十个 I 帧 —— 码率暴涨、网络更堵、丢更多。
 * 500ms 一次是"反应够快"和"不刷屏"之间的折中。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, pli_resend_is_rate_limited)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);
    UT_ASSERT(resync_on_loss(&r, 1000));       /* 第 1 次：当场发 */

    UT_ASSERT(!resync_tick(&r, 1100));          /* 才过 100ms，不发 */
    UT_ASSERT(!resync_tick(&r, 1400));          /* 400ms，不发 */
    UT_ASSERT( resync_tick(&r, 1500));          /* 500ms，发 */
    UT_ASSERT(!resync_tick(&r, 1600));          /* 距离上次 100ms，不发 */
    UT_ASSERT( resync_tick(&r, 2050));          /* 550ms，发 */

    UT_EQ_INT(3, r.pli);
}

/*---------------------------------------------------------------------------
 * 6. 等太久要放弃：画面花的但能动，比一直黑着强
 *
 * 门口机的用途是"看清门口是谁"。黑屏 = 完全没信息；花屏 = 至少有轮廓和动作。
 * 超时值默认 3 秒（GOP 通常 2 秒，留一倍余量），设 0 表示永不放弃。
 *---------------------------------------------------------------------------*/
UT_TEST(resync, gives_up_after_timeout_and_keeps_feeding)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);
    UT_ASSERT(resync_on_loss(&r, 1000));

    UT_EQ_INT(RESYNC_SKIP,   resync_gate(&r, false, 2000));   /* 还在等 */
    UT_EQ_INT(RESYNC_GIVEUP, resync_gate(&r, false, 4100));       /* 超时了 */
    UT_ASSERT(!resync_waiting(&r));

    /* 放弃后就恢复正常喂帧（哪怕画面是花的） */
    UT_EQ_INT(RESYNC_FEED, resync_gate(&r, false, 4200));
    UT_EQ_INT(1, r.gave_up);
}

/*---------------------------------------------------------------------------
 * 7. 关掉之后又能重新打开（换一路流 / 通话中途重启视频）
 *---------------------------------------------------------------------------*/
UT_TEST(resync, reset_returns_to_feeding)
{
    resync_t r;
    resync_init(&r, true, 500, 3000);

    UT_ASSERT(resync_on_loss(&r, 1000));
    UT_ASSERT(resync_waiting(&r));
    UT_EQ_INT(RESYNC_SKIP, resync_gate(&r, false, 1100));

    resync_reset(&r);

    UT_ASSERT(!resync_waiting(&r));
    UT_EQ_INT(RESYNC_FEED, resync_gate(&r, false, 1200));

    /* 状态清了，但配置项要留着 —— reset 不等于重新 init */
    UT_ASSERT(r.enabled);
    UT_EQ_INT(500, r.pli_interval_ms);
    UT_EQ_INT(3000, r.giveup_ms);
}
