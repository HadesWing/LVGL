/**
 * @file rtp_reorder.c
 * @brief RTP 按序交付缓冲实现（见 rtp_reorder.h）
 *
 * 实现取舍：
 *   1) 洞线性扫不建索引 —— 上限 128，正常网络同时存在的洞是个位数，
 *      这个模块在收包热路径上，O(n) 常数小比花哨的数据结构重要。
 *   2) 包缓冲按 slot 定长（1600B × cap），不做动态队列 —— 无碎片，
 *      交叉编译到板子上行为完全确定。
 *   3) 时间全部由调用方传（同 resync 的理由：UT 才能把时序钉死）。
 *
 * 已经被 UT 抓过的边界（改代码前先看这里）：
 *   - seq == next_seq 的包：它可能是"补洞的重传包"也可能是首发，
 *     绝不能掉进大跳变分支（曾把重传包判成流重启，洞永远补不上）；
 *   - 登记洞之前必须跳过"已在缓冲里的序号" —— 否则 push 连发不发 pop，
 *     会给已经握在手里的包发 NACK；
 *   - 缓冲满是断流保险丝：强弃最老的洞腾位置，而不是静默丢包。
 */
#include "rtp_reorder.h"

#include <stdlib.h>
#include <string.h>

#define RO_MTU       1600   /* 覆盖常规视频 RTP 包；更大的包直接弃（统计可见） */
#define RO_HOLE_MAX  128    /* 同时登记的洞上限 —— 超过说明不是抖动是断流 */

typedef struct {
    bool     used;
    uint16_t seq;
    uint32_t ts;
    int      len;
    uint8_t *p;                /* 指向 payload 池 */
} ro_slot_t;

typedef struct {
    bool     live;
    uint16_t seq;
    uint64_t first_ms;         /* 洞出现的时刻 —— hold 从这里起算 */
    uint64_t last_ms;          /* 上次发 NACK 的时刻 */
    int      nacks;            /* 已发出的请求数 */
} ro_hole_t;

struct rtp_reorder {
    int      cap;
    uint32_t hold_ms;

    /* NACK 策略 */
    void    (*nack_cb)(uint16_t seq, void *user);
    void    *nack_user;
    bool     nack_enabled;
    uint32_t nack_retry_ms;
    int      nack_max;

    ro_slot_t *slot;           /* cap 个 */
    uint8_t  *pool;            /* cap × RO_MTU */
    ro_hole_t  hole[RO_HOLE_MAX];
    int        hole_n;

    uint16_t next_seq;         /* 下一个该交付的序号 */
    bool     seq_init;

    rtp_reorder_stats_t st;
};

/*---------------------------------------------------------------------------
 * 序号比较：全部走差值语义，绝不直接比大小（16 位会回绕）
 *---------------------------------------------------------------------------*/
static uint16_t seq_diff(uint16_t a, uint16_t b) { return (uint16_t)(a - b); }

/* a 是否在 b 前方（b 还没到 a） */
static bool seq_ahead(uint16_t a, uint16_t b)
{
    uint16_t d = seq_diff(a, b);
    return d != 0 && d < 0x8000;
}

static void advance_next(rtp_reorder_t *ro);
static ro_slot_t *slot_find(rtp_reorder_t *ro, uint16_t seq);   /* advance_next 要用 */

/*---------------------------------------------------------------------------
 * 洞管理
 *---------------------------------------------------------------------------*/
static ro_hole_t *hole_find(rtp_reorder_t *ro, uint16_t seq)
{
    for (int i = 0; i < ro->hole_n; i++)
        if (ro->hole[i].live && ro->hole[i].seq == seq) return &ro->hole[i];
    return NULL;
}

static void hole_fire_nack(rtp_reorder_t *ro, ro_hole_t *h, uint64_t now)
{
    if (!ro->nack_enabled || !ro->nack_cb) return;
    if (ro->nack_max > 0 && h->nacks >= ro->nack_max) return;
    ro->nack_cb(h->seq, ro->nack_user);
    h->nacks++;
    h->last_ms = now;
    ro->st.nacks++;
}

