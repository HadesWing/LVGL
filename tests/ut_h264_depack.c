/**
 * @file ut_h264_depack.c
 * @brief H.264 RTP 解包/重组用例
 *
 * 这一层的 bug 有个共同特点：**不崩、不报错，只是画面烂**。
 * 分片少拼一个字节 → 花屏；起始码写成 3 字节而码流里正好有 00 00 01 → 帧被截断；
 * 丢片后不回退 → 把两帧的数据拼在一起 → 解码器输出绿块。
 * 这些在屏幕上看起来都差不多，只有把字节级结构钉死才能定位。
 *
 * 所以用例的重点是**逐字节校验重组结果**，而不是"能解出图就算过"。
 */
#include "ut.h"
#include "h264_depack.h"

#include <stdlib.h>

/*---------------------------------------------------------------------------
 * 构造 RTP payload 的小工具
 *---------------------------------------------------------------------------*/
static int mk_single(uint8_t *out, uint8_t type, const uint8_t *data, int n)
{
    out[0] = (uint8_t)(0x60 | type);      /* NRI=3, type */
    memcpy(out + 1, data, (size_t)n);
    return n + 1;
}

static int mk_fu_a(uint8_t *out, uint8_t type, bool start, bool end,
                   const uint8_t *data, int n)
{
    out[0] = (uint8_t)(0x60 | H264_NAL_FU_A);                 /* FU indicator */
    out[1] = (uint8_t)((start ? 0x80 : 0) | (end ? 0x40 : 0) | type);
    memcpy(out + 2, data, (size_t)n);
    return n + 2;
}

/* 在 Annex-B 流里找第 idx 个指定类型的 NAL，返回正文偏移与长度。
 *
 * 注意末尾那个 NAL：它后面没有起始码，长度只能算到缓冲结尾。
 * 第一版这里写成 `j + 3 <= au_len`，最后一个 NAL 会把 j 推到 au_len-3，
 * 少算 3 字节、还会越界读 1 字节 —— 写测试用例时自己踩的坑，记着。 */
static bool find_nal(const uint8_t *au, int au_len, int want_type, int idx,
                     int *p_off, int *p_len)
{
    int cur = 0;
    for (int i = 0; i + 5 <= au_len; ) {
        if (au[i] == 0 && au[i+1] == 0 && au[i+2] == 0 && au[i+3] == 1) {
            int body    = i + 4;
            int hdr_type = au[body] & 0x1F;

            int j = body;
            while (j + 4 <= au_len) {
                if (au[j] == 0 && au[j+1] == 0 && au[j+2] == 0 && au[j+3] == 1)
                    break;
                j++;
            }
            /* 循环可能因"读不下 4 字节"退出而不是因为找到了起始码，
               这时本 NAL 一直到 AU 末尾。两种退出必须分开处理 ——
               混在一起就会少算 3 字节（下面那句 i = j 也同理）。 */
            int end = (j + 4 <= au_len) ? j : au_len;
            int nal_len = end - body;

            if (nal_len > 0 && hdr_type == want_type) {
                if (cur == idx) {
                    *p_off = body;
                    *p_len = nal_len;
                    return true;
                }
                cur++;
            }
            i = (j > i) ? j : i + 4;   /* 防死循环：j 可能等于 i */
        } else {
            i++;
        }
    }
    return false;
}

static int count_nals(const uint8_t *au, int au_len)
{
    int n = 0;
    for (int i = 0; i + 4 <= au_len; i++)
        if (au[i] == 0 && au[i+1] == 0 && au[i+2] == 0 && au[i+3] == 1) n++;
    return n;
}

/*---------------------------------------------------------------------------
 * 1. 单 NAL 包：靠时间戳变化收尾
 *---------------------------------------------------------------------------*/
UT_TEST(depack, single_nal_au_completes_on_ts_change)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    static uint8_t pkt[256];
    static const uint8_t sps[] = { 0x67, 0x64, 0x00, 0x1F };   /* NAL 头 + 3 字节 */
    static const uint8_t pps[] = { 0x68, 0xEE, 0x3C };
    static const uint8_t idr[] = { 0x65, 0x88, 0x84, 0x00, 0x33 };

    int n;
    n = mk_single(pkt, H264_NAL_SPS, sps + 1, (int)sizeof(sps) - 1);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 1000));
    n = mk_single(pkt, H264_NAL_PPS, pps + 1, (int)sizeof(pps) - 1);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 1000));
    n = mk_single(pkt, H264_NAL_IDR, idr + 1, (int)sizeof(idr) - 1);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 1000));

    /* 还没换 ts，不该有完整 AU */
    const uint8_t *au; uint32_t ts;
    UT_EQ_INT(0, h264_depack_pop(d, &au, &ts));

    /* 下一帧的第一个包到了 → 上一帧收尾 */
    n = mk_single(pkt, H264_NAL_SLICE, idr + 1, 3);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 2000));

    int len = h264_depack_pop(d, &au, &ts);
    UT_ASSERT(len > 0);
    UT_EQ_INT(1000, ts);
    UT_EQ_INT(3, count_nals(au, len));

    /* 每个 NAL 都必须带 4 字节起始码，否则解码器认不出边界 */
    UT_EQ_INT(0, au[0]);
    UT_EQ_INT(0, au[1]);
    UT_EQ_INT(0, au[2]);
    UT_EQ_INT(1, au[3]);
    UT_EQ_INT(H264_NAL_SPS, au[4] & 0x1F);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 2. FU-A 三分片重组：逐字节比对原始 NAL
 *---------------------------------------------------------------------------*/
