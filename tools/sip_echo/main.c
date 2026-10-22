/**
 * @file main.c
 * @brief sip_echo —— 纯 C 的 SIP 回声应答器（UAS），给室内机做端到端自测
 *
 * 【为什么需要它】
 *   开发阶段拿 PC 上的 Python 脚本或 MicroSIP 当对端没问题，但产品是嵌入式设备：
 *   板子上没有 Python，也不一定连得上 PC。这个工具只依赖 **libosip2 + BSD socket**，
 *   交叉编译后可直接跑在 RK3308 上，于是"板子自己测自己"：
 *
 *     终端 A（板子）：./sip_echo                     # 应答器：自动接听 + RTP 回声
 *     终端 B（板子）：./main  SIP_SERVER=127.0.0.1   # 被测的室内机程序
 *
 *   全程不需要 PC、不需要 miniSIPServer、不需要 Python。
 *
 * 【能力边界（够用就好，不追求做完整 UAS）】
 *   REGISTER → 200（无条件放行，不做鉴权）
 *   INVITE   → 100 → 180 → 200(SDP) → ACK → BYE/200
 *   INVITE   → CANCEL → CANCEL 得 200，原 INVITE 得 487
 *   OPTIONS  → 200（保活探测）
 *   RTP      → 收到什么原样回送（PCMA/PCMU 都行，不解析编码）
 *
 * 【编译】
 *   见同目录 CMakeLists.txt；交叉编译时把 OSIP_ROOT 指向 aarch64 版 libosip2。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <time.h>

/* 注意顺序：WinSock 必须在 osip 之前（osip.h 里用到 struct timeval） */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close   closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <errno.h>
  #include <sys/select.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close   close
#endif

#include <osip2/osip.h>
#include <osipparser2/osip_message.h>
#include <osipparser2/osip_parser.h>

/*---------------------------------------------------------------------------
 * 状态
 *---------------------------------------------------------------------------*/
static osip_t  *s_osip = NULL;       /* 必须先 osip_init：它建头域哈希表，否则解析崩溃 */
static sock_t   s_sip = SOCK_INVALID;
static sock_t   s_rtp = SOCK_INVALID;
static uint16_t s_sip_port = 5090;   /* 故意避开 5060：和本机 miniSIPServer 共存 */
static uint16_t s_rtp_port = 5092;
static bool     s_running = true;

static uint32_t s_rtp_rx, s_rtp_tx;
static uint64_t s_t_start, s_t_stat;

/* 一条请求的头域（响应时原样回填） */
typedef struct {
    char call_id[96];
    char from_hdr[256];
    char to_hdr[256];
    char via_hdr[256];
    char cseq_hdr[64];
    char method[16];
    struct sockaddr_in peer;
} hdrs_t;

/* 当前这一通对话（同一时刻只支持一路） */
typedef struct {
    bool     active;
    hdrs_t   h;                 /* INVITE 的头域：487 / 重发 200 要用 */
    char     local_tag[24];
    uint64_t t_invite_ms;
} dialog_t;

static dialog_t s_dlg;
/* 本机媒体地址：刻意放在对话结构之外 —— 对话清空（memset）时它不能跟着丢，
   否则下一条无状态响应里的 Contact 会变成 <sip:echo@:5090>，对端解析直接失败 */
static char s_local_ip[64] = "127.0.0.1";
static bool s_local_ip_set;

/*---------------------------------------------------------------------------
 * 小工具
 *---------------------------------------------------------------------------*/
static uint64_t now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static void safe_str(char *dst, size_t dsz, const char *src)
{
    if (!dst || dsz == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dsz, "%s", src);
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* 取"到对端所走的本机地址"：UDP 无连接，但 connect 之后 getsockname 能问到
   路由选中的源地址。比直接写 127.0.0.1 靠谱 —— SDP 里的 c= 行必须是这个，
   否则对端会把 RTP 打到 127.0.0.1 上（板子上就是自己） */
static void local_ip_toward(const struct sockaddr_in *peer, char *out, size_t outsz)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) { safe_str(out, outsz, "127.0.0.1"); return; }

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    int len = sizeof(me);
    if (connect(s, (const struct sockaddr *)peer, sizeof(*peer)) == 0 &&
        getsockname(s, (struct sockaddr *)&me, &len) == 0) {
        safe_str(out, outsz, inet_ntoa(me.sin_addr));
    } else {
        safe_str(out, outsz, "127.0.0.1");
    }
    sock_close(s);
}

