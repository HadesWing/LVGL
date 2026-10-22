/**
 * @file app_sip.c
 * @brief SIP 客户端实现（oSIP 负责 SIP 消息编解码，本层负责 UDP 传输与状态机）
 *
 * 已实现：
 *   REGISTER → 401 → REGISTER(Digest) → 200 OK
 *   INVITE 呼入：100 Trying → 180 Ringing → 200 OK(SDP) → ACK → BYE/200
 *   INVITE 呼出：INVITE(SDP) → 180 → 200 → ACK → BYE
 *   RTP：PCMA 8k，收到包原样回送（回声）——无板时验证双向媒体的最省事办法
 *
 * 未实现（后续/M6）：视频 H.264、SRTP、事务层重传、通话中 401、多路并发
 */
#include "app_sip.h"
#include "app_events.h"
#include "app_audio.h"
#include "g711.h"
#include "sip_md5.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 注意顺序：WinSock 必须在 osip 之前（osip.h 里用到 struct timeval） */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close closesocket
  static HANDLE s_thread;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <sys/select.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
  static pthread_t s_thread;
#endif

#include <osip2/osip.h>
#include <osipparser2/osip_message.h>
#include <osipparser2/osip_parser.h>

/*---------------------------------------------------------------------------
 * 状态
 *---------------------------------------------------------------------------*/
static sip_cfg_t  s_cfg;
static osip_t    *s_osip = NULL;   /* 必须先 osip_init：它会调用 parser_init()
                                      建头域哈希表 hdr_ref_table，否则解析时
                                      pconfig[0].hname 为 NULL → strcmp 崩溃 */
static sock_t     s_sock = SOCK_INVALID;
static sock_t     s_rtp  = SOCK_INVALID;
static bool       s_running;
static bool       s_registered;
static volatile sip_state_t s_state = SIP_IDLE;
static char       s_local_ip[64] = "127.0.0.1";
static char       s_regid[96];     /* 注册用的 Call-ID */
static int        s_cseq = 1;

/* --- Digest 认证状态（401 挑战 → 带 Authorization 重发 REGISTER） --- */
static char s_realm[128];
static char s_nonce[256];
static char s_qop[32];
static int  s_auth_tried;

/* --- 一条请求/响应的"回信所需头域" --- */
typedef struct {
    char call_id[96];
    char from_hdr[256];
    char to_hdr[256];
    char via_hdr[256];
    char cseq_hdr[64];
    struct sockaddr_in peer_addr;   /* UDP 无连接，响应必须发回源地址 */
} sip_hdrs_t;

/* --- 当前通话 --- */
typedef struct {
    bool        active;
    bool        outgoing;
    bool        answered;
    sip_hdrs_t  h;              /* 对端请求的头域（响应时原样回填） */
    char        local_tag[32];
    char        remote_tag[64];
    char        peer[64];       /* 对端显示名/号码（UI 来电显示） */
    char        peer_uri[128];  /* 对端 URI（ACK/BYE 目标） */
    int         cseq;
    char        invite_branch[64]; /* CANCEL 必须复用 INVITE 的 branch */
    int         invite_cseq;
    bool        cancelled;         /* 已发 CANCEL，等 487 回来 ACK 之后再清对话 */
    uint64_t    t_cancel_ms;
    uint16_t    rtp_local;
    uint16_t    rtp_remote;
    uint8_t     payload;        /* 8=PCMA / 0=PCMU（按 SDP 协商结果） */
    char        remote_ip[64];
    uint64_t    t_answer_ms;
    uint64_t    t_start_ms;
} sip_call_t;

static sip_call_t s_call;
static uint32_t s_rtp_rx, s_rtp_tx;

/* --- RTP 发送侧状态 --- */
static uint16_t s_rtp_seq;
static uint32_t s_rtp_ts;
static uint32_t s_rtp_ssrc;
static uint64_t s_next_frame_ms;  /* 下一帧的发送时刻（按墙上时钟补帧，防欠载） */
static bool     s_rtp_echo;      /* 无声卡时（CI）用回声验证媒体通路 */

static void rtp_send_frame(int tone, const int16_t *sine8, int *phase);

/* --- 注册 --- */
static int      s_expires = 3600;
static uint64_t s_last_register_ms;
static bool     s_reg_pending;   /* 有一个 REGISTER 事务在飞（区分注册响应/呼叫响应） */

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

/* 进程号：Call-ID 里必须带上。
   踩过的坑：srand(time(NULL)) 做种子，两台设备（或同机两个实例）在同一秒启动
   → rand() 序列完全相同 → Call-ID 撞车。服务器会当成同一笔对话，第二路
   直接被判成重传，表现为"打不进去/对话被顶掉一个"。 */
static unsigned proc_id(void)
{
#ifdef _WIN32
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

static void set_state(sip_state_t st)
{
    s_state = st;
    /* UI 事件值：0=idle 1=online 2=来电 3=通话中 4=呼出中 */
    int ui = (st == SIP_ONLINE)      ? 1
           : (st == SIP_INCOMING)    ? 2
           : (st == SIP_INCALL)      ? 3
           : (st == SIP_RINGING)     ? 4
           : (st == SIP_REGISTERING) ? 1 : 0;
    app_events_post(EV_SIP_STATE, ui, (int32_t)st, s_call.peer);
}

sip_state_t app_sip_state(void) { return s_state; }
const char *app_sip_peer_name(void) { return s_call.peer; }

int app_sip_call_seconds(void)
{
    if (s_state != SIP_INCALL || !s_call.t_answer_ms) return 0;
    return (int)((now_ms() - s_call.t_answer_ms) / 1000u);
}

void app_sip_rtp_stats(uint32_t *rx, uint32_t *tx)
{
    if (rx) *rx = s_rtp_rx;
    if (tx) *tx = s_rtp_tx;
}

/*---------------------------------------------------------------------------
 * 工具
 *---------------------------------------------------------------------------*/
static void strip_quotes(const char *in, char *out, size_t outsz)
{
    if (!in) { out[0] = '\0'; return; }
    while (*in == '"') in++;
    size_t n = strlen(in);
    while (n > 0 && in[n - 1] == '"') n--;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

/* <sip:1002@1.2.3.4>;tag=x → 1002 */
static void uri_user(const char *uri, char *out, size_t outsz)
{
    const char *p = strstr(uri, "sip:");
    out[0] = '\0';
    if (!p) return;
    p += 4;
    size_t i = 0;
    while (*p && *p != '@' && *p != '>' && *p != ';' && i + 1 < outsz) out[i++] = *p++;
    out[i] = '\0';
}

static void safe_str(char *dst, size_t dsz, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, dsz, "%s", src);
}

/* 探测本机出口 IP：连一下服务器地址，看内核选了哪块网卡（LAN 联调必需） */
static void detect_local_ip(void)
{
    sock_t t = socket(AF_INET, SOCK_DGRAM, 0);
    if (t == SOCK_INVALID) return;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(s_cfg.server_port);
    dst.sin_addr.s_addr = inet_addr(s_cfg.server);
    if (connect(t, (struct sockaddr *)&dst, sizeof(dst)) == 0) {
        struct sockaddr_in me;
        int len = sizeof(me);
        if (getsockname(t, (struct sockaddr *)&me, &len) == 0) {
            const char *ip = inet_ntoa(me.sin_addr);
            if (ip && strcmp(ip, "0.0.0.0") != 0) safe_str(s_local_ip, sizeof(s_local_ip), ip);
        }
    }
    sock_close(t);
}

/*---------------------------------------------------------------------------
 * RTP：从 local_port+1000 起找偶数口（奇数口留给 RTCP，本实现不发）
 *---------------------------------------------------------------------------*/
static int rtp_open(void)
{
    if (s_rtp != SOCK_INVALID) return 0;
    for (uint16_t p = (uint16_t)(s_cfg.local_port + 1000);
         p < (uint16_t)(s_cfg.local_port + 1100); p = (uint16_t)(p + 2)) {
        sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == SOCK_INVALID) return -1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(p);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            s_rtp = fd;
            s_call.rtp_local = p;
            printf("[sip] RTP listening on udp/%u\n", p);
            fflush(stdout);
            return 0;
        }
        sock_close(fd);
    }
    printf("[sip] RTP bind failed (%u..%u)\n", s_cfg.local_port + 1000, s_cfg.local_port + 1100);
    return -1;
}

