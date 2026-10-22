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
    uint16_t    rtp_local;
    uint16_t    rtp_remote;
    char        remote_ip[64];
    uint64_t    t_answer_ms;
} sip_call_t;

static sip_call_t s_call;
static uint32_t s_rtp_rx, s_rtp_tx;

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

/* 字段扫描取音频端口与对端 IP（避免引入 sdp_message 的额外解析层级） */
static void parse_remote_sdp(const char *sdp, uint16_t *port, char *ip, size_t ipsz)
{
    if (!sdp) return;
    const char *m = strstr(sdp, "m=audio ");
    if (m && port) {
        int p = atoi(m + 8);
        if (p > 0) *port = (uint16_t)p;
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
    osip_message_set_expires(msg, "3600");
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

    snprintf(buf, sizeof(buf), "SIP/2.0/UDP %s:%u;branch=z9hG4bK%u;rport",
             s_local_ip, s_cfg.local_port, (unsigned)(s_call.cseq * 104729 + 7));
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

static int send_msg(osip_message_t *msg)
{
    char *dest = NULL;
    size_t len = 0;
    if (osip_message_to_str(msg, &dest, &len) != 0 || !dest) return -1;

    const char *eol = strstr(dest, "\r\n");
    int head_len = eol ? (int)(eol - dest) : (int)len;
    if (head_len > 200) head_len = 200;
    printf("[sip] >> %.*s\n", head_len, dest);

    int rc = send_raw(dest, len, NULL);
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
        set_state(SIP_REGISTERING);
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
        set_state(SIP_REGISTERING);
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
    snprintf(s_call.local_tag, sizeof(s_call.local_tag), "ind%u", (unsigned)(now_ms() & 0xffffff));
    snprintf(s_call.h.call_id, sizeof(s_call.h.call_id), "%08x%08x@%s",
             (unsigned)rand(), (unsigned)rand(), s_local_ip);
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
    int rc = msg ? send_msg(msg) : -1;
    if (msg) osip_message_free(msg);
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
    s_call.t_answer_ms = now_ms();
    set_state(SIP_INCALL);
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
        printf("[sip] cancel outgoing call\n");
        fflush(stdout);
    }
    call_reset();
    s_rtp_rx = s_rtp_tx = 0;
    idle_state();
    return 0;
}

/*---------------------------------------------------------------------------
 * 收包处理
 *---------------------------------------------------------------------------*/
static void handle_response(osip_message_t *msg, struct sockaddr_in *from)
{
    int code = msg->status_code;
    printf("[sip] << %d %s\n", code, msg->reason_phrase ? msg->reason_phrase : "");
    fflush(stdout);

    /* --- 注册 --- */
    if (s_state == SIP_REGISTERING && code == 200) {
        s_registered = true;
        set_state(SIP_ONLINE);
        /* 自测用：注册成功后自动呼出（SIP_AUTOCALL=1002） */
        {
            static int autocall_done = 0;
            const char *ac = getenv("SIP_AUTOCALL");
            if (ac && *ac && !autocall_done) { autocall_done = 1; app_sip_call(ac); }
        }
        return;
    }
    if (s_state == SIP_REGISTERING && (code == 401 || code == 407)) {
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

        /* 对端 SDP：媒体目标以应答为准 */
        osip_body_t *body = NULL;
        if (osip_message_get_body(msg, 0, &body) == 0 && body && body->body)
            parse_remote_sdp(body->body, &s_call.rtp_remote,
                             s_call.remote_ip, sizeof(s_call.remote_ip));

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

        rtp_open();
        s_call.answered = true;
        s_call.t_answer_ms = now_ms();
        set_state(SIP_INCALL);
        return;
    }
    if (code >= 300) {
        printf("[sip] call failed %d\n", code);
        fflush(stdout);
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
                             s_call.remote_ip, sizeof(s_call.remote_ip));
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
        fflush(stdout);
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
        send_reply(&h, 200, "OK", NULL, NULL);
        send_reply(&s_call.h, 487, "Request Terminated", NULL, NULL);
        call_reset();
        idle_state();
        return;
    }
}

/* RTP：收到就原样回送（回声），无板时验证双向媒体 */
static void rtp_on_readable(void)
{
    static unsigned char buf[1600];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = (int)recvfrom(s_rtp, (char *)buf, sizeof(buf), 0,
                          (struct sockaddr *)&from, &fromlen);
    if (n <= 0) return;
    s_rtp_rx++;
    if (n > 12) {          /* 12 字节 RTP 头之后的负载原样回送 */
        sendto(s_rtp, (const char *)buf, n, 0, (struct sockaddr *)&from, fromlen);
        s_rtp_tx++;
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

        struct timeval tv = { 0, 200000 };   /* 200ms，便于退出 */
        if (select((int)maxfd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            /* 自测用：SIP_AUTOHANGUP=<秒> 通话 N 秒后自动挂断 */
            static int hang_s = -1;
            if (hang_s < 0) {
                const char *v = getenv("SIP_AUTOHANGUP");
                hang_s = (v && *v) ? atoi(v) : 0;
            }
            if (hang_s > 0 && s_state == SIP_INCALL && app_sip_call_seconds() >= hang_s)
                app_sip_hangup();
            continue;
        }

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
            printf("[sip] parse failed (%d bytes)\n", n);
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
    if (!server || !*server) {
        printf("[sip] SIP_SERVER not set -> SIP disabled (UI-only mode)\n");
        return false;
    }
    s_cfg.server      = server;
    s_cfg.server_port = (uint16_t)atoi(getenv("SIP_PORT") ? getenv("SIP_PORT") : "5060");
    s_cfg.user        = getenv("SIP_USER") ? getenv("SIP_USER") : "1001";
    s_cfg.pass        = getenv("SIP_PASS") ? getenv("SIP_PASS") : "";
    s_cfg.local_port  = (uint16_t)atoi(getenv("SIP_LOCAL_PORT") ? getenv("SIP_LOCAL_PORT") : "5062");
    srand((unsigned)time(NULL));
    snprintf(s_regid, sizeof(s_regid), "indoor-%u@%s", (unsigned)rand(), s_local_ip);
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
    printf("[sip] listening on udp/%u\n", s_cfg.local_port);
    fflush(stdout);

    send_register();
}

void app_sip_stop(void)
{
    s_running = false;
    if (s_sock != SOCK_INVALID) { sock_close(s_sock); s_sock = SOCK_INVALID; }
    rtp_close();
    if (s_osip) { osip_release(s_osip); s_osip = NULL; }
}
