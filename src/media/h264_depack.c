/**
 * @file h264_depack.c
 * @brief H.264 RTP 解包与 AU 重组实现（见 h264_depack.h）
 *
 * 实现要点：
 *
 * 1) FU-A 分片**直接追加进 AU 缓冲**，不另开一块 FU 缓冲。分片断裂时把
 *    au_len 回退到本 NAL 的起始偏移即可 —— 少一块 256KB 内存，也少一次拷贝。
 *
 * 2) AU 边界用 RTP 时间戳判，不解析 slice header。原因见头文件：
 *    SPS/PPS/SEI 不属于 slice，解析 slice header 对它们无从下手，而时间戳
 *    对同一帧的所有 NAL 都一样，判据统一。
 *
 * 3) 所有"丢包/错序"都必须**计数**。视频出问题时最常见的问法是
 *    "是网络丢包还是解码错"，没有计数就只能猜。
 */
#include "h264_depack.h"

#include <stdlib.h>
#include <string.h>

/* Annex-B 起始码。4 字节版本比 3 字节更安全：码流内部出现 00 00 01 时，
   解码器不会把数据误认成 NAL 边界（这是 H.264 裸流解析的经典坑）。 */
static const uint8_t k_start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

struct h264_depack {
    uint8_t *au;              /* 当前累积的 AU（Annex-B） */
    int      au_len;
    uint32_t au_ts;
    bool     au_open;
    bool     au_dirty;        /* 本 AU 已被丢包污染 —— 收尾时必须整帧丢弃 */
    bool     au_has_idr;      /* 本 AU 里有 IDR */

    uint8_t *done;            /* 已完成、等待取走的 AU */
    int      done_len;
    uint32_t done_ts;
    bool     done_ready;
    bool     done_has_idr;

    uint16_t last_seq;        /* 上一个包的 RTP 序号（16 位会回绕） */
    bool     seq_init;

    int      cur_nal_off;     /* 本 NAL 在 au 中的起点（断裂时回退用） */
    bool     fu_active;
    uint8_t  fu_hdr;          /* 正在重组的 NAL header */

    h264_depack_stats_t st;
};

/*---------------------------------------------------------------------------
 * 缓冲辅助
 *---------------------------------------------------------------------------*/
static bool au_room(const h264_depack_t *d, int need)
{
    /* 用 long 计算，避免 need 很大时 int 溢出后"看起来还有空间" */
    long total = (long)d->au_len + (long)need;
    return total <= (long)H264_DEPACK_AU_MAX;
}

static void au_put(h264_depack_t *d, const uint8_t *p, int n)
{
    memcpy(d->au + d->au_len, p, (size_t)n);
    d->au_len += n;
}

/*---------------------------------------------------------------------------
 * 生命周期
 *---------------------------------------------------------------------------*/