static void rtp_close(void)
{
    if (s_rtp != SOCK_INVALID) { sock_close(s_rtp); s_rtp = SOCK_INVALID; }
}

/*---------------------------------------------------------------------------
 * 媒体开关
 *   起：初始化 RTP 序号/时间戳，尝试打开声卡；打不开就进"回声模式"
 *       （无麦克风的 CI 环境里，靠原样回送证明双向通路是通的）
 *   停：关声卡，清计数
 *---------------------------------------------------------------------------*/
static void rtp_start_media(void)
{
    rtp_open();
    s_rtp_seq  = (uint16_t)(rand() & 0x7fff);
    s_rtp_ts   = (uint32_t)rand();
    s_rtp_ssrc = (uint32_t)rand();
    s_rtp_rx = s_rtp_tx = 0;

    /* SIP_RTP_ECHO=1 时强制回声（CI/无声卡环境验证媒体通路用），否则尝试开声卡 */
    if (getenv("SIP_RTP_ECHO")) {
        s_rtp_echo = true;
    } else if (getenv("SIP_NO_AUDIO")) {
        s_rtp_echo = false;      /* 纯信令回归：完全不碰声卡；发不发由 SIP_TONE 决定 */
    } else {
        s_rtp_echo = !app_audio_open();
    }
    printf("[rtp] media start: %s:%u pt=%u mode=%s\n",
           s_call.remote_ip, s_call.rtp_remote, s_call.payload,
           s_rtp_echo ? "echo(无声卡)" : "audio");
    fflush(stdout);
}

static void rtp_stop_media(void)
{
    app_audio_close();
    s_rtp_echo = false;
}

/*---------------------------------------------------------------------------
 * SDP：只谈一路 PCMA/8000（与海思/大华门口机一致）
 *---------------------------------------------------------------------------*/
static int build_sdp(char *out, size_t sz)
{
    return snprintf(out, sz,
        "v=0\r\n"
        "o=- %u 1 IN IP4 %s\r\n"
        "s=indoor-unit\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n"
        "m=audio %u RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendrecv\r\n",
        (unsigned)rand(), s_local_ip, s_local_ip, s_call.rtp_local);
}

/* 字段扫描取音频端口/IP/payload（避免引入 sdp_message 的额外解析层级） */
static void parse_remote_sdp(const char *sdp, uint16_t *port, char *ip, size_t ipsz,
                             uint8_t *payload)
{
    if (!sdp) return;
    const char *m = strstr(sdp, "m=audio ");
    if (m && port) {
        int p = atoi(m + 8);
        if (p > 0) *port = (uint16_t)p;
        /* m=audio 4000 RTP/AVP 8  → 取编码号，0=PCMU 8=PCMA */
        const char *avp = strstr(m, "RTP/AVP ");
        if (avp && payload) {
            int pt = atoi(avp + 8);
            if (pt == 0 || pt == 8) *payload = (uint8_t)pt;
        }
    }
    const char *c = strstr(sdp, "c=IN IP4 ");
    if (c && ip) {
        const char *s = c + 9;
        size_t i = 0;
        while (*s && *s != '\r' && *s != '\n' && *s != '/' && i + 1 < ipsz)
            ip[i++] = *s++;
        ip[i] = '\0';
    }
}

/*---------------------------------------------------------------------------
 * 组包：REGISTER
 *---------------------------------------------------------------------------*/
static void build_register(osip_message_t **out, const char *authorization)
{
    char buf[512];
    osip_message_t *msg = NULL;
    osip_message_init(&msg);

    /* set_method/set_version 接管指针所有权：传字面量会在 free 时崩堆 */
    osip_message_set_method(msg, osip_strdup("REGISTER"));
    osip_message_set_version(msg, osip_strdup("SIP/2.0"));

    osip_uri_t *uri = NULL;
    snprintf(buf, sizeof(buf), "sip:%s@%s", s_cfg.user, s_cfg.server);
    osip_uri_init(&uri);
    osip_uri_parse(uri, buf);
    osip_message_set_uri(msg, uri);

    snprintf(buf, sizeof(buf), "SIP/2.0/UDP %s:%u;branch=z9hG4bK%u;rport",
             s_local_ip, s_cfg.local_port, (unsigned)(s_cseq * 7919));
    osip_message_set_via(msg, buf);

    snprintf(buf, sizeof(buf), "<sip:%s@%s>;tag=indoor%u", s_cfg.user, s_cfg.server, 1000u);
    osip_message_set_from(msg, buf);
    snprintf(buf, sizeof(buf), "<sip:%s@%s>", s_cfg.user, s_cfg.server);
    osip_message_set_to(msg, buf);
    osip_message_set_call_id(msg, s_regid);

    snprintf(buf, sizeof(buf), "%d REGISTER", s_cseq++);
    osip_message_set_cseq(msg, buf);

    snprintf(buf, sizeof(buf), "<sip:%s@%s:%u>", s_cfg.user, s_local_ip, s_cfg.local_port);
    osip_message_set_contact(msg, buf);
    osip_message_set_max_forwards(msg, "70");
    snprintf(buf, sizeof(buf), "%d", s_expires);
    osip_message_set_expires(msg, buf);
    osip_message_set_content_length(msg, "0");

    if (authorization && *authorization)
        osip_message_set_authorization(msg, authorization);

    *out = msg;
}

/*---------------------------------------------------------------------------
 * 组包：INVITE（呼出）
 *---------------------------------------------------------------------------*/
