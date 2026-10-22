/**
 * @file ut_sdp.c
 * @brief SDP 拼装/解析用例
 *
 * 这个模块的错误有个共同点：**只有在对端那一侧才能看出来**。
 * 设备自己不会报错，日志里 SDP 也"看着挺正常"，但电话就是打不通或者没画面。
 * 所以用例的重点是把那些"对端会怎么理解"的规则写成断言：
 *
 *   · answer 的 m-line 条数必须与 offer 一致（RFC 3264 §6）
 *   · 不支持的类型要用 port=0 明确拒绝，不能直接不写
 *   · pt 必须回显 offer 里的号（软电话常用 102，我们默认 96）
 *   · video 的 pt 以 a=rtpmap 为准，不是 m 行的第一个数字
 *   · packetization-mode=1 必须显式声明，否则对端可能按单 NAL 打包
 */
#include "ut.h"
#include "sdp.h"

#include <stdio.h>

/* 取 sdp 里第 n 次出现的某关键字所在行（用于断言"有这一行/顺序对不对"） */
static const char *find_line(const char *sdp, const char *key)
{
    return strstr(sdp, key);
}

/*---------------------------------------------------------------------------
 * 1. 音频基本形状
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, builds_audio_mline)
{
    sdp_local_t L = { "192.168.1.10", 4000, 0, 0, NULL };
    char buf[1024];
    int n = sdp_build(&L, buf, sizeof(buf));

    UT_ASSERT(n > 0);
    UT_ASSERT(strstr(buf, "v=0\r\n") == buf);
    UT_ASSERT(find_line(buf, "c=IN IP4 192.168.1.10\r\n") != NULL);
    UT_ASSERT(find_line(buf, "m=audio 4000 RTP/AVP 8\r\n") != NULL);
    UT_ASSERT(find_line(buf, "a=rtpmap:8 PCMA/8000\r\n") != NULL);
}

/*---------------------------------------------------------------------------
 * 2. 没有视频端口时必须写 m=video 0 + inactive
 *
 * 少这一行是"手机点呼叫就挂"的经典原因：Linphone 默认带视频 offer，
 * answer 里 m-line 少一条它判为非法。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, rejects_video_with_port_zero)
{
    sdp_local_t L = { "10.0.0.2", 5062, 0, 0, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    UT_ASSERT_MSG(strstr(buf, "m=video 0 RTP/AVP 0\r\n") != NULL,
                  "拒绝视频必须显式写 m=video 0");
    UT_ASSERT_MSG(strstr(buf, "a=inactive\r\n") != NULL,
                  "拒绝的媒体段要标 inactive");
    UT_ASSERT_MSG(strstr(buf, "a=rtpmap:96") == NULL,
                  "拒绝时不该出现视频 rtpmap");
}

/*---------------------------------------------------------------------------
 * 3. 有视频端口时的形状
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, builds_video_mline)
{
    sdp_local_t L = { "192.168.1.10", 4000, 4002, 96, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    UT_ASSERT(find_line(buf, "m=video 4002 RTP/AVP 96\r\n") != NULL);
    UT_ASSERT(find_line(buf, "a=rtpmap:96 H264/90000\r\n") != NULL);
    UT_ASSERT_MSG(strstr(buf, "packetization-mode=1") != NULL,
                  "不声明 packetization-mode=1，对端可能按单 NAL 打包，大 I 帧发不出去");
    UT_ASSERT_MSG(strstr(buf, "a=recvonly") != NULL,
                  "室内机只看不发，必须标 recvonly");
    UT_ASSERT_MSG(strstr(buf, "42E01F") != NULL,
                  "profile-level-id 要声明，否则对端可能发 high profile 的 CABAC");
}

/*---------------------------------------------------------------------------
 * 4. m-line 顺序：audio 必须在前
 *
 * RFC 3264 要求 answer 的 m-line 相对顺序与 offer 一致。
 * 我们固定 audio→video，和绝大多数门口机/软电话一致。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, audio_mline_comes_before_video)
{
    sdp_local_t L = { "1.2.3.4", 5000, 5002, 96, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    const char *a = strstr(buf, "m=audio ");
    const char *v = strstr(buf, "m=video ");
    UT_ASSERT(a != NULL && v != NULL);
    UT_ASSERT_MSG(a < v, "m=audio 必须在 m=video 之前");
}

/*---------------------------------------------------------------------------
 * 5. 视频 pt 回显：我们作为 answer 方时必须用 offer 给的号
 *
 * 这是"对方能听到声音、看不到画面"的头号原因：
 * 对端按 102 发，我们按 96 认，包全被 pt 校验丢掉。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, echoes_offered_video_pt)
{
    sdp_local_t L = { "1.2.3.4", 5000, 5002, 102, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    UT_ASSERT(find_line(buf, "m=video 5002 RTP/AVP 102\r\n") != NULL);
    UT_ASSERT(find_line(buf, "a=rtpmap:102 H264/90000\r\n") != NULL);
    UT_ASSERT_MSG(strstr(buf, "a=rtpmap:96 ") == NULL,
                  "已经协商成 102 了，不该再出现 96");
}

/*---------------------------------------------------------------------------
 * 6. 缓冲不足必须报错，不能返回半截 SDP
 *
 * 半截 SDP 比"生成失败"危险得多：它可能被当成合法报文发出去，
 * 对端解析出乱七八糟的媒体行。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, tiny_buffer_fails_cleanly)
{
    sdp_local_t L = { "192.168.1.10", 4000, 4002, 96, NULL };
    char small[32];
    UT_ASSERT(sdp_build(&L, small, sizeof(small)) < 0);
}

UT_TEST(sdp, null_arguments_are_rejected)
{
    char buf[256];
    sdp_local_t L = { "1.1.1.1", 5000, 0, 0, NULL };
    UT_ASSERT(sdp_build(NULL, buf, sizeof(buf)) < 0);
    UT_ASSERT(sdp_build(&L, NULL, sizeof(buf)) < 0);

    sdp_local_t no_ip = { NULL, 5000, 0, 0, NULL };
    UT_ASSERT(sdp_build(&no_ip, buf, sizeof(buf)) < 0);

    sdp_local_t no_port = { "1.1.1.1", 0, 0, 0, NULL };
    UT_ASSERT(sdp_build(&no_port, buf, sizeof(buf)) < 0);
}

/*---------------------------------------------------------------------------
 * 7. 解析：自己生成的 SDP 自己必须能解析回去（往返一致性）
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, build_then_parse_roundtrip)
{
    sdp_local_t L = { "192.168.1.10", 4000, 4002, 104, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    sdp_remote_t r;
    UT_EQ_INT(1, sdp_parse_remote(buf, &r));
    UT_EQ_INT(4000, r.audio_port);
    UT_EQ_INT(4002, r.video_port);
    UT_EQ_INT(8, r.audio_pt);
    UT_EQ_INT(104, r.video_pt);
    UT_EQ_STR("192.168.1.10", r.ip);
}

/*---------------------------------------------------------------------------
 * 8. 解析拒绝视频的 SDP：video_port 必须是 0
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, parses_rejected_video_as_zero)
{
    sdp_local_t L = { "192.168.1.10", 4000, 0, 0, NULL };
    char buf[1024];
    UT_ASSERT(sdp_build(&L, buf, sizeof(buf)) > 0);

    sdp_remote_t r;
    sdp_parse_remote(buf, &r);
    UT_EQ_INT(4000, r.audio_port);
    UT_EQ_INT(0, r.video_port);
}

/*---------------------------------------------------------------------------
 * 9. 解析真实软电话的 SDP（Linphone 风格：多 pt、带 rtcp 属性、session 级 c=）
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, parses_linphone_style_offer)
{
    static const char *offer =
        "v=0\r\n"
        "o=1002 1234 5678 IN IP4 192.168.1.50\r\n"
        "s=Talk\r\n"
        "c=IN IP4 192.168.1.50\r\n"
        "t=0 0\r\n"
        "m=audio 7078 RTP/AVP 8 0 101\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=rtpmap:101 telephone-event/8000\r\n"
        "a=sendrecv\r\n"
        "m=video 9078 RTP/AVP 102 96\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=fmtp:102 packetization-mode=1\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=sendrecv\r\n";

    sdp_remote_t r;
    UT_EQ_INT(1, sdp_parse_remote(offer, &r));

    UT_EQ_STR("192.168.1.50", r.ip);
    UT_EQ_INT(7078, r.audio_port);
    UT_EQ_INT(8, r.audio_pt);          /* 列表里有 8 和 0，应当挑到 8 */
    UT_EQ_INT(9078, r.video_port);
    /* 关键：m 行第一个是 102，而 102 正好是 H264 —— 但 96 是 VP8。
       如果实现"取第一个数字"，会撞对了；如果实现"取 rtpmap 里第一个"，
       会拿到 102 也对。真正的区分用例在下一个（H264 不在第一位）。 */
    UT_EQ_INT(102, r.video_pt);
}