UT_TEST(depack, fu_a_reassembles_byte_exact)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    enum { NAL_BODY = 300 };
    static uint8_t body[NAL_BODY];
    for (int i = 0; i < NAL_BODY; i++) body[i] = (uint8_t)(i * 7 + 3);

    static uint8_t pkt[512];
    int chunk = NAL_BODY / 3;

    /* 第一片：S=1 E=0 */
    int n = mk_fu_a(pkt, H264_NAL_IDR, true, false, body, chunk);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 500));
    /* 中间片 */
    n = mk_fu_a(pkt, H264_NAL_IDR, false, false, body + chunk, chunk);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 500));
    /* 末片：E=1 */
    n = mk_fu_a(pkt, H264_NAL_IDR, false, true, body + chunk * 2, NAL_BODY - chunk * 2);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 500));

    /* 下一帧触发收尾 */
    n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    UT_EQ_INT(0, h264_depack_push(d, pkt, n, 9000));

    const uint8_t *au; uint32_t ts;
    int len = h264_depack_pop(d, &au, &ts);
    UT_ASSERT(len > 0);
    UT_EQ_INT(305, len);      /* 4 起始码 + 1 NAL 头 + 300 正文 */
    UT_EQ_INT(500, ts);

    int off, nal_len;
    UT_ASSERT(find_nal(au, len, H264_NAL_IDR, 0, &off, &nal_len));
    /* 重组结果 = 1 字节 NAL 头 + 原始正文，必须一字节不差 */
    UT_EQ_INT(NAL_BODY + 1, nal_len);
    UT_EQ_INT(0x60 | H264_NAL_IDR, au[off]);
    UT_MEM_EQ(body, au + off + 1, NAL_BODY);

    const h264_depack_stats_t *s = h264_depack_stats(d);
    UT_EQ_INT(1, s->fu_seq);
    UT_EQ_INT(0, s->fu_broken);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 3. 丢首片：中间片没有任何上下文，必须整体丢弃而不是硬拼
 *---------------------------------------------------------------------------*/
UT_TEST(depack, fu_a_without_first_fragment_is_dropped)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    static uint8_t body[100];
    memset(body, 0x5A, sizeof(body));
    static uint8_t pkt[128];

    /* 直接来中间片（首片在网络上丢了） */
    int n = mk_fu_a(pkt, H264_NAL_IDR, false, false, body, 50);
    UT_NE_INT(0, h264_depack_push(d, pkt, n, 100));

    /* 再来末片 */
    n = mk_fu_a(pkt, H264_NAL_IDR, false, true, body + 50, 50);
    UT_NE_INT(0, h264_depack_push(d, pkt, n, 100));

    const h264_depack_stats_t *s = h264_depack_stats(d);
    UT_ASSERT(s->fu_broken >= 1);

    /* 触发收尾，AU 里不该有这个残缺 NAL */
    n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    h264_depack_push(d, pkt, n, 200);

    const uint8_t *au; uint32_t ts;
    int len = h264_depack_pop(d, &au, &ts);
    if (len > 0) {
        int off, nal_len;
        UT_ASSERT_MSG(!find_nal(au, len, H264_NAL_IDR, 0, &off, &nal_len),
                      "残缺的 IDR 不该出现在 AU 里");
    }
    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 4. 分片序列被打断：前一个 NAL 必须回退掉
 *
 * 场景：S 片到了、中间丢了、然后发送端开始发**下一个** NAL 的 S 片。
 * 如果此时不回退，两个 NAL 的数据会拼成一个"看起来完整"的 NAL ——
 * 解码器会解出一帧撕裂的画面，而且不会报错。
 *---------------------------------------------------------------------------*/