static void build_invite(osip_message_t **out)
{
    char buf[512];
    char sdp[512];
    osip_message_t *msg = NULL;
    osip_message_init(&msg);

    osip_message_set_method(msg, osip_strdup("INVITE"));
    osip_message_set_version(msg, osip_strdup("SIP/2.0"));

    osip_uri_t *uri = NULL;
    osip_uri_init(&uri);
    osip_uri_parse(uri, s_call.peer_uri);
    osip_message_set_uri(msg, uri);

    /* CANCEL 必须复用同一个 branch 与同一个 CSeq 号（只换方法名），
       否则服务器认为它取消的是另一笔事务 —— 这是呼出取消失败最常见的原因 */
    s_call.invite_cseq = s_call.cseq;
    snprintf(s_call.invite_branch, sizeof(s_call.invite_branch), "z9hG4bK%u",
             (unsigned)(s_call.cseq * 104729 + 7));
    snprintf(buf, sizeof(buf), "SIP/2.0/UDP %s:%u;branch=%s;rport",
             s_local_ip, s_cfg.local_port, s_call.invite_branch);
    osip_message_set_via(msg, buf);

    snprintf(buf, sizeof(buf), "<sip:%s@%s>;tag=%s", s_cfg.user, s_local_ip, s_call.local_tag);
    osip_message_set_from(msg, buf);
    snprintf(buf, sizeof(buf), "<%s>", s_call.peer_uri);
    osip_message_set_to(msg, buf);
    osip_message_set_call_id(msg, s_call.h.call_id);

    snprintf(buf, sizeof(buf), "%d INVITE", s_call.cseq);
    osip_message_set_cseq(msg, buf);
    snprintf(buf, sizeof(buf), "<sip:%s@%s:%u>", s_cfg.user, s_local_ip, s_cfg.local_port);
    osip_message_set_contact(msg, buf);
    osip_message_set_max_forwards(msg, "70");

    osip_message_set_content_type(msg, "application/sdp");
    build_sdp(sdp, sizeof(sdp));
    osip_message_set_body(msg, sdp, strlen(sdp));
    /* Content-Length 由 osip_message_to_str() 按 body 自动生成，不要手工设 */

    *out = msg;
}

/*---------------------------------------------------------------------------
 * 发送
 *---------------------------------------------------------------------------*/
static int send_raw(const char *data, size_t len, const struct sockaddr_in *to)
{
    if (s_sock == SOCK_INVALID) return -1;
    struct sockaddr_in dst;
    if (!to) {
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(s_cfg.server_port);
        dst.sin_addr.s_addr = inet_addr(s_cfg.server);
        to = &dst;
    }
    int rc = (int)sendto(s_sock, data, (int)len, 0, (const struct sockaddr *)to, sizeof(*to));
    if (rc < 0) {
#ifdef _WIN32
        printf("[sip] sendto failed: %d\n", WSAGetLastError());
#else
        printf("[sip] sendto failed: %s\n", strerror(errno));
#endif
        fflush(stdout);
        return -1;
    }
    return 0;
}

static int send_msg_to(osip_message_t *msg, const struct sockaddr_in *to);

static int send_msg(osip_message_t *msg)
{
    return send_msg_to(msg, NULL);
}

/* 从 sip:user@host:port 里解析出目的地址。
   【为什么需要】INVITE 是"发给 Request-URI 指向的那台设备"，不是"发给服务器"。
   之前 send_msg() 一律发到 s_cfg.server，于是只有"服务器地址恰好等于对端地址"
   时才能打通（走 miniSIPServer 时碰巧成立，一旦 IP 直呼就发错地方）。
   UDP 无连接，发错地址不会有任何报错，只表现为"对端一直不响"。 */
static int uri_addr(const char *uri, struct sockaddr_in *out)
{
    if (!uri || !*uri || !out) return -1;
    const char *at = strrchr(uri, '@');
    const char *host = at ? at + 1 : (strncmp(uri, "sip:", 4) == 0 ? uri + 4 : uri);

    char hbuf[64];
    int port = 5060;
    const char *colon = strrchr(host, ':');
    if (colon) {
        size_t n = (size_t)(colon - host);
        if (n == 0 || n >= sizeof(hbuf)) return -1;
        memcpy(hbuf, host, n);
        hbuf[n] = '\0';
        port = atoi(colon + 1);
    } else {
        safe_str(hbuf, sizeof(hbuf), host);
        char *semi = strchr(hbuf, ';');     /* 去掉 ;transport=udp 这类参数 */
        if (semi) *semi = '\0';
    }
    if (port <= 0) port = 5060;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)port);
    out->sin_addr.s_addr = inet_addr(hbuf);
    return (out->sin_addr.s_addr == INADDR_NONE) ? -1 : 0;
}

static int send_msg_to(osip_message_t *msg, const struct sockaddr_in *to)
{
    char *dest = NULL;
    size_t len = 0;
    if (osip_message_to_str(msg, &dest, &len) != 0 || !dest) return -1;

    const char *eol = strstr(dest, "\r\n");
    int head_len = eol ? (int)(eol - dest) : (int)len;
    if (head_len > 200) head_len = 200;
    printf("[sip] >> %.*s\n", head_len, dest);

    int rc = send_raw(dest, len, to);
    fflush(stdout);
    osip_free(dest);
    return rc;
}

/*---------------------------------------------------------------------------
 * 响应构造：UDP 无状态，回填请求头域；180/200 的 To 要补本机 tag
 *---------------------------------------------------------------------------*/
static void send_reply(const sip_hdrs_t *h, int code, const char *reason,
                       const char *extra, const char *body)
{
    char buf[2048];
    char to[352];
    char contact[128];
    size_t blen = body ? strlen(body) : 0;

    safe_str(to, sizeof(to), h->to_hdr[0] ? h->to_hdr : "<sip:unknown>");
    if ((code == 180 || code == 200) && !strstr(to, "tag=")) {
        size_t n = strlen(to);
        snprintf(to + n, sizeof(to) - n, ";tag=%s", s_call.local_tag);
    }
    snprintf(contact, sizeof(contact), "<sip:%s@%s:%u>", s_cfg.user, s_local_ip, s_cfg.local_port);

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

    printf("[sip] >> %d %s\n", code, reason);
    fflush(stdout);
    send_raw(buf, (size_t)n, &h->peer_addr);
}

/* 从 200 OK 的 Contact 取对端实际地址：ACK/BYE 是端到端，必须发到这里，
   不能老发给代理，否则对端收不到 ACK 会不停重传 200（现象就是刷屏） */
static int contact_addr(osip_message_t *msg, struct sockaddr_in *out, char *uri, size_t urisz)
{
    osip_contact_t *ct = NULL;
    if (!msg || osip_message_get_contact(msg, 0, &ct) != 0 || !ct || !ct->url) return -1;

    if (uri && urisz) {
        char *us = NULL;
        if (osip_uri_to_str(ct->url, &us) == 0 && us) {
            safe_str(uri, urisz, us);
            osip_free(us);
        }
    }

    const char *host = ct->url->host;
    const char *port = ct->url->port;
    if (!host || !*host) return -1;

    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port && *port ? (uint16_t)atoi(port) : 5060);
    out->sin_addr.s_addr = inet_addr(host);
    return (out->sin_addr.s_addr == INADDR_NONE) ? -1 : 0;
}