/* 登记一个新洞（重复登记静默忽略）；返回 false = 登记不下（断流保险丝） */
static bool hole_add(rtp_reorder_t *ro, uint16_t seq, uint64_t now)
{
    if (hole_find(ro, seq)) return true;

    ro->st.holes++;
    if (ro->hole_n >= RO_HOLE_MAX) {
        ro->st.overflow++;
        return false;
    }
    ro_hole_t *h = &ro->hole[ro->hole_n++];
    h->live     = true;
    h->seq      = seq;
    h->first_ms = now;
    h->last_ms  = now;
    h->nacks    = 0;
    hole_fire_nack(ro, h, now);                    /* 洞一出现当场就要 */
    return true;
}

/* 洞被重传补上了。返回 true = 它确实是个洞（计入成功） */
static bool hole_take(rtp_reorder_t *ro, uint16_t seq)
{
    for (int i = 0; i < ro->hole_n; i++) {
        if (ro->hole[i].live && ro->hole[i].seq == seq) {
            ro->hole[i].live = false;
            ro->st.filled++;
            return true;
        }
    }
    return false;
}

/* 放弃一个洞（超时/流重启/保险丝）：它代表的包正式判丢 */
static void hole_abandon(rtp_reorder_t *ro, ro_hole_t *h)
{
    h->live = false;
    ro->st.gap_pkts++;
}

/*---------------------------------------------------------------------------
 * next_seq 前移：跳过已判丢的洞，对齐到最小的在缓冲包。
 *
 * 只在"前方还有在缓冲的包"时推进 —— 缓冲全空时停住：流可能断了，
 * 盲目推进会把将来的包错标成"迟到"。
 *---------------------------------------------------------------------------*/
static void advance_next(rtp_reorder_t *ro)
{
    if (!ro->seq_init) return;
    for (;;) {
        if (slot_find(ro, ro->next_seq)) break;        /* 就绪，pop 去交付 */
        if (hole_find(ro, ro->next_seq)) break;        /* 洞还活着，等它 */
        bool any_ahead = false;
        for (int i = 0; i < ro->cap; i++) {
            if (ro->slot[i].used && seq_ahead(ro->slot[i].seq, ro->next_seq)) {
                any_ahead = true;
                break;
            }
        }
        if (!any_ahead) break;
        ro->next_seq++;
    }
}

/*---------------------------------------------------------------------------
 * 包槽位
 *---------------------------------------------------------------------------*/
static ro_slot_t *slot_find(rtp_reorder_t *ro, uint16_t seq)
{
    for (int i = 0; i < ro->cap; i++)
        if (ro->slot[i].used && ro->slot[i].seq == seq) return &ro->slot[i];
    return NULL;
}

static ro_slot_t *slot_free(rtp_reorder_t *ro)
{
    for (int i = 0; i < ro->cap; i++)
        if (!ro->slot[i].used) return &ro->slot[i];
    return NULL;
}

static bool slot_store(rtp_reorder_t *ro, uint16_t seq, uint32_t ts,
                       const uint8_t *payload, int len)
{
    ro_slot_t *s = slot_find(ro, seq);
    if (s) { ro->st.dup++; return true; }          /* 网络重复，已在缓冲 */

    s = slot_free(ro);
    if (!s) {
        /* 满了：强弃最老的洞腾位置（hold 窗口内的包量远小于 cap，
           走到这里基本是断流 —— 这是保险丝，不是常规路径）。 */
        for (int i = 0; i < ro->hole_n; i++) {
            if (ro->hole[i].live) { hole_abandon(ro, &ro->hole[i]); break; }
        }
        advance_next(ro);
        s = slot_free(ro);
        if (!s) { ro->st.full++; return false; }
    }
    s->used = true;
    s->seq  = seq;
    s->ts   = ts;
    s->len  = len;
    memcpy(s->p, payload, (size_t)len);
    return true;
}

