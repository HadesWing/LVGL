/**
 * @file resync.c
 * @brief 丢包恢复状态机实现（见 resync.h）
 */
#include "resync.h"

#include <string.h>

void resync_init(resync_t *r, bool enabled, uint32_t pli_interval_ms, uint32_t giveup_ms)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->enabled         = enabled;
    r->pli_interval_ms = pli_interval_ms ? pli_interval_ms : RESYNC_PLI_INTERVAL_MS_DEFAULT;
    r->giveup_ms       = giveup_ms;
}

void resync_reset(resync_t *r)
{
    if (!r) return;
    bool     en = r->enabled;
    uint32_t pi = r->pli_interval_ms;
    uint32_t gu = r->giveup_ms;
    memset(r, 0, sizeof(*r));
    r->enabled         = en;
    r->pli_interval_ms = pi;
    r->giveup_ms       = gu;
}

bool resync_on_loss(resync_t *r, uint64_t now_ms)
{
    if (!r || !r->enabled) return false;

    if (r->waiting) return false;      /* 不重置 wait_start，见头文件说明 */

    r->waiting       = true;
    r->wait_start_ms = now_ms;
    r->last_pli_ms   = now_ms;
    r->last_skipped  = 0;              /* 本轮重新计数 */
    r->events++;
    r->pli++;
    return true;                       /* 丢包当场就请求，别等下一个周期 */
}

bool resync_tick(resync_t *r, uint64_t now_ms)
{
    if (!r || !r->enabled || !r->waiting) return false;
    if (now_ms - r->last_pli_ms < (uint64_t)r->pli_interval_ms) return false;
    r->last_pli_ms = now_ms;
    r->pli++;
    return true;
}

resync_action_t resync_gate(resync_t *r, bool au_has_idr, uint64_t now_ms)
{
    if (!r || !r->enabled || !r->waiting) return RESYNC_FEED;

    if (au_has_idr) {
        uint64_t w = now_ms - r->wait_start_ms;
        r->waiting      = false;
        r->resumed++;
        r->last_wait_ms = w;
        if (w > r->max_wait_ms) r->max_wait_ms = w;
        return RESYNC_RESUME;
    }

    /* 等太久了：画面花的但能动，比长时间黑屏有用（取舍见头文件） */
    if (r->giveup_ms && (now_ms - r->wait_start_ms) >= (uint64_t)r->giveup_ms) {
        r->waiting = false;
        r->gave_up++;
        return RESYNC_GIVEUP;
    }

    r->skipped++;
    r->last_skipped++;
    return RESYNC_SKIP;
}

bool resync_waiting(const resync_t *r)
{
    return r && r->enabled && r->waiting;
}