/* 把一条入站请求的头域抓进 h（响应/后续 BYE 都要用） */
static void capture_request(osip_message_t *msg, const struct sockaddr_in *from, sip_hdrs_t *h)
{
    memset(h, 0, sizeof(*h));
    h->peer_addr = *from;

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
 * Digest 计算（RFC 2617：qop=auth 与 qop 缺省两种）
 *---------------------------------------------------------------------------*/
static int build_auth_header(char *out, size_t outsz)
{
    char ha1[33], ha2[33], resp[33];
    char a1buf[256], a2buf[256];
    char cnonce[24];
    const char *nc = "00000001";

    snprintf(a1buf, sizeof(a1buf), "%s:%s:%s", s_cfg.user, s_realm, s_cfg.pass);
    md5_hex_str(a1buf, ha1);
    snprintf(a2buf, sizeof(a2buf), "REGISTER:sip:%s@%s", s_cfg.user, s_cfg.server);
    md5_hex_str(a2buf, ha2);
    snprintf(cnonce, sizeof(cnonce), "%08x%08x", (unsigned)rand(), (unsigned)time(NULL));

    if (s_qop[0])
        snprintf(a2buf, sizeof(a2buf), "%s:%s:%s:%s:%s:%s", ha1, s_nonce, nc, cnonce, s_qop, ha2);
    else
        snprintf(a2buf, sizeof(a2buf), "%s:%s:%s", ha1, s_nonce, ha2);
    md5_hex_str(a2buf, resp);

    int n;
    if (s_qop[0]) {
        n = snprintf(out, outsz,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"sip:%s@%s\", "
            "response=\"%s\", algorithm=MD5, cnonce=\"%s\", nc=%s, qop=%s",
            s_cfg.user, s_realm, s_nonce, s_cfg.user, s_cfg.server, resp, cnonce, nc, s_qop);
    } else {
        n = snprintf(out, outsz,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"sip:%s@%s\", "
            "response=\"%s\", algorithm=MD5",
            s_cfg.user, s_realm, s_nonce, s_cfg.user, s_cfg.server, resp);
    }
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

static void send_register(void)
{
    osip_message_t *msg = NULL;
    build_register(&msg, NULL);
    if (msg) {
        /* 续期（通话中）不能改状态，否则来电/通话中的界面会被打回"在线" */
        if (s_state == SIP_IDLE || s_state == SIP_ERROR) set_state(SIP_REGISTERING);
        s_reg_pending = true;
        if (send_msg(msg) == 0) printf("[sip] REGISTER sent (cseq=%d)\n", s_cseq - 1);
        osip_message_free(msg);
    }
    fflush(stdout);
}

static void send_register_authed(void)
{
    osip_message_t *msg = NULL;
    char auth[512];
    if (build_auth_header(auth, sizeof(auth)) != 0) {
        printf("[sip] auth header too long\n");
        set_state(SIP_ERROR);
        return;
    }
    build_register(&msg, auth);
    if (msg) {
        if (s_state == SIP_IDLE || s_state == SIP_ERROR) set_state(SIP_REGISTERING);
        s_reg_pending = true;
        if (send_msg(msg) == 0) printf("[sip] REGISTER(auth) sent\n");
        osip_message_free(msg);
    }
    fflush(stdout);
}

/*---------------------------------------------------------------------------
 * 呼叫控制
 *---------------------------------------------------------------------------*/
static void call_reset(void)
{
    s_call.active = false;
    s_call.answered = false;
    s_call.outgoing = false;
    s_call.cancelled = false;
    s_call.t_answer_ms = 0;
}

static void idle_state(void)
{
    set_state(s_registered ? SIP_ONLINE : SIP_IDLE);
}

int app_sip_call(const char *target)
{
    if (!target || !*target) return -1;
    if (s_state == SIP_INCALL || s_state == SIP_RINGING || s_state == SIP_INCOMING) return -2;

    call_reset();
    s_call.active = true;
    s_call.outgoing = true;
    s_call.cseq = 1;
    s_call.payload = 8;                  /* 默认 PCMA，最终以对端应答的 SDP 为准 */
    s_call.rtp_remote = 0;
    s_call.remote_ip[0] = '\0';
    s_call.t_start_ms = now_ms();
    snprintf(s_call.local_tag, sizeof(s_call.local_tag), "ind%u", (unsigned)(now_ms() & 0xffffff));
    /* Call-ID 必须全局唯一：随机数 + 进程号 + 通话序号，三者缺一都可能撞车 */
    static unsigned s_call_seq;
    snprintf(s_call.h.call_id, sizeof(s_call.h.call_id), "%08x%08x-%u-%u@%s",
             (unsigned)rand(), (unsigned)rand(), proc_id(), ++s_call_seq, s_local_ip);
    safe_str(s_call.peer, sizeof(s_call.peer), target);

    if (strchr(target, '@')) {
        if (strncmp(target, "sip:", 4) == 0) safe_str(s_call.peer_uri, sizeof(s_call.peer_uri), target);
        else snprintf(s_call.peer_uri, sizeof(s_call.peer_uri), "sip:%s", target);
    } else {
        snprintf(s_call.peer_uri, sizeof(s_call.peer_uri), "sip:%s@%s:%u",
                 target, s_cfg.server, s_cfg.server_port);
    }

    set_state(SIP_RINGING);

    osip_message_t *msg = NULL;
    build_invite(&msg);
    int rc = -1;
    if (msg) {
        /* 发给 Request-URI 指向的那台对端；解析不出 IP（用了域名）才退回服务器 */
        struct sockaddr_in dst;
        rc = (uri_addr(s_call.peer_uri, &dst) == 0) ? send_msg_to(msg, &dst) : send_msg(msg);
        osip_message_free(msg);
    }
    printf("[sip] INVITE -> %s\n", s_call.peer_uri);
    fflush(stdout);
    return rc;
}

int app_sip_answer(void)
{
    if (s_state != SIP_INCOMING) return -1;
    char sdp[512];
    rtp_open();
    build_sdp(sdp, sizeof(sdp));
    send_reply(&s_call.h, 200, "OK", NULL, sdp);
    s_call.answered = true;
    set_state(SIP_INCALL);
    rtp_start_media();               /* 200 OK 一发出，媒体就该通了 */
    s_call.t_answer_ms = now_ms();   /* 同上：音频初始化不计入通话时长 */
    return 0;
}

int app_sip_hangup(void)
{
    if (s_state == SIP_INCOMING) {
        send_reply(&s_call.h, 486, "Busy Here", NULL, NULL);
    } else if (s_call.answered) {
        char buf[1024];
        char to[352];
        safe_str(to, sizeof(to), s_call.h.to_hdr[0] ? s_call.h.to_hdr : s_call.peer_uri);
        s_call.cseq++;
        snprintf(buf, sizeof(buf),
            "BYE %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK%u;rport\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: %s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d BYE\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Length: 0\r\n\r\n",
            s_call.peer_uri, s_local_ip, s_cfg.local_port, (unsigned)(s_call.cseq * 7919 + 3),
            s_cfg.user, s_local_ip, s_call.local_tag, to, s_call.h.call_id, s_call.cseq);
        printf("[sip] >> BYE\n");
        fflush(stdout);
        send_raw(buf, strlen(buf), &s_call.h.peer_addr);
    } else if (s_state == SIP_RINGING) {
        /* 呼出中挂断 = CANCEL：必须复用 INVITE 的 branch 与 CSeq 号，
           否则服务器认为取消的是另一笔事务，对端会一直响到超时 */
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "CANCEL %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: <%s>\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d CANCEL\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Length: 0\r\n\r\n",
            s_call.peer_uri, s_local_ip, s_cfg.local_port, s_call.invite_branch,
            s_cfg.user, s_local_ip, s_call.local_tag, s_call.peer_uri,
            s_call.h.call_id, s_call.invite_cseq);
        printf("[sip] >> CANCEL (cseq=%d branch=%s)\n", s_call.invite_cseq, s_call.invite_branch);
        fflush(stdout);
        /* CANCEL 也必须发给 INVITE 去的同一个地址，否则对端根本不知道你在取消 */
        struct sockaddr_in dst;
        send_raw(buf, strlen(buf), (uri_addr(s_call.peer_uri, &dst) == 0) ? &dst : NULL);

        /* 关键：**不能立刻 call_reset**。487 回来后还要按 RFC 回一个 ACK，
           那时需要 branch / CSeq / Call-ID。所以只把"已取消"标记挂上，
           等 487 处理完再清对话（超时 5s 兜底，见 recv_loop）。 */
        s_call.cancelled = true;
        s_call.t_cancel_ms = now_ms();
        idle_state();
        return 0;
    }
    printf("[rtp] stats rx=%u tx=%u dur=%ds (pps=%.1f)\n", s_rtp_rx, s_rtp_tx,
           app_sip_call_seconds(),
           app_sip_call_seconds() > 0 ? (double)s_rtp_tx / app_sip_call_seconds() : 0.0);
    fflush(stdout);
    rtp_stop_media();
    call_reset();
    s_rtp_rx = s_rtp_tx = 0;
    idle_state();
    return 0;
}

/*---------------------------------------------------------------------------
 * 收包处理
 *---------------------------------------------------------------------------*/
/* 自测用：进入在线状态后自动呼出一次。
   SIP_AUTOCALL=1002（走服务器）或 sip:1002@192.168.1.10:5062（IP 直呼，无需服务器）。
   注册成功与点对点模式两条路都要走它，所以抽成函数。 */
static void try_autocall_once(void)
{
    static int done;
    const char *ac = getenv("SIP_AUTOCALL");
    if (!ac || !*ac || done || s_state != SIP_ONLINE) return;
    done = 1;
    app_sip_call(ac);
}

static void handle_response(osip_message_t *msg, struct sockaddr_in *from)
{
    int code = msg->status_code;
    printf("[sip] << %d %s\n", code, msg->reason_phrase ? msg->reason_phrase : "");
    fflush(stdout);

    /* --- 注册 --- */
    if (s_reg_pending && code == 200) {
        s_reg_pending = false;
        s_registered = true;
        s_last_register_ms = now_ms();
        /* 通话/来电/呼出中收到的是"续期"响应，不能把界面状态打回"在线" */
        if (s_state != SIP_INCALL && s_state != SIP_INCOMING && s_state != SIP_RINGING)
            set_state(SIP_ONLINE);
        printf("[sip] registered (expires=%d)\n", s_expires);
        fflush(stdout);
        /* 自测用：注册成功后自动呼出（SIP_AUTOCALL=1002） */
        try_autocall_once();
        return;
    }
    if (s_reg_pending && (code == 401 || code == 407)) {
        osip_www_authenticate_t *wa = NULL;
        osip_message_get_www_authenticate(msg, 0, &wa);
        if (!wa) osip_message_get_proxy_authenticate(msg, 0, &wa);
        if (!wa || s_auth_tried) {
            printf("[sip] auth failed%s\n", s_auth_tried ? " (retry already sent)" : " (no challenge)");
            set_state(SIP_ERROR);
            return;
        }
        s_auth_tried = 1;
        strip_quotes(osip_www_authenticate_get_realm(wa), s_realm, sizeof(s_realm));
        strip_quotes(osip_www_authenticate_get_nonce(wa), s_nonce, sizeof(s_nonce));
        strip_quotes(osip_www_authenticate_get_qop_options(wa), s_qop, sizeof(s_qop));
        printf("[sip] 401 challenge: realm=\"%s\" nonce=%.16s... qop=%s\n",
               s_realm, s_nonce, s_qop[0] ? s_qop : "-");
        fflush(stdout);
        send_register_authed();
        return;
    }

    /* --- 呼出 --- */
    if (!s_call.outgoing) return;
    if (code == 100) return;
    if (code == 180 || code == 183) {
        printf("[sip] peer ringing\n");
        fflush(stdout);
        return;
    }
    if (code == 200) {
        if (s_call.answered) return;   /* 200 重传：ACK 已发过，忽略 */

        char buf[1024];
        char to_tag[96] = "remote";
        /* sip->to 是 osip_from_t：tag 存在 gen_params 里，用宏取 */
        osip_generic_param_t *tagp = NULL;
        if (msg->to && osip_to_get_tag(msg->to, &tagp) == 0 && tagp && tagp->gvalue)
            safe_str(to_tag, sizeof(to_tag), tagp->gvalue);

        /* ACK 目标：优先 Contact（端到端），退回响应源地址 */
        s_call.h.peer_addr = *from;
        {
            char curi[160] = "";
            if (contact_addr(msg, &s_call.h.peer_addr, curi, sizeof(curi)) == 0 && curi[0]) {
                /* 对话的远端目标 = Contact，后续 ACK/BYE 的 Request-URI 都用它 */
                safe_str(s_call.peer_uri, sizeof(s_call.peer_uri), curi);
            }
        }

        /* To 头必须与 200 OK 里的一字不差（tag + URI），否则代理匹配不到对话，
           会一直重传 200 OK —— 这是本轮踩到的真实坑 */
        {
            char *s = NULL;
            if (msg->to && osip_to_to_str(msg->to, &s) == 0 && s) {
                safe_str(s_call.h.to_hdr, sizeof(s_call.h.to_hdr), s);
                osip_free(s);
            }
        }
        char to_line[352];
        if (s_call.h.to_hdr[0])
            safe_str(to_line, sizeof(to_line), s_call.h.to_hdr);
        else
            snprintf(to_line, sizeof(to_line), "<%s>;tag=%s", s_call.peer_uri, to_tag);

        /* 对端 SDP：媒体目标与编码以应答为准（应答方说了算） */
        osip_body_t *body = NULL;
        if (osip_message_get_body(msg, 0, &body) == 0 && body && body->body)
            parse_remote_sdp(body->body, &s_call.rtp_remote,
                             s_call.remote_ip, sizeof(s_call.remote_ip),
                             &s_call.payload);

        /* 注意：2xx 的 ACK 必须用与 INVITE **相同**的 CSeq 号，不能自增，
           否则服务器/UAS 认为 ACK 匹配不上，会一直重传 200 OK（实测踩到） */
        snprintf(buf, sizeof(buf),
            "ACK %s SIP/2.0\r\n"
            "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK%u;rport\r\n"
            "From: <sip:%s@%s>;tag=%s\r\n"
            "To: %s\r\n"
            "Call-ID: %s\r\n"
            "CSeq: %d ACK\r\n"
            "Max-Forwards: 70\r\n"
            "Content-Length: 0\r\n\r\n",
            s_call.peer_uri, s_local_ip, s_cfg.local_port, (unsigned)(s_call.cseq * 7919 + 11),
            s_cfg.user, s_local_ip, s_call.local_tag,
            to_line, s_call.h.call_id, s_call.cseq);
        printf("[sip] >> ACK -> %s:%u\n", inet_ntoa(s_call.h.peer_addr.sin_addr),
               ntohs(s_call.h.peer_addr.sin_port));
        if (getenv("SIP_DEBUG")) printf("[sip] ACK dump:\n%s\n", buf);
        fflush(stdout);
        send_raw(buf, strlen(buf), &s_call.h.peer_addr);

        s_call.answered = true;
        set_state(SIP_INCALL);
        rtp_start_media();               /* 收到 200 OK + ACK，媒体通路开起来 */
        /* 计时起点放在音频初始化之后：Windows WASAPI 首次打开设备可能耗 1~3s，
           若先计时会把"开声卡的耗时"算成通话时长（实测踩到，AUTOHANGUP 秒触发） */
        s_call.t_answer_ms = now_ms();
        return;
    }
    if (code >= 300) {
        printf("[sip] call failed %d\n", code);
        fflush(stdout);

        /* RFC 3261 §17.1.1.3：INVITE 的**非 2xx 终态响应也必须 ACK**，
           否则 UAS/代理会按事务重传定时器一直重发这个响应（实测刷屏）。
           这个 ACK 属于 INVITE 事务本身：同 branch、同 CSeq，Request-URI 用 INVITE 的。 */
        if (s_call.active && s_call.outgoing) {
            char ack[1024], to[352];
            char *ts = NULL;
            if (msg->to && osip_to_to_str(msg->to, &ts) == 0 && ts) {
                safe_str(to, sizeof(to), ts);
                osip_free(ts);
            } else {
                snprintf(to, sizeof(to), "<%s>", s_call.peer_uri);
            }
            snprintf(ack, sizeof(ack),
                "ACK %s SIP/2.0\r\n"
                "Via: SIP/2.0/UDP %s:%u;branch=%s;rport\r\n"
                "From: <sip:%s@%s>;tag=%s\r\n"
                "To: %s\r\n"
                "Call-ID: %s\r\n"
                "CSeq: %d ACK\r\n"
                "Max-Forwards: 70\r\n"
                "Content-Length: 0\r\n\r\n",
                s_call.peer_uri, s_local_ip, s_cfg.local_port, s_call.invite_branch,
                s_cfg.user, s_local_ip, s_call.local_tag, to,
                s_call.h.call_id, s_call.invite_cseq);
            printf("[sip] >> ACK(非2xx响应)\n");
            fflush(stdout);
            send_raw(ack, strlen(ack), from);
        }

        rtp_stop_media();
        call_reset();
        idle_state();
    }
}

static void handle_request(osip_message_t *msg, struct sockaddr_in *from)
{
    const char *method = msg->sip_method ? msg->sip_method : "";
    printf("[sip] << %s\n", method);
    fflush(stdout);

    if (strcmp(method, "INVITE") == 0) {
        sip_hdrs_t h;
        capture_request(msg, from, &h);

        if (s_call.active) {           /* 占线：486 */
            send_reply(&h, 486, "Busy Here", NULL, NULL);
            return;
        }

        call_reset();
        s_call.active = true;
        s_call.outgoing = false;
        s_call.h = h;
        snprintf(s_call.local_tag, sizeof(s_call.local_tag), "ind%u",
                 (unsigned)(now_ms() & 0xffffff));

        /* 来电显示：优先 From 的 displayname，没有就用号码 */
        s_call.peer[0] = '\0';
        if (msg->from && msg->from->displayname && *msg->from->displayname)
            safe_str(s_call.peer, sizeof(s_call.peer), msg->from->displayname);
        else if (msg->from && msg->from->url && msg->from->url->username)
            safe_str(s_call.peer, sizeof(s_call.peer), msg->from->url->username);
        else
            safe_str(s_call.peer, sizeof(s_call.peer), "unknown");

        osip_body_t *body = NULL;
        if (osip_message_get_body(msg, 0, &body) == 0 && body && body->body)
            parse_remote_sdp(body->body, &s_call.rtp_remote,
                             s_call.remote_ip, sizeof(s_call.remote_ip),
                             &s_call.payload);
        if (!s_call.remote_ip[0])
            safe_str(s_call.remote_ip, sizeof(s_call.remote_ip), inet_ntoa(from->sin_addr));

        rtp_open();
        send_reply(&s_call.h, 100, "Trying", NULL, NULL);
        send_reply(&s_call.h, 180, "Ringing", NULL, NULL);
        set_state(SIP_INCOMING);
        printf("[sip] incoming from %s (%s:%u)\n", s_call.peer, s_call.remote_ip, s_call.rtp_remote);
        fflush(stdout);

        /* 无界面自测/CI：SIP_AUTOANSWER=1 自动接听（真实设备靠 UI 按钮） */
        if (getenv("SIP_AUTOANSWER")) app_sip_answer();
        return;
    }

    if (strcmp(method, "BYE") == 0) {
        sip_hdrs_t h;
        capture_request(msg, from, &h);
        send_reply(&h, 200, "OK", NULL, NULL);
        printf("[sip] call ended by peer\n");
        printf("[rtp] stats rx=%u tx=%u dur=%ds\n", s_rtp_rx, s_rtp_tx, app_sip_call_seconds());
        fflush(stdout);
        rtp_stop_media();
        call_reset();
        s_rtp_rx = s_rtp_tx = 0;
        idle_state();
        return;
    }

    if (strcmp(method, "ACK") == 0) return;

    if (strcmp(method, "OPTIONS") == 0) {   /* 服务器/软交换保活 */
        sip_hdrs_t h;
        capture_request(msg, from, &h);
        send_reply(&h, 200, "OK", "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS\r\n", NULL);
        return;
    }

    if (strcmp(method, "CANCEL") == 0) {
        sip_hdrs_t h;
        capture_request(msg, from, &h);
        send_reply(&h, 200, "OK", NULL, NULL);          /* 先确认 CANCEL 本身 */
        send_reply(&s_call.h, 487, "Request Terminated", NULL, NULL); /* 再终结 INVITE */
        printf("[sip] incoming call cancelled by peer\n");
        fflush(stdout);
        rtp_stop_media();
        call_reset();
        idle_state();
        return;
    }
}

/*---------------------------------------------------------------------------
 * RTP 媒体收：解 G.711 → 播放环
 *   如果声卡打不开（CI/无声卡），退回"收到就原样回送"，用回声证明双向通路通
 *---------------------------------------------------------------------------*/
static void rtp_on_readable(void)
{
    static uint8_t buf[1600];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = (int)recvfrom(s_rtp, (char *)buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
    if (n <= 12) return;
    s_rtp_rx++;

    if (s_rtp_echo) {
        sendto(s_rtp, (const char *)buf, n, 0, (struct sockaddr *)&from, fromlen);
        s_rtp_tx++;
        return;
    }

    const uint8_t *payload = buf + 12;
    int plen = n - 12;
    /* RTP 扩展头（X 位）极少出现在语音里，但出现了必须跳过，
       否则把扩展数据当语音解码 → 刺啦声 */
    if (buf[0] & 0x10) {
        if (plen < 4) return;
        int ext_words = (payload[2] << 8) | payload[3];
        int skip = 4 + ext_words * 4;
        if (skip >= plen) return;
        payload += skip;
        plen -= skip;
    }
    if (plen > G711_FRAME_SAMPLES * 2) plen = G711_FRAME_SAMPLES * 2;

    static int16_t pcm[G711_FRAME_SAMPLES * 2];
    if (s_call.payload == 0) g711_decode_ulaw(payload, pcm, plen);
    else                     g711_decode_alaw(payload, pcm, plen);
    app_audio_write(pcm, plen);
}

/*---------------------------------------------------------------------------
 * RTP 媒体发：每 20ms 一帧 160 采样（G.711 固定 64kbps）
 *   节奏由 recv_loop 的 select 超时驱动（呼叫中 20ms 一次），
 *   没有麦克风数据时补静音 —— 保持稳定的包间隔，避免对端抖动缓冲"饿死"
 *---------------------------------------------------------------------------*/
static void rtp_tx_tick(void)
{
    /* 自测音源：1kHz 正弦（8kHz 采样下一个周期正好 8 个点，用查表免掉 math.h）。
       用途：没有麦克风的工控机/CI 上验证"编码→RTP→解码"整条链路；
       板子上也可以拿它做对讲自检（喇叭里应听到"嘟——"）。 */
    static const int16_t sine8[8] = { 0, 11585, 16384, 11585, 0, -11585, -16384, -11585 };
    static int sine_phase;
    static int tone = -1;
    if (tone < 0) tone = getenv("SIP_TONE") ? 1 : 0;

    if (s_state != SIP_INCALL || !s_call.answered) { s_next_frame_ms = 0; return; }
    if (s_rtp_echo || s_rtp == SOCK_INVALID || s_call.rtp_remote == 0) { s_next_frame_ms = 0; return; }
    if (!tone && !app_audio_can_record()) { s_next_frame_ms = 0; return; }

    /* 按"墙上时钟"补帧：一次 tick 里把落后的帧补齐（最多 3 帧），
       否则 select 每轮 33ms 就只能发 1 帧 → 32kbps 而不是 64kbps，
       对端播放缓冲会被慢慢抽干（实测 underrun 290 次）。 */
    uint64_t now = now_ms();
    if (s_next_frame_ms == 0) s_next_frame_ms = now;
    if (s_next_frame_ms > now + 500) s_next_frame_ms = now;   /* 时钟跳变保护 */

    int sent = 0;
    while (s_next_frame_ms <= now && sent < 3) {
        rtp_send_frame(tone, sine8, &sine_phase);
        s_next_frame_ms += 20;
        sent++;
    }
    if (sent == 0 && s_next_frame_ms < now && (int64_t)(now - s_next_frame_ms) > 1000)
        s_next_frame_ms = now;   /* 长时间没发（切网/挂起），直接对齐，不要一次补几百帧 */
}

/* 发一帧 20ms，编码按协商的 payload（8=PCMA / 0=PCMU） */
static void rtp_send_frame(int tone, const int16_t *sine8, int *phase)
{
    uint8_t pkt[12 + G711_FRAME_SAMPLES];
    int16_t pcm[G711_FRAME_SAMPLES];

    int got = 0;
    if (tone) {
        for (int i = 0; i < G711_FRAME_SAMPLES; i++) {
            pcm[i] = sine8[*phase & 7];
            (*phase)++;
        }
        got = G711_FRAME_SAMPLES;
    } else {
        got = app_audio_read(pcm, G711_FRAME_SAMPLES);
    }
    if (got < G711_FRAME_SAMPLES)
        memset(pcm + got, 0, (size_t)(G711_FRAME_SAMPLES - got) * sizeof(int16_t));

    pkt[0]  = 0x80;                       /* V=2 */
    pkt[1]  = s_call.payload;             /* 8=PCMA / 0=PCMU，M 位为 0 */
    pkt[2]  = (uint8_t)(s_rtp_seq >> 8);
    pkt[3]  = (uint8_t)(s_rtp_seq & 0xFF);
    pkt[4]  = (uint8_t)(s_rtp_ts >> 24);
    pkt[5]  = (uint8_t)(s_rtp_ts >> 16);
    pkt[6]  = (uint8_t)(s_rtp_ts >> 8);
    pkt[7]  = (uint8_t)(s_rtp_ts);
    pkt[8]  = (uint8_t)(s_rtp_ssrc >> 24);
    pkt[9]  = (uint8_t)(s_rtp_ssrc >> 16);
    pkt[10] = (uint8_t)(s_rtp_ssrc >> 8);
    pkt[11] = (uint8_t)(s_rtp_ssrc);

    if (s_call.payload == 0) g711_encode_ulaw(pcm, pkt + 12, G711_FRAME_SAMPLES);
    else                     g711_encode_alaw(pcm, pkt + 12, G711_FRAME_SAMPLES);

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons(s_call.rtp_remote);
    to.sin_addr.s_addr = inet_addr(s_call.remote_ip[0] ? s_call.remote_ip : "127.0.0.1");

    if (sendto(s_rtp, (const char *)pkt, sizeof(pkt), 0,
               (const struct sockaddr *)&to, sizeof(to)) > 0) {
        s_rtp_tx++;
        s_rtp_seq++;
        s_rtp_ts += G711_FRAME_SAMPLES;
    }
}

static void *recv_loop(void *arg)
{
    (void)arg;
    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        sock_t maxfd = s_sock;
        if (s_rtp != SOCK_INVALID) { FD_SET(s_rtp, &rfds); if (s_rtp > maxfd) maxfd = s_rtp; }

        /* 呼叫中 20ms 一次（RTP 帧节奏），空闲时 100ms 足够 —— 既不空转 CPU，
           又能保证音频包间隔稳定（用 200ms 会在两帧之间攒出抖动） */
        struct timeval tv = (s_state == SIP_INCALL) ? (struct timeval){ 0, 20000 }
                                                    : (struct timeval){ 0, 100000 };
        int sel = select((int)maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) {
            rtp_tx_tick();
            /* CANCEL 发出后 5s 还没等到 487：清对话兜底，避免一直占线 */
            if (s_call.cancelled && (now_ms() - s_call.t_cancel_ms) > 5000) {
                printf("[sip] CANCEL 未收到 487，强制清理\n");
                fflush(stdout);
                call_reset();
            }
            /* 注册续期：Expires 一半时重发 REGISTER（真实服务器会踢掉过期账号） */
            if (s_registered && s_expires > 0 &&
                (now_ms() - s_last_register_ms) > (uint64_t)(s_expires / 2) * 1000u) {
                s_auth_tried = 0;
                send_register();
            }
            /* 自测用：SIP_AUTOHANGUP=<秒> 通话 N 秒后自动挂断 */
            static int hang_s = -1;
            if (hang_s < 0) {
                const char *v = getenv("SIP_AUTOHANGUP");
                hang_s = (v && *v) ? atoi(v) : 0;
            }
            if (hang_s > 0 &&
                (s_state == SIP_INCALL || s_state == SIP_RINGING)) {
                int secs = (s_state == SIP_INCALL)
                         ? app_sip_call_seconds()
                         : (int)((now_ms() - s_call.t_start_ms) / 1000u);
                if (secs >= hang_s) app_sip_hangup();
            }
            continue;
        }

        /* 有包也要看看是不是该发音频了（20ms 节奏不能只靠超时分支） */
        rtp_tx_tick();

        if (s_rtp != SOCK_INVALID && FD_ISSET(s_rtp, &rfds)) rtp_on_readable();
        if (!FD_ISSET(s_sock, &rfds)) continue;

        static char buf[4096];
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int n = (int)recvfrom(s_sock, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        buf[n] = '\0';

        /* 过滤非 SIP 报文：真实网络里会有杂包（扫描、误投），
           直接丢，不要送去 oSIP 解析并刷屏 */
        if (strncmp(buf, "SIP/2.0", 7) != 0 && strstr(buf, " SIP/2.0\r\n") == NULL)
            continue;

        osip_message_t *msg = NULL;
        if (osip_message_init(&msg) != 0) continue;
        if (osip_message_parse(msg, buf, (size_t)n) != 0) {
            /* 打出首行：解析失败十次有九次是对方回了个畸形响应，
               只看"parse failed"根本没法判断是哪个字段坏了 */
            printf("[sip] parse failed (%d bytes): %.48s\n", n, buf);
            fflush(stdout);
            osip_message_free(msg);
            continue;
        }

        if (msg->status_code > 0) handle_response(msg, &from);
        else                      handle_request(msg, &from);

        osip_message_free(msg);
    }
    return NULL;
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
bool app_sip_init_from_env(void)
{
    const char *server = getenv("SIP_SERVER");
    /* 点对点模式（SIP_NO_REGISTER）不需要服务器：门口机与室内机直连、
       或者拿 MicroSIP 直接打 IP 联调，都属于这种场景 */
    bool p2p = getenv("SIP_NO_REGISTER") != NULL;
    if ((!server || !*server) && !p2p) {
        printf("[sip] SIP_SERVER not set -> SIP disabled (UI-only mode)\n");
        return false;
    }
    if (!server || !*server) server = "127.0.0.1";   /* p2p 时仅用于拼默认 URI */
    s_cfg.server      = server;
    s_cfg.server_port = (uint16_t)atoi(getenv("SIP_PORT") ? getenv("SIP_PORT") : "5060");
    s_cfg.user        = getenv("SIP_USER") ? getenv("SIP_USER") : "1001";
    s_cfg.pass        = getenv("SIP_PASS") ? getenv("SIP_PASS") : "";
    s_cfg.local_port  = (uint16_t)atoi(getenv("SIP_LOCAL_PORT") ? getenv("SIP_LOCAL_PORT") : "5062");
    s_expires         = getenv("SIP_EXPIRES") ? atoi(getenv("SIP_EXPIRES")) : 3600;
    if (s_expires < 60) s_expires = 60;      /* 太快会被服务器当泛洪 */
    /* 强迫回声模式（CI 无麦克风时用，验证媒体双向通路而不依赖声卡） */
    s_rtp_echo = getenv("SIP_RTP_ECHO") != NULL;
    srand((unsigned)time(NULL));
    snprintf(s_regid, sizeof(s_regid), "indoor-%08x-%u@%s",
             (unsigned)rand(), proc_id(), s_local_ip);
    printf("[sip] cfg: %s:%u user=%s local=%u\n",
           s_cfg.server, s_cfg.server_port, s_cfg.user, s_cfg.local_port);
    return true;
}

void app_sip_start(void)
{
    /* ① 初始化 oSIP 运行时（内部 parser_init() 建头域哈希表）。
          漏掉这一步：osip_message_parse() 会在 __osip_message_is_known_header()
          里对 NULL 做 strcmp → 访问违例（0xC0000005 / exit 139）。 */
    if (!s_osip && osip_init(&s_osip) != 0) {
        printf("[sip] osip_init failed\n");
        fflush(stdout);
        return;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    detect_local_ip();
    printf("[sip] local ip = %s\n", s_local_ip);

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock == SOCK_INVALID) { printf("[sip] socket failed\n"); return; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(s_cfg.local_port);
    local.sin_addr.s_addr = INADDR_ANY;
    if (bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("[sip] bind %u failed\n", s_cfg.local_port);
        return;
    }

    s_running = true;
#ifdef _WIN32
    s_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)recv_loop, NULL, 0, NULL);
#else
    pthread_create(&s_thread, NULL, recv_loop, NULL);
#endif
    rtp_open();

    /* 预开音频设备。室内机是固定功能设备，"上电就把声卡拿到手"是合理策略：
       否则第一次接听时 SDL/WASAPI 才去枚举设备，桌面实测要 1~5 秒
       （本机没有采集设备时更久），整段延后都压在接听路径上，
       表现为"按了接听，几秒后才听见声音"。板端 ALSA 只要几十毫秒，
       但道理一样 —— 建链路径里不该有阻塞式设备初始化。 */
    if (!getenv("SIP_RTP_ECHO") && !getenv("SIP_NO_AUDIO")) app_audio_open();

    printf("[sip] listening on udp/%u\n", s_cfg.local_port);
    fflush(stdout);

    /* 点对点模式（SIP_NO_REGISTER=1）：不注册直接置在线。
       ① 有些项目根本没有平台/服务器，门口机与室内机直连（IP 直呼）；
       ② 联调时想让 MicroSIP 直接打 IP 当对端，也不需要 registrar。
       注意 s_registered 保持 false —— 否则 recv_loop 会按 Expires 去续注册。 */
    if (getenv("SIP_NO_REGISTER")) {
        printf("[sip] SIP_NO_REGISTER -> 点对点模式，跳过 REGISTER\n");
        set_state(SIP_ONLINE);
        try_autocall_once();
    } else {
        send_register();
    }
}

void app_sip_stop(void)
{
    s_running = false;
    if (s_sock != SOCK_INVALID) { sock_close(s_sock); s_sock = SOCK_INVALID; }
    rtp_close();
    if (s_osip) { osip_release(s_osip); s_osip = NULL; }
}
