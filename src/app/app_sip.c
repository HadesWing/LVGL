/**
 * @file app_sip.c
 * @brief SIP 客户端实现（oSIP 负责 SIP 消息编解码，本层负责 UDP 传输与状态机）
 *
 * 已验证：REGISTER 发送 / 响应解析（对本地 UDP 应答器）
 * 待实测：真实服务器认证（401 Digest）、INVITE 接听、RTP 媒体（M3 继续）
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
static bool       s_running;
static volatile sip_state_t s_state = SIP_IDLE;
static char       s_local_ip[64] = "127.0.0.1";
static char       s_call_id[64];
static int        s_cseq = 1;
static char       s_remote_target[128];

/* --- Digest 认证状态（401 挑战 → 带 Authorization 重发 REGISTER） --- */
static char s_realm[128];
static char s_nonce[256];
static char s_qop[32];
static int  s_auth_tried;   /* 防止 401 死循环：带认证重发后再 401 就报错停 */

static void set_state(sip_state_t st)
{
    s_state = st;
    /* 0=idle 1=online 2=incoming（UI 状态栏用） */
    int ui = (st == SIP_ONLINE || st == SIP_REGISTERING) ? 1
           : (st == SIP_INCOMING) ? 2
           : (st == SIP_INCALL) ? 2 : 0;
    app_events_post(EV_SIP_STATE, ui, (int32_t)st, NULL);
}

sip_state_t app_sip_state(void) { return s_state; }

/*---------------------------------------------------------------------------
 * 组包：REGISTER（首注无 Authorization，401 挑战后带认证重发）
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

static void build_register(osip_message_t **out, const char *authorization)
{
    char buf[512];
    osip_message_t *msg = NULL;
    osip_message_init(&msg);

    /* 注意：set_method / set_version 是"接管指针"，传字符串字面量会在
       osip_message_free 时 free(字面量) → 堆损坏崩溃。必须 osip_strdup。 */
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
    osip_message_set_call_id(msg, s_call_id);

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
 * Digest 计算与 401 应答（RFC 2617，qop=auth 与 qop 缺省两种都支持）
 *   HA1 = MD5(user:realm:pass)
 *   HA2 = MD5(REGISTER:uri)                （无报文体）
 *   response = qop ? MD5(HA1:nonce:nc:cnonce:qop:HA2)
 *                  : MD5(HA1:nonce:HA2)
 *---------------------------------------------------------------------------*/