/*---------------------------------------------------------------------------
 * 10. H264 不在 m 行格式列表第一位时，pt 必须以 rtpmap 为准
 *
 * 这是"看起来两个 pt 都像"的典型陷阱：
 *   m=video 9078 RTP/AVP 96 102   ← 96 排在前面，但它是 VP8
 *   a=rtpmap:96 VP8/90000
 *   a=rtpmap:102 H264/90000       ← 真正的 H.264 在这里
 * 取第一个数字就错成 96，然后对端发 102 的包全被丢弃。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, video_pt_follows_rtpmap_not_mline_order)
{
    static const char *offer =
        "v=0\r\n"
        "o=- 1 1 IN IP4 10.0.0.9\r\n"
        "c=IN IP4 10.0.0.9\r\n"
        "m=audio 5004 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "m=video 5006 RTP/AVP 96 102\r\n"
        "a=rtpmap:96 VP8/90000\r\n"
        "a=rtpmap:102 H264/90000\r\n";

    sdp_remote_t r;
    sdp_parse_remote(offer, &r);
    UT_EQ_INT(5006, r.video_port);
    UT_ASSERT_MSG(r.video_pt == 102,
                  "H.264 的 pt 要看 a=rtpmap，不能取 m 行第一个数字（那是 VP8）");
    UT_EQ_INT(102, r.video_pt);
}

/*---------------------------------------------------------------------------
 * 11. 只有音频的 SDP（对端根本不带视频行）
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, parses_audio_only_sdp)
{
    static const char *sdp =
        "v=0\r\n"
        "o=- 1 1 IN IP4 10.0.0.9\r\n"
        "c=IN IP4 10.0.0.9\r\n"
        "m=audio 5004 RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n";

    sdp_remote_t r;
    UT_EQ_INT(1, sdp_parse_remote(sdp, &r));
    UT_EQ_INT(5004, r.audio_port);
    UT_EQ_INT(0, r.video_port);
    UT_EQ_INT(8, r.audio_pt);
}

/*---------------------------------------------------------------------------
 * 12. 音频 pt 只认识 0/8，其它值要退回默认 PCMA
 *
 * 有的设备只 offer G.722(9) 或 opus(111)。我们解不了，
 * 退回 8 至少还能按"对端大概率也支持"去回包，而不是发一堆解不出来的东西。
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, unknown_audio_pt_falls_back_to_pcma)
{
    static const char *sdp =
        "v=0\r\n"
        "c=IN IP4 10.0.0.9\r\n"
        "m=audio 5004 RTP/AVP 9 111\r\n"
        "a=rtpmap:9 G722/8000\r\n"
        "a=rtpmap:111 opus/48000/2\r\n";

    sdp_remote_t r;
    sdp_parse_remote(sdp, &r);
    UT_EQ_INT(5004, r.audio_port);
    UT_EQ_INT(8, r.audio_pt);
}

/*---------------------------------------------------------------------------
 * 13. 残缺/空输入不许崩
 *---------------------------------------------------------------------------*/
UT_TEST(sdp, malformed_input_is_safe)
{
    sdp_remote_t r;

    UT_EQ_INT(0, sdp_parse_remote(NULL, &r));
    UT_EQ_INT(0, r.audio_port);

    UT_EQ_INT(0, sdp_parse_remote("", &r));
    UT_EQ_INT(0, sdp_parse_remote("garbage\r\n", &r));
    /* 有 m=audio 但端口是 0（拒绝音频）：不该当成可用媒体 */
    UT_EQ_INT(0, sdp_parse_remote("m=audio 0 RTP/AVP 8\r\n", &r));
    UT_EQ_INT(0, r.audio_port);

    UT_EQ_INT(0, sdp_parse_remote("m=audio 5000 RTP/AVP 8\r\n", NULL));
}