/*---------------------------------------------------------------------------
 * 组 SDP：只谈一路 G.711（与海思/大华门口机一致）
 *---------------------------------------------------------------------------*/
static int build_sdp(char *out, size_t sz, const char *ip, const char *contact_port)
{
    (void)contact_port;
    return snprintf(out, sz,
        "v=0\r\n"
        "o=- %u 1 IN IP4 %s\r\n"
        "s=sip-echo\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendrecv\r\n",
        (unsigned)rand(), ip, ip, s_rtp_port);
}

/*---------------------------------------------------------------------------
 * 从 oSIP 解析结果里抓出回信要用的头域
 *---------------------------------------------------------------------------*/
static void capture(osip_message_t *msg, const struct sockaddr_in *from, hdrs_t *h)
{
    memset(h, 0, sizeof(*h));
    h->peer = *from;
    safe_str(h->method, sizeof(h->method),
             (msg->sip_method && *msg->sip_method) ? msg->sip_method : "");
    safe_str(h->call_id, sizeof(h->call_id),
             (msg->call_id && msg->call_id->number) ? msg->call_id->number : "unknown");

    char *s = NULL;
    if (msg->from && msg->from->url && osip_from_to_str(msg->from, &s) == 0 && s) {
        safe_str(h->from_hdr, sizeof(h->from_hdr), s);
        osip_free(s); s = NULL;
    }
    if (msg->to && msg->to->url && osip_to_to_str(msg->to, &s) == 0 && s) {
        safe_str(h->to_hdr, sizeof(h->to_hdr), s);
        osip_free(s); s = NULL;
    }
    osip_via_t *via = NULL;
    if (osip_message_get_via(msg, 0, &via) == 0 && via &&
        osip_via_to_str(via, &s) == 0 && s) {
        safe_str(h->via_hdr, sizeof(h->via_hdr), s);
        osip_free(s);
    }
    if (msg->cseq) {
        snprintf(h->cseq_hdr, sizeof(h->cseq_hdr), "%s %s",
                 msg->cseq->number ? msg->cseq->number : "1",
                 msg->cseq->method ? msg->cseq->method : "INVITE");
    }
}

/*---------------------------------------------------------------------------
 * 回响应：UDP 无状态，Via/From/To/Call-ID/CSeq 一律原样回填
 *   tag：对话内响应（180/200）要补本端 tag，无状态响应（REGISTER/OPTIONS）传 NULL
 *---------------------------------------------------------------------------*/
static void reply(const hdrs_t *h, int code, const char *reason,
                  const char *extra, const char *body, const char *tag, const char *ip)
{
    char buf[2048];
    char to[352];
    char contact[160];
    size_t blen = body ? strlen(body) : 0;

    safe_str(to, sizeof(to), h->to_hdr[0] ? h->to_hdr : "<sip:unknown>");
    if (tag && *tag && !strstr(to, "tag=")) {
        size_t n = strlen(to);
        snprintf(to + n, sizeof(to) - n, ";tag=%s", tag);
    }
    snprintf(contact, sizeof(contact), "<sip:echo@%s:%u>", ip, s_sip_port);

    int n = snprintf(buf, sizeof(buf),
        "SIP/2.0 %d %s\r\n"
        "Via: %s\r\n"
        "From: %s\r\n"
        "To: %s\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %s\r\n"
        "Contact: %s\r\n"
        "%s"
        "%s"
        "Content-Length: %u\r\n\r\n%s",
        code, reason,
        h->via_hdr, h->from_hdr, to, h->call_id, h->cseq_hdr, contact,
        extra ? extra : "",
        body ? "Content-Type: application/sdp\r\n" : "",
        (unsigned)blen, body ? body : "");

    sendto(s_sip, buf, n, 0, (const struct sockaddr *)&h->peer, sizeof(h->peer));
    logf_("[echo] >> %d %s", code, reason);
}

/*---------------------------------------------------------------------------
 * 请求分发
 *---------------------------------------------------------------------------*/
