/**
 * @file sdp.h
 * @brief SDP 拼装与解析（只做对讲用得到的那部分）
 *
 * 为什么从 app_sip.c 里抽出来：
 *   SDP 是"错了就整通电话打不通"的东西，而且**现场没有任何调试手段** ——
 *   设备装在墙上，你只能看日志。偏偏它的错误形式又很隐蔽：
 *     · m-line 条数与 offer 不一致 → 对端判 SDP 非法，直接挂断
 *     · pt 没回显 offer 的号  → 对端按 96 发、我们按 102 收，包全丢
 *     · 不支持的类型没写 port=0 → 对端以为我们支持，发过来没人收
 *   这些都不是"跑一下就知道"的，必须靠单元测试钉死。
 *   而只要它还挂在 app_sip.c 里（依赖 s_call、s_local_ip 一堆全局状态），
 *   就写不了测试。抽出来之后它是纯函数：字符串进、结构体出。
 *
 * 刻意不做的部分：
 *   · 不做完整的 SDP 语法解析（RFC 4566 那套太宽，引入复杂度换不来收益）
 *   · 不处理多路同类型 m-line（对讲场景不会出现）
 *   · 不处理 SRTP/SDP security（明文 RTP，见文档的加密规划）
 */
#ifndef SDP_H
#define SDP_H

#include <stddef.h>
#include <stdint.h>

/* 对端 SDP 里我们关心的字段 */
typedef struct {
    char     ip[64];        /* c=IN IP4 <ip>；取不到时为空串 */
    uint16_t audio_port;    /* m=audio 的端口，0 表示不支持/未提供 */
    uint16_t video_port;    /* m=video 的端口，0 表示对端拒绝视频 */
    uint8_t  audio_pt;      /* 0=PCMU / 8=PCMA；其它值保持默认 8 */
    uint8_t  video_pt;      /* H.264 的动态负载类型，0 表示没找到 */
} sdp_remote_t;

/* 我们生成 SDP 需要的输入 */
typedef struct {
    const char *local_ip;      /* c= 行里的地址 */
    uint16_t    audio_port;    /* m=audio 端口，必填 */
    uint16_t    video_port;    /* m=video 端口；0 = 明确拒绝视频（port=0 + inactive） */
    uint8_t     video_pt;      /* 视频 pt；0 时默认用 96 */
    const char *video_fmtp;    /* fmtp 附加参数，NULL 用默认的 packetization-mode=1 */
} sdp_local_t;

/**
 * 拼装 SDP。返回写入长度（不含结尾 NUL），负数为参数错误。
 *
 * 输出永远是 m=audio 在前、m=video 在后 —— 和绝大多数门口机/软电话的
 * offer 顺序一致。RFC 3264 只要求 answer 与 offer 的**相对顺序**一致，
 * 而按这个顺序生成，无论我们是 offer 方还是 answer 方都不会错位。
 */
int sdp_build(const sdp_local_t *in, char *out, size_t outsz);

/**
 * 解析对端 SDP。sdp 为 NULL 时清零 out 并返回 0。
 * 返回 1 表示解析到至少一个可用媒体行，0 表示没有任何可用媒体。
 */
int sdp_parse_remote(const char *sdp, sdp_remote_t *out);

#endif /* SDP_H */