/*---------------------------------------------------------------------------
 * 时钟驱动：放弃过期洞 + 重发 NACK + 前移 next_seq
 *
 * 放行策略：洞超时后判丢，next_seq 一次性前移到"最小的在缓冲包"——
 * 下游 depack 会看到一次序号跳变，走它既有的 poison → resync → PLI
 * 慢路径，本模块不再重复通知（职责单一，慢路径的统计归 resync）。
 *---------------------------------------------------------------------------*/
static void maintain(rtp_reorder_t *ro, uint64_t now)
{
    /* 1) 超时的洞判丢 */
    for (int i = 0; i < ro->hole_n; i++) {
        ro_hole_t *h = &ro->hole[i];
        if (h->live && ro->hold_ms > 0 &&
            now - h->first_ms >= (uint64_t)ro->hold_ms)
            hole_abandon(ro, h);
    }

    /* 2) NACK 重发（还没放弃、还没到次数上限的才有意义） */
    if (ro->nack_enabled && ro->nack_cb && ro->nack_retry_ms > 0) {
        for (int i = 0; i < ro->hole_n; i++) {
            ro_hole_t *h = &ro->hole[i];
            if (!h->live || h->nacks <= 0) continue;   /* 首发在 hole_add 里 */
            if (ro->nack_max > 0 && h->nacks >= ro->nack_max) continue;
            if (now - h->last_ms < (uint64_t)ro->nack_retry_ms) continue;
            hole_fire_nack(ro, h, now);
        }
    }

    /* 3) next_seq 前移 */
    advance_next(ro);
}

/*---------------------------------------------------------------------------
 * 流重启：清空缓冲与洞，但**保留统计** —— 中途一次 SSRC 重启不该把
 * 这一通电话已经积累的质量数据一起抹掉。残存的洞按判丢计。
 *---------------------------------------------------------------------------*/
static void flush_stream(rtp_reorder_t *ro)
{
    for (int i = 0; i < ro->cap; i++) ro->slot[i].used = false;
    for (int i = 0; i < ro->hole_n; i++) {
        if (ro->hole[i].live) hole_abandon(ro, &ro->hole[i]);
    }
    ro->hole_n   = 0;
    ro->seq_init = false;
}

/*---------------------------------------------------------------------------
 * 生命周期
 *---------------------------------------------------------------------------*/
rtp_reorder_t *rtp_reorder_new(int cap_packets, uint32_t hold_ms)
{
    if (cap_packets <= 0) cap_packets = 64;
    rtp_reorder_t *ro = (rtp_reorder_t *)calloc(1, sizeof(*ro));
    if (!ro) return NULL;
    ro->cap     = cap_packets;
    ro->hold_ms = hold_ms;
    ro->slot    = (ro_slot_t *)calloc((size_t)cap_packets, sizeof(ro_slot_t));
    ro->pool    = (uint8_t *)malloc((size_t)cap_packets * RO_MTU);
    if (!ro->slot || !ro->pool) {
        free(ro->slot);
        free(ro->pool);
        free(ro);
        return NULL;
    }
    for (int i = 0; i < cap_packets; i++) ro->slot[i].p = ro->pool + (size_t)i * RO_MTU;
    ro->nack_enabled  = true;
    ro->nack_retry_ms = 8;
    ro->nack_max      = 3;
    return ro;
}

void rtp_reorder_free(rtp_reorder_t *ro)
{
    if (!ro) return;
    free(ro->slot);
    free(ro->pool);
    free(ro);
}

void rtp_reorder_reset(rtp_reorder_t *ro)
{
    if (!ro) return;
    flush_stream(ro);
    memset(&ro->st, 0, sizeof(ro->st));
}

void rtp_reorder_set_nack_cb(rtp_reorder_t *ro,
                             void (*cb)(uint16_t seq, void *user), void *user)
{
    if (!ro) return;
    ro->nack_cb   = cb;
    ro->nack_user = user;
}