static void handle_request(osip_message_t *msg, const struct sockaddr_in *from)
{
    hdrs_t h;
    capture(msg, from, &h);
    logf_("[echo] << %s  (%s)", h.method[0] ? h.method : "?", inet_ntoa(from->sin_addr));

    /* 第一次收到请求时问出"到该对端走的本机地址"：Contact 与 SDP 的 c= 行都要用它。
       只在没算过时算一次 —— 每包都建临时 socket 没必要。 */
    if (!s_local_ip_set) {
        local_ip_toward(from, s_local_ip, sizeof(s_local_ip));
        s_local_ip_set = true;
    }

    /* --- REGISTER：无条件 200。应答器不做鉴权，目的是把被测端的注册流程跑通 --- */
    if (strcmp(h.method, "REGISTER") == 0) {
        reply(&h, 200, "OK", "Expires: 3600\r\n", NULL, NULL, s_local_ip);
        return;
    }

    if (strcmp(h.method, "OPTIONS") == 0) {
        reply(&h, 200, "OK", "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS\r\n",
              NULL, NULL, s_local_ip);
        return;
    }

    /* --- INVITE：自动接听 --- */
    if (strcmp(h.method, "INVITE") == 0) {
        /* 同一笔 INVITE 的重传（UDP 丢包）：直接重发 200，不要重建对话 */
        if (s_dlg.active && strcmp(s_dlg.h.call_id, h.call_id) == 0) {
            logf_("[echo] INVITE 重传，重发 200");
        } else if (s_dlg.active) {
            reply(&h, 486, "Busy Here", NULL, NULL, NULL, s_local_ip);
            return;
        } else {
            memset(&s_dlg, 0, sizeof(s_dlg));
            s_dlg.active = true;
            s_dlg.h = h;
            s_dlg.t_invite_ms = now_ms();
            snprintf(s_dlg.local_tag, sizeof(s_dlg.local_tag), "echo%u",
                     (unsigned)(now_ms() & 0xffffff));
            local_ip_toward(from, s_local_ip, sizeof(s_local_ip));
            s_local_ip_set = true;
            s_rtp_rx = s_rtp_tx = 0;
            s_t_start = now_ms();
            logf_("[echo] 来电 → 自动接听；本机媒体地址 %s:%u",
                  s_local_ip, s_rtp_port);
        }

        reply(&s_dlg.h, 100, "Trying", NULL, NULL, NULL, s_local_ip);
        reply(&s_dlg.h, 180, "Ringing", NULL, NULL, s_dlg.local_tag, s_local_ip);

        char sdp[512];
        build_sdp(sdp, sizeof(sdp), s_local_ip, NULL);
        reply(&s_dlg.h, 200, "OK", NULL, sdp, s_dlg.local_tag, s_local_ip);
        return;
    }

    if (strcmp(h.method, "ACK") == 0) return;   /* ACK 永远不回响应 */

    if (strcmp(h.method, "BYE") == 0) {
        reply(&h, 200, "OK", NULL, NULL, NULL, s_local_ip);
        uint32_t secs = (uint32_t)((now_ms() - s_t_start) / 1000u);
        logf_("[echo] 通话结束 rx=%u tx=%u 时长=%us", s_rtp_rx, s_rtp_tx, secs);
        memset(&s_dlg, 0, sizeof(s_dlg));
        return;
    }

    /* --- CANCEL：先确认 CANCEL 本身，再把原 INVITE 以 487 终结 --- */
    if (strcmp(h.method, "CANCEL") == 0) {
        reply(&h, 200, "OK", NULL, NULL, NULL, s_local_ip);
        if (s_dlg.active) {
            reply(&s_dlg.h, 487, "Request Terminated", NULL, NULL,
                  s_dlg.local_tag, s_local_ip);
            logf_("[echo] 来电被取消");
            memset(&s_dlg, 0, sizeof(s_dlg));
        }
        return;
    }

    /* 其余方法一律 405，别装作支持 */
    reply(&h, 405, "Method Not Allowed",
          "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS\r\n", NULL, NULL, s_local_ip);
}

/*---------------------------------------------------------------------------
 * RTP：收到就原样回送
 *   只证明"双向能通、包序连贯"，不解析编码内容 —— 编解码正确性由 G.711 单测覆盖
 *---------------------------------------------------------------------------*/
