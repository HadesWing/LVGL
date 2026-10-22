/**
 * @file sdp.c
 * @brief SDP 拼装与解析（见 sdp.h 说明为什么独立成模块）
 *
 * 解析用的是"按关键字扫描"而不是完整语法树：
 *   对讲里只关心 c= 行、两三个 m= 行、a=rtpmap。用 oSIP 的 sdp_message
 *   要多走一层对象解析，出错信息还更难读；扫描实现的边界行为完全可以被
 *   单元测试覆盖，反而更可控。
 */
#include "sdp.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 在 sdp 里找 "m=video " 之后的视频段结尾：下一个 "\nm=" 或者字符串结尾 */
static const char *media_section_end(const char *mline)
{
    const char *p = mline;
    while (*p) {
        if (*p == '\n') {
            const char *q = p + 1;
            if (q[0] == 'm' && q[1] == '=') return p;
        }
        p++;
    }
    return p;
}

/*---------------------------------------------------------------------------
 * 拼装
 *---------------------------------------------------------------------------*/
int sdp_build(const sdp_local_t *in, char *out, size_t outsz)
{
    if (!in || !out || outsz == 0) return -1;
    if (!in->local_ip || !in->audio_port) return -1;

    static char vpart[384];
    if (in->video_port) {
        unsigned vpt = in->video_pt ? in->video_pt : 96u;
        /* packetization-mode=1 是必须显式声明的：不写的话对端可能按
           mode=0（单 NAL 包）发，那样一个大 I 帧根本发不出去。 */
        const char *fmtp = in->video_fmtp ? in->video_fmtp
                                          : "packetization-mode=1;profile-level-id=42E01F";
        snprintf(vpart, sizeof(vpart),
            "m=video %u RTP/AVP %u\r\n"
            "a=rtpmap:%u H264/90000\r\n"
            "a=fmtp:%u %s\r\n"
            "a=recvonly\r\n",
            (unsigned)in->video_port, vpt, vpt, vpt, fmtp);
    } else {
        /* port=0 + inactive：RFC 3264 规定的"这个媒体类型我不支持"的写法。
           少了这一行，带视频 offer 的软电话会判 SDP 非法然后挂断。 */
        snprintf(vpart, sizeof(vpart),
            "m=video 0 RTP/AVP 0\r\n"
            "a=inactive\r\n");
    }

    int n = snprintf(out, outsz,
        "v=0\r\n"
        "o=- %u 1 IN IP4 %s\r\n"
        "s=indoor-unit\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendrecv\r\n"
        "%s",
        (unsigned)rand(), in->local_ip, in->local_ip,
        (unsigned)in->audio_port, vpart);

    if (n < 0 || (size_t)n >= outsz) return -2;   /* 缓冲不够，别返回半截 SDP */
    return n;
}

/*---------------------------------------------------------------------------
 * 解析
 *---------------------------------------------------------------------------*/
static void parse_audio(const char *sdp, sdp_remote_t *out)
{
    const char *m = strstr(sdp, "m=audio ");
    if (!m) return;

    int p = atoi(m + 8);
    if (p > 0) out->audio_port = (uint16_t)p;

    /* m=audio 4000 RTP/AVP 8  → 从格式列表里挑我们支持的（0 或 8）。
       注意不能直接取"第一个数字"就完事：有的实现写成 "RTP/AVP 8 0 101"，
       我们要挑 8 而不是被后面的 0 覆盖；反过来也一样。 */
    const char *avp = strstr(m, "RTP/AVP ");
    if (avp) {
        const char *end = media_section_end(m);
        const char *q = avp + 8;
        bool got = false;
        while (q < end && *q && *q != '\r' && *q != '\n') {
            while (*q == ' ') q++;
            if (*q < '0' || *q > '9') break;
            int pt = atoi(q);
            if (pt == 0 || pt == 8) { out->audio_pt = (uint8_t)pt; got = true; break; }
            while (*q && *q != ' ' && *q != '\r' && *q != '\n') q++;
        }
        if (!got) out->audio_pt = 8;    /* 一个都不认识就按 PCMA（国内最常见） */
    }
}

static void parse_video(const char *sdp, sdp_remote_t *out)
{
    const char *m = strstr(sdp, "m=video ");
    if (!m) return;

    int p = atoi(m + 8);
    if (p <= 0) return;                 /* port=0 = 对端拒绝视频，保持 0 */
    out->video_port = (uint16_t)p;

    const char *end = media_section_end(m);

    /* pt 以 a=rtpmap 里的 H264/90000 为准 —— 这是唯一权威来源。
       m 行里的格式列表只是候选集合，可能列了好几个（96 97 98），
       直接取第一个会拿到一个对端根本没用来发 H.264 的号。
       每一步都必须限制在**当前这一行**内搜索：strstr 是搜到字符串结尾的，
       不加行边界判断就会越过本行、命中下一行的 "H264/90000"，
       结果把 VP8 的 96 当成 H.264 的 pt（这个 bug 是被 ut_sdp 抓出来的）。 */
    int found = 0;
    const char *rm = m;
    while (rm < end) {
        rm = strstr(rm, "a=rtpmap:");
        if (!rm || rm >= end) break;

        const char *line_end = rm;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') line_end++;

        const char *hit = strstr(rm, "H264/90000");
        if (hit && hit < line_end) {
            int pt = atoi(rm + 9);
            if (pt >= 0 && pt <= 127) { found = pt; break; }
        }
        rm = line_end;
    }
    if (!found) {
        /* 没有 rtpmap（不规范但确实存在）：退回 m 行第一个动态 pt */
        const char *avp = strstr(m, "RTP/AVP ");
        if (avp) {
            int pt = atoi(avp + 8);
            if (pt >= 96 && pt <= 127) found = pt;
        }
    }
    if (found > 0) out->video_pt = (uint8_t)found;
}

int sdp_parse_remote(const char *sdp, sdp_remote_t *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    out->audio_pt = 8;                  /* 默认 PCMA */
    if (!sdp) return 0;

    const char *c = strstr(sdp, "c=IN IP4 ");
    if (c) {
        const char *s = c + 9;
        size_t i = 0;
        while (*s && *s != '\r' && *s != '\n' && *s != '/' && i + 1 < sizeof(out->ip))
            out->ip[i++] = *s++;
        out->ip[i] = '\0';
    }

    parse_audio(sdp, out);
    parse_video(sdp, out);

    return (out->audio_port || out->video_port) ? 1 : 0;
}