h264_depack_t *h264_depack_new(void)
{
    h264_depack_t *d = (h264_depack_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->au   = (uint8_t *)malloc(H264_DEPACK_AU_MAX);
    /* done 缓冲只在"帧完成到被取走"之间用。为了不为它再吃 256KB，
       这里也按同样大小分配 —— RK3308 有 128MB DDR，512KB 总共不是问题；
       真到极限内存的设备可以把它改成"由上层直接持有 AU 指针"的零拷贝接口。 */
    d->done = (uint8_t *)malloc(H264_DEPACK_AU_MAX);
    if (!d->au || !d->done) {
        free(d->au);
        free(d->done);
        free(d);
        return NULL;
    }
    return d;
}

void h264_depack_free(h264_depack_t *d)
{
    if (!d) return;
    free(d->au);
    free(d->done);
    free(d);
}

void h264_depack_reset(h264_depack_t *d)
{
    if (!d) return;
    d->au_len      = 0;
    d->au_open     = false;
    d->au_ts       = 0;
    d->au_dirty    = false;
    d->au_has_idr  = false;
    d->done_len    = 0;
    d->done_ready  = false;
    d->done_has_idr = false;
    d->cur_nal_off = 0;
    d->fu_active   = false;
    d->seq_init    = false;
}

const h264_depack_stats_t *h264_depack_stats(const h264_depack_t *d)
{
    return d ? &d->st : NULL;
}

/*---------------------------------------------------------------------------
 * 开始一个新 NAL：写起始码 + NAL header，记住起点
 *---------------------------------------------------------------------------*/
static bool nal_begin(h264_depack_t *d, uint8_t nal_hdr)
{
    if (!au_room(d, 5)) {          /* 4 字节起始码 + 1 字节头 */
        d->st.drop_oversize++;
        return false;
    }
    d->cur_nal_off = d->au_len;
    au_put(d, k_start_code, 4);
    d->au[ d->au_len++ ] = nal_hdr;
    if ((nal_hdr & 0x1F) == H264_NAL_IDR) d->au_has_idr = true;
    return true;
}

/*---------------------------------------------------------------------------
 * 完成当前 AU：搬到 done 槽
 *---------------------------------------------------------------------------*/
static void au_finish(h264_depack_t *d)
{
    if (d->au_len <= 0) return;

    /* 脏帧一律不交出去。
     *
     * 这是"丢包恢复"能成立的**前提**：只要这个 AU 缺过数据，就整帧丢弃，
     * 让上层（resync）去等下一个 IDR。曾经的做法是"回退掉残缺的那个 NAL，
     * 剩下的照发" —— 看起来更"完整"，实际上是把一帧缺了数据的图交给解码器，
     * 解码器不报错、只是画错，而且错误会顺着参考链传到后面每一帧。
     * 逐字节比对（make duo-video）能照出这种事，靠眼睛看只会得出"信号不好"。 */
    if (d->au_dirty) {
        d->st.au_dropped++;
        d->au_len     = 0;
        d->au_open    = false;
        d->au_dirty   = false;
        d->au_has_idr = false;
        d->fu_active  = false;
        return;
    }

    if (d->done_ready) {
        /* 上一帧还没被取走。说明上层处理速度跟不上码流 —— 实时场景下
           丢掉旧的比堆积起来更合理（堆积只会让延迟越来越大）。 */
        d->st.drop_slow++;
    }
    memcpy(d->done, d->au, (size_t)d->au_len);
    d->done_len     = d->au_len;
    d->done_ts      = d->au_ts;
    d->done_ready   = true;
    d->done_has_idr = d->au_has_idr;

    d->st.au_done++;
    d->au_len     = 0;
    d->au_open    = false;
    d->au_has_idr = false;
    d->fu_active  = false;
}

/* 一个包丢了：当前 AU 已经不可能完整，标记为脏。
   注意**不能**在这里直接把 au_len 清零并关掉 au_open —— 同一帧后面还有包
   要进来（它们的 ts 没变），如果关掉 au_open，下一包会被当成新帧的开头，
   于是我们会把"半帧"当成完整 AU 收尾交出去，比丢帧更糟。 */
static void au_poison(h264_depack_t *d)
{
    if (d->au_open) d->au_dirty = true;
}

/*---------------------------------------------------------------------------
 * 喂包
 *---------------------------------------------------------------------------*/
int h264_depack_push(h264_depack_t *d, const uint8_t *payload, int len,
                     uint32_t ts, uint16_t seq)
{
    if (!d || !payload || len < 1) return -1;
    d->st.pkt_in++;

    /* --- 序号连续性：这是"有没有丢包"的唯一判据 ---------------------------
     * 16 位序号会回绕，所以必须用 (uint16_t)(last + 1) 比较，
     * 直接写 seq != last + 1 在 65535 → 0 会误判成丢包（1/65536 的概率，
     * 但通话久了必然遇上，而且现象是"毫无征兆地花一下"）。 */
    if (!d->seq_init) {
        d->seq_init = true;
    } else if (seq != (uint16_t)(d->last_seq + 1)) {
        d->st.gap++;
        au_poison(d);
    }
    d->last_seq = seq;

    /* --- AU 边界：时间戳变了就收尾 --- */
    if (!d->au_open) {
        d->au_open = true;
        d->au_ts   = ts;
    } else if (ts != d->au_ts) {
        au_finish(d);
        d->au_open = true;
        d->au_ts   = ts;
    }

    uint8_t nal_type = payload[0] & 0x1F;

    /*--- 单 NAL 单元包 ---------------------------------------------------*/
    if (nal_type >= 1 && nal_type <= 23) {
        if (!nal_begin(d, payload[0])) return -2;
        if (!au_room(d, len - 1)) { d->au_len = d->cur_nal_off; d->st.drop_oversize++; return -2; }
        au_put(d, payload + 1, len - 1);
        d->st.nal_done++;
        return 0;
    }

    /*--- STAP-A：多个小 NAL 聚在一个包里 --------------------------------*/
    if (nal_type == H264_NAL_STAP_A) {
        int off = 1;
        int got = 0;
        while (off + 2 <= len) {
            int sz = (payload[off] << 8) | payload[off + 1];
            off += 2;
            if (sz <= 0 || off + sz > len) break;    /* 长度字段坏了，剩下的都不要 */

            /* STAP-A 只聚合整个 NAL，不需要起始码之外的处理 */
            if (!au_room(d, sz + 4)) { d->st.drop_oversize++; break; }
            au_put(d, k_start_code, 4);
            au_put(d, payload + off, sz);
            if ((payload[off] & 0x1F) == H264_NAL_IDR) d->au_has_idr = true;
            off += sz;
            got++;
            d->st.nal_done++;
        }
        if (got) d->st.stap_pkt++;
        else     d->st.drop_type++;
        return got ? 0 : -3;
    }

    /*--- FU-A：一个 NAL 被切成多片 --------------------------------------*/
    if (nal_type == H264_NAL_FU_A) {
        if (len < 2) return -1;

        uint8_t fu_ind = payload[0];
        uint8_t fu_hdr = payload[1];
        bool    start  = (fu_hdr & 0x80) != 0;
        bool    end    = (fu_hdr & 0x40) != 0;
        uint8_t type   = fu_hdr & 0x1F;

        if (start) {
            /* 上一个 NAL 若还没收到 E 片，就是序列断了：把它整段丢掉，
               并且**整个 AU 作废** —— 缺片的不只是这个 NAL 的数据，
               解码器拿到这帧会画错，还会把错误顺着参考链传下去。 */
            if (d->fu_active) {
                d->st.fu_broken++;
                d->au_len = d->cur_nal_off;
                d->fu_active = false;
                d->au_dirty = true;
            }
            /* 重组后的 NAL header = FU indicator 的 F/NRI 位 + FU header 的 type */
            uint8_t nal_hdr = (uint8_t)(fu_ind & 0xE0) | type;
            if (!nal_begin(d, nal_hdr)) return -2;
            d->fu_hdr    = nal_hdr;
            d->fu_active = true;

        } else {
            if (!d->fu_active) {
                /* 中间片先到：首片丢了。这片没有任何上下文，只能丢，
                   而且这一帧已经不可能还原了。 */
                d->st.fu_broken++;
                d->au_dirty = true;
                return -4;
            }
            /* 分片类型和当前 NAL 不一致 = 序列已经被打断（可能中间丢了片） */
            uint8_t expect = (uint8_t)((fu_ind & 0xE0) | type);
            if (expect != d->fu_hdr) {
                d->st.fu_broken++;
                d->au_len    = d->cur_nal_off;
                d->fu_active = false;
                d->au_dirty  = true;
                return -4;
            }
        }

        if (!au_room(d, len - 2)) {
            d->st.drop_oversize++;
            d->au_len    = d->cur_nal_off;
            d->fu_active = false;
            return -2;
        }
        au_put(d, payload + 2, len - 2);

        if (end) {
            d->fu_active = false;
            d->st.fu_seq++;
            d->st.nal_done++;
        }
        return 0;
    }

    /*--- 不支持的类型（STAP-B/MTAP/FU-B…）-------------------------------*/
    d->st.drop_type++;
    return -5;
}

/*---------------------------------------------------------------------------
 * 取 AU
 *---------------------------------------------------------------------------*/
int h264_depack_pop(h264_depack_t *d, const uint8_t **p_au, uint32_t *p_ts,
                    bool *p_has_idr)
{
    if (!d || !d->done_ready) return 0;
    if (p_au)       *p_au       = d->done;
    if (p_ts)       *p_ts       = d->done_ts;
    if (p_has_idr)  *p_has_idr  = d->done_has_idr;
    int n = d->done_len;
    d->done_ready   = false;
    d->done_len     = 0;
    d->done_has_idr = false;
    return n;
}