static void rtp_on_readable(void)
{
    static uint8_t buf[1600];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = (int)recvfrom(s_rtp, (char *)buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
    if (n <= 12) return;                  /* 至少要有 12 字节 RTP 头 */
    s_rtp_rx++;
    sendto(s_rtp, (const char *)buf, n, 0, (struct sockaddr *)&from, fromlen);
    s_rtp_tx++;
}

static int open_udp(uint16_t port)
{
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        logf_("[echo] 绑定端口 %u 失败（端口被占用？）", port);
        sock_close(s);
        return -1;
    }
    return (int)s;
}

int main(int argc, char **argv)
{
    if (argc > 1) s_sip_port = (uint16_t)atoi(argv[1]);
    if (argc > 2) s_rtp_port = (uint16_t)atoi(argv[2]);
    if (getenv("ECHO_SIP_PORT")) s_sip_port = (uint16_t)atoi(getenv("ECHO_SIP_PORT"));
    if (getenv("ECHO_RTP_PORT")) s_rtp_port = (uint16_t)atoi(getenv("ECHO_RTP_PORT"));

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif

    srand((unsigned)time(NULL));
    if (osip_init(&s_osip) != 0) { logf_("[echo] osip_init 失败"); return 1; }

    s_sip = (sock_t)open_udp(s_sip_port);
    s_rtp = (sock_t)open_udp(s_rtp_port);
    if (s_sip == SOCK_INVALID || s_rtp == SOCK_INVALID) return 1;

    safe_str(s_local_ip, sizeof(s_local_ip), "127.0.0.1");
    s_t_stat = now_ms();

    logf_("[echo] 内置回声应答器就绪  SIP :%u  RTP :%u", s_sip_port, s_rtp_port);
    logf_("[echo] 被测端这样接：SIP_SERVER=127.0.0.1 SIP_PORT=%u SIP_USER=echo "
          "SIP_PASS=echo SIP_LOCAL_PORT=5062 SIP_AUTOCALL=echo SIP_TONE=1", s_sip_port);
    logf_("[echo] Ctrl-C 退出");

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_sip, &rfds);
        FD_SET(s_rtp, &rfds);
        sock_t maxfd = (s_sip > s_rtp) ? s_sip : s_rtp;
        struct timeval tv = { 0, 200000 };   /* 200ms：够处理语音，又不空转 */

        int sel = select((int)maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }

        if (sel > 0) {
            if (FD_ISSET(s_rtp, &rfds)) rtp_on_readable();
            if (FD_ISSET(s_sip, &rfds)) {
                static char buf[4096];
                struct sockaddr_in from;
                int fromlen = sizeof(from);
                int n = (int)recvfrom(s_sip, buf, sizeof(buf) - 1, 0,
                                      (struct sockaddr *)&from, &fromlen);
                if (n > 0) {
                    buf[n] = '\0';
                    /* 过滤杂包：真实网络里有扫描流量，别让 oSIP 去解析它并刷屏 */
                    if (strstr(buf, " SIP/2.0\r\n") != NULL) {
                        osip_message_t *msg = NULL;
                        if (osip_message_init(&msg) == 0) {
                            if (osip_message_parse(msg, buf, (size_t)n) == 0) {
                                /* 应答器只处理请求；响应（我们不会收到）直接丢 */
                                if (msg->status_code == 0) handle_request(msg, &from);
                            } else {
                                logf_("[echo] 解析失败（%d 字节）", n);
                            }
                            osip_message_free(msg);
                        }
                    }
                }
            }
        }

        /* 通话中每 5 秒报一次媒体统计：pps 应为 50（20ms 一帧） */
        if (s_dlg.active && (now_ms() - s_t_stat) >= 5000) {
            uint64_t d = now_ms() - s_t_start;
            s_t_stat = now_ms();
            logf_("[echo] rtp rx=%u tx=%u dur=%llus pps=%.1f",
                  s_rtp_rx, s_rtp_tx,
                  (unsigned long long)(d / 1000u),
                  d ? (double)s_rtp_rx * 1000.0 / (double)d : 0.0);
        }
    }

    sock_close(s_sip);
    sock_close(s_rtp);
    osip_release(s_osip);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