UT_TEST(depack, interrupted_fu_sequence_rolls_back)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    static uint8_t body[200];
    for (int i = 0; i < 200; i++) body[i] = (uint8_t)i;
    static uint8_t pkt[256];

    /* 第一个 NAL 的 S 片（只有开头，中间丢了） */
    int n = mk_fu_a(pkt, H264_NAL_IDR, true, false, body, 60);
    h264_depack_push(d, pkt, n, 100);

    /* 直接来下一个 NAL 的 S 片 */
    n = mk_fu_a(pkt, H264_NAL_SLICE, true, false, body + 100, 60);
    h264_depack_push(d, pkt, n, 100);
    n = mk_fu_a(pkt, H264_NAL_SLICE, false, true, body + 160, 40);
    h264_depack_push(d, pkt, n, 100);

    const h264_depack_stats_t *s = h264_depack_stats(d);
    UT_EQ_INT(1, s->fu_broken);

    /* 收尾后 AU 里只应有那个完整的 SLICE，IDR 的残片必须已被回退 */
    n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    h264_depack_push(d, pkt, n, 999);

    const uint8_t *au; uint32_t ts;
    int len = h264_depack_pop(d, &au, &ts);
    UT_ASSERT(len > 0);

    int off, nal_len;
    UT_ASSERT_MSG(!find_nal(au, len, H264_NAL_IDR, 0, &off, &nal_len),
                  "被打断的 IDR 残片必须回退，不能留在 AU 里");
    UT_ASSERT(find_nal(au, len, H264_NAL_SLICE, 0, &off, &nal_len));
    UT_EQ_INT(101, nal_len);   /* 1 字节头 + 60 + 40 字节正文 */

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 5. STAP-A：SPS+PPS 聚合包拆成两个 NAL
 *---------------------------------------------------------------------------*/
UT_TEST(depack, stap_a_splits_into_nals)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    static const uint8_t sps_body[] = { 0x64, 0x00, 0x1F, 0xFF, 0xE1 };
    static const uint8_t pps_body[] = { 0xEE, 0x3C, 0x80 };

    uint8_t pkt[64];
    int k = 0;
    pkt[k++] = (uint8_t)(0x60 | H264_NAL_STAP_A);
    pkt[k++] = 0; pkt[k++] = (uint8_t)(sizeof(sps_body) + 1);  /* SPS: 头 + 正文 */
    pkt[k++] = (uint8_t)(0x60 | H264_NAL_SPS);
    memcpy(pkt + k, sps_body, sizeof(sps_body)); k += (int)sizeof(sps_body);
    pkt[k++] = 0; pkt[k++] = (uint8_t)(sizeof(pps_body) + 1);
    pkt[k++] = (uint8_t)(0x60 | H264_NAL_PPS);
    memcpy(pkt + k, pps_body, sizeof(pps_body)); k += (int)sizeof(pps_body);

    UT_EQ_INT(0, h264_depack_push(d, pkt, k, 10));

    /* 收尾 */
    uint8_t t[8];
    int n = mk_single(t, H264_NAL_SLICE, sps_body, 3);
    h264_depack_push(d, t, n, 20);

    const uint8_t *au; uint32_t ts;
    int len = h264_depack_pop(d, &au, &ts);
    UT_ASSERT(len > 0);
    /* SPS + PPS 是同一个 AU 里的两个 NAL。
       注意：触发收尾的那个 SLICE 包属于**下一个** AU（ts 变了就分帧），
       所以这里数出来是 2 不是 3 —— 第一版我按 3 写，挂了才发现是自己想错。 */
    UT_EQ_INT(2, count_nals(au, len));

    const h264_depack_stats_t *s = h264_depack_stats(d);
    UT_EQ_INT(1, s->stap_pkt);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 6. STAP-A 长度字段越界：必须老实停住，不许读越界
 *---------------------------------------------------------------------------*/
UT_TEST(depack, stap_a_bad_length_does_not_overrun)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    uint8_t pkt[12];
    pkt[0] = (uint8_t)(0x60 | H264_NAL_STAP_A);
    pkt[1] = 0x00; pkt[2] = 0xFF;         /* 声称 255 字节，实际只剩 9 字节 */
    pkt[3] = 0x67;
    memset(pkt + 4, 0xAA, 8);

    int r = h264_depack_push(d, pkt, (int)sizeof(pkt), 10);
    UT_ASSERT(r != 0);                    /* 整个包被拒 */
    UT_EQ_INT(0, h264_depack_stats(d)->stap_pkt);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 7. 不支持的类型（FU-B / MTAP）要计数，不能静默忽略
 *
 * 静默忽略的后果：对端用 packetization-mode=2 推流时，我们"什么都没收到"
 * 但统计上一片空白，排查会绕很大一圈。
 *---------------------------------------------------------------------------*/
UT_TEST(depack, unsupported_nal_types_are_counted)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    uint8_t pkt[8] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    pkt[0] = (uint8_t)(0x60 | 29);        /* FU-B */
    UT_NE_INT(0, h264_depack_push(d, pkt, 6, 1));
    pkt[0] = (uint8_t)(0x60 | 26);        /* MTAP16 */
    UT_NE_INT(0, h264_depack_push(d, pkt, 6, 1));

    UT_EQ_INT(2, h264_depack_stats(d)->drop_type);
    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 8. 超长 NAL：必须整帧丢弃并回退，绝不能写穿缓冲
 *---------------------------------------------------------------------------*/