void rtp_reorder_set_nack_policy(rtp_reorder_t *ro, bool enabled,
                                 uint32_t retry_ms, int max_retries)
{
    if (!ro) return;
    ro->nack_enabled  = enabled;
    ro->nack_retry_ms = retry_ms;
    ro->nack_max      = max_retries;
}

/*---------------------------------------------------------------------------
 * 收包
 *---------------------------------------------------------------------------*/
void rtp_reorder_push(rtp_reorder_t *ro, uint16_t seq, uint32_t ts,
                      const uint8_t *payload, int len, uint64_t now_ms)
{
    if (!ro || !payload || len <= 0) return;
    ro->st.pushed++;

    if (len > RO_MTU) { ro->st.oversize++; return; }   /* 比缓冲槽还大：存不下 */

    /* seq == next_seq：可能是首发，也可能是"补 next_seq 这个洞的重传包"。
       两种情况都只是存起来等交付 —— **绝不能掉进大跳变分支**
       （那会把重传包判成流重启，洞永远补不上）。 */
    if (ro->seq_init && seq == ro->next_seq) {
        hole_take(ro, seq);              /* 它曾是个洞 → 记一笔重传成功 */
        slot_store(ro, seq, ts, payload, len);
        return;
    }

    if (!ro->seq_init) {
        ro->seq_init = true;
        ro->next_seq = seq;
        slot_store(ro, seq, ts, payload, len);
        return;
    }

    uint16_t d = seq_diff(seq, ro->next_seq);

    if (d != 0 && d < 0x4000) {
        /* --- 前方新包：中间缺的序号都是新洞。
             已在缓冲里的要跳过 —— 否则 push 连发、来不及 pop 时，
             会给已经握在手里的包发 NACK（UT 抓出来的边界）。 --- */
        for (uint16_t s = ro->next_seq; s != seq; s = (uint16_t)(s + 1)) {
            if (slot_find(ro, s)) continue;       /* 已缓冲，不是洞 */
            if (!hole_add(ro, s, now_ms)) {
                /* 洞登记不下：说明不是抖动是断流，整体重启，避免 NACK 风暴 */
                flush_stream(ro);
                ro->seq_init = true;
                ro->next_seq = seq;
                slot_store(ro, seq, ts, payload, len);
                ro->st.reset++;
                return;
            }
        }
        slot_store(ro, seq, ts, payload, len);
        return;
    }

    if (d >= 0xC000) {
        /* --- 后方旧包：要么是重传补洞，要么是迟到/重复 --- */
        if (hole_take(ro, seq)) {
            slot_store(ro, seq, ts, payload, len);   /* 补上，等按序交付 */
        } else {
            ro->st.late++;                           /* 洞已交付/放弃：用不上了 */
        }
        return;
    }

    /* --- 大跳变（0x4000~0xBFFF）：流重启，别当洞 --- */
    flush_stream(ro);
    ro->seq_init = true;
    ro->next_seq = seq;
    slot_store(ro, seq, ts, payload, len);
    ro->st.reset++;
}

/*---------------------------------------------------------------------------
 * 取包
 *---------------------------------------------------------------------------*/
int rtp_reorder_pop(rtp_reorder_t *ro, uint16_t *seq, uint32_t *ts,
                    const uint8_t **payload, uint64_t now_ms)
{
    if (!ro) return 0;
    maintain(ro, now_ms);

    ro_slot_t *s = ro->seq_init ? slot_find(ro, ro->next_seq) : NULL;
    if (!s) return 0;

    if (seq)     *seq     = s->seq;
    if (ts)      *ts      = s->ts;
    if (payload) *payload = s->p;
    int n = s->len;
    s->used = false;
    ro->next_seq = (uint16_t)(ro->next_seq + 1);
    ro->st.delivered++;
    return n;
}

void rtp_reorder_tick(rtp_reorder_t *ro, uint64_t now_ms)
{
    if (ro) maintain(ro, now_ms);
}

const rtp_reorder_stats_t *rtp_reorder_stats(const rtp_reorder_t *ro)
{
    return ro ? &ro->st : NULL;
}