static int build_auth_header(char *out, size_t outsz)
{
    char ha1[33], ha2[33], resp[33];
    char a1buf[256], a2buf[256];
    char cnonce[24];
    const char *nc = "00000001";
    const char *uri_scheme = "sip:";

    snprintf(a1buf, sizeof(a1buf), "%s:%s:%s", s_cfg.user, s_realm, s_cfg.pass);
    md5_hex_str(a1buf, ha1);
    snprintf(a2buf, sizeof(a2buf), "REGISTER:%s%s@%s", uri_scheme, s_cfg.user, s_cfg.server);
    md5_hex_str(a2buf, ha2);

    /* cnonce：地址无关的每次随机值 */
    snprintf(cnonce, sizeof(cnonce), "%08x%08x",
             (unsigned)rand(), (unsigned)time(NULL));

    if (s_qop[0]) {
        snprintf(a2buf, sizeof(a2buf), "%s:%s:%s:%s:%s:%s",
                 ha1, s_nonce, nc, cnonce, s_qop, ha2);
    } else {
        snprintf(a2buf, sizeof(a2buf), "%s:%s:%s", ha1, s_nonce, ha2);
    }
    md5_hex_str(a2buf, resp);

    int n;
    if (s_qop[0]) {
        n = snprintf(out, outsz,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s%s@%s\", "
            "response=\"%s\", algorithm=MD5, cnonce=\"%s\", nc=%s, qop=%s",
            s_cfg.user, s_realm, s_nonce, uri_scheme, s_cfg.user, s_cfg.server,
            resp, cnonce, nc, s_qop);
    } else {
        n = snprintf(out, outsz,
            "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s%s@%s\", "
            "response=\"%s\", algorithm=MD5",
            s_cfg.user, s_realm, s_nonce, uri_scheme, s_cfg.user, s_cfg.server,
            resp);
    }
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

/*---------------------------------------------------------------------------
 * 发送
 *---------------------------------------------------------------------------*/
static int send_msg(osip_message_t *msg)
{
    char *dest = NULL;
    size_t len = 0;
    if (osip_message_to_str(msg, &dest, &len) != 0 || !dest) return -1;

    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons(s_cfg.server_port);
    to.sin_addr.s_addr = inet_addr(s_cfg.server);

    int rc = (int)sendto(s_sock, dest, (int)len, 0, (struct sockaddr *)&to, sizeof(to));
    /* 只打印首行（请求行/状态行），且长度做保护：dest 可能不含 CRLF */
    const char *eol = strstr(dest, "\r\n");
    int head_len = eol ? (int)(eol - dest) : (int)len;
    if (head_len > 200) head_len = 200;
    printf("[sip] >> %.*s\n", head_len, dest);
    if (rc < 0) {
#ifdef _WIN32
        printf("[sip] sendto failed: %d\n", WSAGetLastError());
#else
        printf("[sip] sendto failed: %s\n", strerror(errno));
#endif
    }
    fflush(stdout);
    osip_free(dest);
    return rc < 0 ? -1 : 0;
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
 * 收包处理
 *---------------------------------------------------------------------------*/
static void handle_response(osip_message_t *msg)
{
    printf("[sip] << %d %s\n", msg->status_code,
           msg->reason_phrase ? msg->reason_phrase : "");
    fflush(stdout);

    if (msg->status_code == 200) {
        set_state(SIP_ONLINE);
    } else if (msg->status_code == 401 || msg->status_code == 407) {
        /* RFC 2617 Digest：从 WWW-Authenticate / Proxy-Authenticate 提取
           realm/nonce/qop，计算 MD5 摘要后带 Authorization 重发一次 */
        osip_www_authenticate_t *wa = NULL;
        osip_message_get_www_authenticate(msg, 0, &wa);
        if (!wa) osip_message_get_proxy_authenticate(msg, 0, &wa);
        if (!wa || s_auth_tried) {
            printf("[sip] auth failed%s\n", s_auth_tried ? " (retry already sent)" : " (no challenge header)");
            set_state(SIP_ERROR);
            return;
        }
        s_auth_tried = 1;
        strip_quotes(osip_www_authenticate_get_realm(wa),  s_realm, sizeof(s_realm));
        strip_quotes(osip_www_authenticate_get_nonce(wa),  s_nonce, sizeof(s_nonce));
        strip_quotes(osip_www_authenticate_get_qop_options(wa), s_qop, sizeof(s_qop));
        printf("[sip] 401 challenge: realm=\"%s\" nonce=%.16s... qop=%s\n",
               s_realm, s_nonce, s_qop[0] ? s_qop : "-");
        fflush(stdout);
        send_register_authed();
    }
}

static void handle_request(osip_message_t *msg)
{
    const char *method = msg->sip_method ? msg->sip_method : "";
    printf("[sip] << %s\n", method);
    fflush(stdout);

    if (strcmp(method, "INVITE") == 0) {
        set_state(SIP_INCOMING);
    } else if (strcmp(method, "BYE") == 0) {
        set_state(SIP_IDLE);
    } else if (strcmp(method, "OPTIONS") == 0) {
        printf("[sip] OPTIONS keepalive\n");
    }
}

static void *recv_loop(void *arg)
{
    (void)arg;
    char buf[4096];
    while (s_running) {
        struct sockaddr_in from;
        int fromlen = sizeof(from);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        struct timeval tv = { 0, 200000 };   /* 200ms，便于退出 */

        if (select((int)s_sock + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        int n = (int)recvfrom(s_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;
        buf[n] = '\0';

        osip_message_t *msg = NULL;
        if (osip_message_init(&msg) != 0) continue;
        if (osip_message_parse(msg, buf, (size_t)n) != 0) {
            printf("[sip] parse failed (%d bytes)\n", n); fflush(stdout);
            osip_message_free(msg);
            continue;
        }

        if (msg->status_code > 0) handle_response(msg);
        else                      handle_request(msg);

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
    snprintf(s_call_id, sizeof(s_call_id), "indoor-%u@%s", (unsigned)rand(), s_local_ip);
    printf("[sip] cfg: %s:%u user=%s local=%u\n",
           s_cfg.server, s_cfg.server_port, s_cfg.user, s_cfg.local_port);
    return true;
}

void app_sip_start(void)
{
    /* ① 初始化 oSIP 运行时（内部 parser_init() 建头域哈希表）。
          漏掉这一步：osip_message_parse() 会在 __osip_message_is_known_header()
          里对 NULL 做 strcmp → 访问违例（0xC0000005 / exit 139）。
          这个坑在只"组包不解析"的场景（如只发 REGISTER 的测试）里不会暴露。 */
    if (!s_osip) {
        if (osip_init(&s_osip) != 0) {
            printf("[sip] osip_init failed\n");
            fflush(stdout);
            return;
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
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
    printf("[sip] listening on udp/%u\n", s_cfg.local_port);
    fflush(stdout);

    send_register();
}

void app_sip_stop(void)
{
    s_running = false;
    if (s_sock != SOCK_INVALID) { sock_close(s_sock); s_sock = SOCK_INVALID; }
    if (s_osip) { osip_release(s_osip); s_osip = NULL; }
}

int app_sip_call(const char *target_uri)
{
    if (!target_uri) return -1;
    snprintf(s_remote_target, sizeof(s_remote_target), "%s", target_uri);
    printf("[sip] INVITE -> %s (M3 补齐：本层已收包解析，发送侧待做)\n", target_uri);
    fflush(stdout);
    return 0;
}

int app_sip_answer(void)
{
    printf("[sip] answer: 200 OK + SDP (M3 待做)\n");
    set_state(SIP_INCALL);
    return 0;
}

int app_sip_hangup(void)
{
    printf("[sip] BYE (M3 待做)\n");
    set_state(SIP_IDLE);
    return 0;
}