UT_TEST(depack, oversize_nal_is_rejected)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    /* 分片累加超过 AU 上限 */
    static uint8_t big[60000];
    memset(big, 0x33, sizeof(big));
    static uint8_t pkt[60100];

    int n = mk_fu_a(pkt, H264_NAL_IDR, true, false, big, (int)sizeof(big));
    h264_depack_push(d, pkt, n, 1);

    int pushed = 5;
    for (int i = 0; i < pushed; i++) {
        n = mk_fu_a(pkt, H264_NAL_IDR, false, false, big, (int)sizeof(big));
        h264_depack_push(d, pkt, n, 1);
    }
    /* 60KB × 5 = 300KB > 256KB，必然触发上限 */
    UT_ASSERT(h264_depack_stats(d)->drop_oversize > 0);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 9. pop 的语义：没有就返回 0，取过一次就不该再返回同一个
 *---------------------------------------------------------------------------*/
UT_TEST(depack, pop_is_idempotent_after_empty)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    const uint8_t *au = NULL; uint32_t ts = 0;
    UT_EQ_INT(0, h264_depack_pop(d, &au, &ts));
    UT_EQ_INT(0, h264_depack_pop(d, &au, &ts));

    uint8_t body[4] = { 1, 2, 3, 4 };
    uint8_t pkt[8];
    int n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    h264_depack_push(d, pkt, n, 1);
    n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    h264_depack_push(d, pkt, n, 2);       /* 触发收尾 */

    UT_ASSERT(h264_depack_pop(d, &au, &ts) > 0);
    UT_EQ_INT(0, h264_depack_pop(d, &au, &ts));   /* 同一帧不能取两次 */

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 10. 上层取太慢：新帧覆盖旧帧，但要计数
 *
 * 实时视频的选择是"丢旧保新" —— 堆积只会让延迟越滚越大，
 * 最后变成"画面比声音慢两秒"这种没法用的状态。
 *---------------------------------------------------------------------------*/
UT_TEST(depack, slow_consumer_drops_old_frame)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    uint8_t body[4] = { 1, 2, 3, 4 };
    uint8_t pkt[8];

    for (uint32_t ts = 1; ts <= 3; ts++) {
        int n = mk_single(pkt, H264_NAL_SLICE, body, 4);
        h264_depack_push(d, pkt, n, ts * 1000);
    }
    /* 一次都没 pop：三次收尾只留下最后一个 AU */
    const h264_depack_stats_t *s = h264_depack_stats(d);
    UT_EQ_INT(1, s->drop_slow);

    h264_depack_free(d);
}

/*---------------------------------------------------------------------------
 * 11. 空指针 / 零长度
 *---------------------------------------------------------------------------*/
UT_TEST(depack, null_and_empty_input_are_safe)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    uint8_t byte = 0x65;
    UT_NE_INT(0, h264_depack_push(NULL, &byte, 1, 0));
    UT_NE_INT(0, h264_depack_push(d, NULL, 1, 0));
    UT_NE_INT(0, h264_depack_push(d, &byte, 0, 0));
    UT_EQ_INT(0, h264_depack_pop(NULL, NULL, NULL));
    UT_EQ_INT(0, h264_depack_pop(d, NULL, NULL));   /* 空队列 + 空输出指针 */

    h264_depack_free(d);
    h264_depack_free(NULL);   /* 必须能安全释放 */
}

/*---------------------------------------------------------------------------
 * 12. reset：清中间状态但统计保留（换一路视频重新开始时用）
 *---------------------------------------------------------------------------*/
UT_TEST(depack, reset_clears_pending_but_keeps_stats)
{
    h264_depack_t *d = h264_depack_new();
    UT_ASSERT(d != NULL);
    if (!d) return;

    uint8_t body[4] = { 1, 2, 3, 4 };
    uint8_t pkt[8];
    int n = mk_single(pkt, H264_NAL_SLICE, body, 4);
    h264_depack_push(d, pkt, n, 1);

    uint32_t before = h264_depack_stats(d)->pkt_in;
    UT_EQ_INT(1, before);

    h264_depack_reset(d);

    /* 残留的半帧不能被当成完整帧交出去 */
    const uint8_t *au; uint32_t ts;
    UT_EQ_INT(0, h264_depack_pop(d, &au, &ts));

    /* 统计是累计值，reset 不动它 —— 否则"通话中途断了一次重连"之后
       就再也看不到断之前丢了多少包了 */
    UT_EQ_INT(1, h264_depack_stats(d)->pkt_in);

    h264_depack_free(d);
}
