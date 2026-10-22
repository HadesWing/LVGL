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
#include "app_video.h"
#include "sdp.h"
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
static sock_t     s_rtp_v = SOCK_INVALID;   /* 视频 RTP，独立端口（+2）独立 socket */
static bool       s_running;
static bool       s_registered;
static volatile sip_state_t s_state = SIP_IDLE;
static char       s_local_ip[64] = "127.0.0.1";
static char       s_regid[96];     /* 注册用的 Call-ID */

/* --- 配置缓冲（定义在文件前部：detect_local_ip 等函数先于 init 使用它们） ---
   产品路径 = sip.ini → 静态缓冲；调试路径 = 环境变量覆盖。
   不能直接存 getenv 的指针：ini 的行缓冲会被下一行覆盖。 */
static char     s_cfg_server[80]  = "127.0.0.1";
static char     s_cfg_user[40]    = "1001";
static char     s_cfg_pass[80]    = "";
static char     s_cfg_localip[64] = "";    /* 空 = 自动探测出口网卡 */
static uint16_t s_cfg_sport       = 5060;
static uint16_t s_cfg_lport       = 5062;
static int      s_cfg_expires     = 3600;
static bool     s_cfg_p2p;                 /* 点对点：不注册、无服务器 */
static bool     s_cfg_has_server;          /* 配置里到底有没有给服务器 */
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
    /* --- 视频（室内机目前只收不发） --- */
    uint16_t    rtp_local_v;    /* 本地视频端口 = rtp_local + 2 */
    uint16_t    rtp_remote_v;   /* 对端视频端口（从对端 SDP 的 m=video 取） */
    uint8_t     video_pt;       /* 对端给 H.264 的动态负载类型，常见 96/102 —— 必须回显 */
    bool        video_on;       /* 本次通话视频通道是否真的建立了 */
    char        remote_ip[64];
    uint64_t    t_answer_ms;
    uint64_t    t_start_ms;
} sip_call_t;

static sip_call_t s_call;
static uint32_t s_rtp_rx, s_rtp_tx;

/* --- RTP 接收侧质量统计（只看收发个数会漏掉"丢了 5 个又重复 5 个"） --- */
static uint16_t s_rtp_seq_exp;    /* 期望的下一个序号 */
static uint32_t s_rtp_lost;       /* 序号跳跃累计的丢包数 */
static uint32_t s_rtp_ooo;        /* 乱序/重复/跳变次数 */
static bool     s_rtp_seq_ok;

/* --- RTP 发送侧状态 --- */
static uint16_t s_rtp_seq;
static uint32_t s_rtp_ts;
static uint32_t s_rtp_ssrc;
static uint64_t s_next_frame_ms;  /* 下一帧的发送时刻（按墙上时钟补帧，防欠载） */
static bool     s_rtp_echo;      /* 无声卡时（CI）用回声验证媒体通路 */

/* --- 视频接收计数（和音频分开，便于区分"哪一路坏了"） --- */
static uint32_t s_v_rx;          /* 收到的视频 RTP 包数 */
static uint32_t s_v_pt_bad;      /* pt 不匹配被丢弃的包数 */
static uint32_t s_v_last_ts;

static void rtp_send_frame(void);

/* --- 注册 --- */
static int      s_expires = 3600;
static uint64_t s_last_register_ms;
static bool     s_reg_pending;   /* 有一个 REGISTER 事务在飞（区分注册响应/呼叫响应） */

/* 注册的"可靠性"三件套：事务超时、指数退避、续期抖动。
   没有这三样，网络闪断 30 秒后设备就永久掉线——而界面上还显示"已注册"，
   这是对讲产品最典型的现场投诉。 */
static uint64_t s_reg_sent_ms;        /* 本次 REGISTER 发出时刻 */
static uint64_t s_reg_retry_at_ms;    /* 下次重试时刻，0 = 无待办 */
static int      s_reg_backoff_s = 1;  /* 当前退避秒数：1,2,4,8...60 */
static int      s_reg_refresh_pct = 50; /* 续期触发点（Expires 的百分比，50~80 随机） */
static bool     s_reg_fatal;          /* 账号/密码/域类错误：重试没有意义 */
static int      s_reg_cseq;           /* 本次注册事务的 CSeq（用于把呼出的响应区分开） */

/* 注册与呼叫共用同一个 CSeq 计数器，而响应里只有 CSeq 能说明它属于哪个事务。
   不比对 CSeq 的后果：续期窗口内正好有个呼叫被 486 拒了，会被当成"注册失败"，
   接着把设备状态打到 SIP_REGISTERING —— 界面从"在线"跳成"未注册"。 */
static bool is_reg_response(osip_message_t *msg)
{
    if (!s_reg_pending) return false;
    if (s_reg_cseq <= 0) return true;                       /* 老路径兜底 */
    if (!msg->cseq || !msg->cseq->number) return true;      /* 对端没回 CSeq：按注册处理 */
    return atoi(msg->cseq->number) == s_reg_cseq;
}

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

/* 探测本机出口 IP：连一下目标地址，看内核选了哪块网卡（LAN 联调必需）
 *
 * 探测目标优先级：SIP_LOCAL_IP（人工指定，多网卡板子必用）
 *              > SIP_SERVER（走服务器形态）
 *              > 8.8.8.8:53（点对点直呼形态，只看路由选卡，UDP connect 不发包）
 *
 * 注意：不做这层回退时，SIP_NO_REGISTER=1 且没填 SIP_SERVER 会让 s_local_ip
 * 停在 127.0.0.1，SDP 的 c= 行就成了 127.0.0.1 —— 手机收到后把音频往自己发，
 * 症状是"信令全对、两边都静音"，极难查。 */
static void detect_local_ip(void)
{
    /* 配置里写死优先（板子 eth0+wlan0 双网卡时必填，否则可能挑错出口） */
    if (s_cfg_localip[0]) {
        safe_str(s_local_ip, sizeof(s_local_ip), s_cfg_localip);
        return;
    }

    const char *target = (s_cfg.server[0] && strcmp(s_cfg.server, "0.0.0.0") != 0)
                         ? s_cfg.server : "8.8.8.8";
    uint16_t    tport  = (target == s_cfg.server) ? s_cfg.server_port : 53;

    sock_t t = socket(AF_INET, SOCK_DGRAM, 0);
    if (t == SOCK_INVALID) return;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(tport);
    dst.sin_addr.s_addr = inet_addr(target);
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
 *
 * 视频用**相邻的偶数口**（audio_local + 2）单独开一个 socket。
 * 为什么不共用一个 socket：
 *   1) 视频包大（一个 I 帧分片上百分之一千个包），塞进音频那个 1600 字节
 *      缓冲的接收循环里，会把音频的 20ms 节奏打乱；
 *   2) 端口分开之后，抓包能一眼看出"音频通了视频没通"，排查少绕一圈；
 *   3) 真实门口机/大华 VTH 的 SDP 里 m=audio 和 m=video 本来就是两个端口。
 *---------------------------------------------------------------------------*/
static int rtp_open_video(void);

static int rtp_bind_even(uint16_t from, uint16_t span, uint16_t *got, const char *tag)
{
    for (uint16_t p = from; p < (uint16_t)(from + span); p = (uint16_t)(p + 2)) {
        sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == SOCK_INVALID) return -1;
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(p);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
            *got = p;
            printf("[sip] %s RTP listening on udp/%u\n", tag, p);
            fflush(stdout);
            return (int)fd;
        }
        sock_close(fd);
    }
    printf("[sip] %s RTP bind failed (%u..%u)\n", tag, from, (unsigned)(from + span));
    return -1;
}

static int rtp_open(void)
{
    if (s_rtp != SOCK_INVALID) return 0;

    int fd = rtp_bind_even((uint16_t)(s_cfg.local_port + 1000), 100,
                           &s_call.rtp_local, "audio");
    if (fd < 0) return -1;
    s_rtp = (sock_t)fd;

    rtp_open_video();
    return 0;
}

/* 视频端口单独开：启动时（rtp_open）会先试一次，但那时 app_video 还没 open，
   wants_rx() 为 false —— 真正打开是在 rtp_start_media 里 app_video_open() 之后
   再调一次。合并成一个函数是为了这两处行为一致（端口号、失败降级、日志）。 */
static int rtp_open_video(void)
{
    if (s_rtp_v != SOCK_INVALID) return 0;
    if (!app_video_wants_rx()) return 0;
    if (!s_call.rtp_local) return -1;      /* 音频没绑上，端口号无基准 */

    int fdv = rtp_bind_even((uint16_t)(s_call.rtp_local + 2), 8,
                            &s_call.rtp_local_v, "video");
    if (fdv < 0) {
        /* 打不开不算致命 —— 纯语音通话仍然可用，SDP 里会把 m=video 回成 0 */
        s_call.rtp_local_v = 0;
        return -1;
    }
    s_rtp_v = (sock_t)fdv;
    return 0;
}

static void rtp_close(void)
{
    if (s_rtp != SOCK_INVALID) { sock_close(s_rtp); s_rtp = SOCK_INVALID; }
    if (s_rtp_v != SOCK_INVALID) { sock_close(s_rtp_v); s_rtp_v = SOCK_INVALID; }
    s_call.rtp_local_v = 0;
}

/*---------------------------------------------------------------------------
 * 媒体开关
 *   起：初始化 RTP 序号/时间戳，尝试打开声卡；打不开就进"回声模式"
 *       （无麦克风的 CI 环境里，靠原样回送证明双向通路是通的）
 *   停：关声卡，清计数
 *---------------------------------------------------------------------------*/
static void rtp_start_media(void)
{
    /* 视频资源必须先开：端口要不要 bind 取决于视频路由（sink=drop 就不开）。
       注意 rtp_open() 在启动时已经跑过一次，那时 app_video 还没 open，
       这里的第二次调用才会真正绑上视频端口 —— 所以下面要再调一次。 */
    app_video_open();
    rtp_open();
    rtp_open_video();
    s_rtp_seq  = (uint16_t)(rand() & 0x7fff);
    s_rtp_ts   = (uint32_t)rand();
    s_rtp_ssrc = (uint32_t)rand();
    s_rtp_rx = s_rtp_tx = 0;
    s_rtp_lost = s_rtp_ooo = 0;
    s_rtp_seq_ok = false;

    /* SIP_RTP_ECHO=1 时强制回声（CI/无声卡环境验证媒体通路用），否则尝试开音频。
       SIP_NO_AUDIO=1 = 纯信令，但仍要把路由解析一遍 —— 否则 SIP_TONE=1 会被
       当成"没有音源"，上行一帧都不发（CI 回归就是这么挂的）。 */
    if (getenv("SIP_RTP_ECHO")) {
        s_rtp_echo = true;
    } else if (getenv("SIP_NO_AUDIO")) {
        app_audio_open();        /* 内部据 SIP_NO_AUDIO 走"不碰设备"的路由 */
        s_rtp_echo = false;
    } else {
        s_rtp_echo = !app_audio_open();
    }
    printf("[rtp] media start: %s:%u pt=%u mode=%s\n",
           s_call.remote_ip, s_call.rtp_remote, s_call.payload,
           s_rtp_echo ? "echo(无可用音频)" : app_audio_route_desc());

    /* 视频：只收不发。端口打不开或没编解码器时静默降级为纯语音，
       不阻塞通话 —— 室内机上"能通话"优先于"有画面"。 */
    s_call.video_on = false;
    if (s_call.rtp_local_v && s_call.rtp_remote_v) {
        s_call.video_on = true;
        printf("[rtp] video: local udp/%u ← %s:%u pt=%u  %s\n",
               (unsigned)s_call.rtp_local_v, s_call.remote_ip,
               (unsigned)s_call.rtp_remote_v, (unsigned)s_call.video_pt,
               app_video_route_desc());
    } else {
        printf("[rtp] video: 未建立（local_v=%u remote_v=%u）\n",
               (unsigned)s_call.rtp_local_v, (unsigned)s_call.rtp_remote_v);
    }
    fflush(stdout);
}

/* 媒体统计：把"质量"和"数量"分开报。
   rx=tx=250 也可能丢了 5 个又重复 5 个 —— 只看个数会被骗。 */
static void rtp_print_stats(const char *why)
{
    int sec = app_sip_call_seconds();
    double pps = sec > 0 ? (double)s_rtp_tx / (double)sec : 0.0;
    uint32_t expect = s_rtp_rx + s_rtp_lost;
    double loss = expect ? 100.0 * (double)s_rtp_lost / (double)expect : 0.0;
    printf("[rtp] stats(%s) rx=%u tx=%u lost=%u ooo=%u loss=%.2f%% dur=%ds pps=%.1f\n",
           why, s_rtp_rx, s_rtp_tx, s_rtp_lost, s_rtp_ooo, loss, sec, pps);
    /* 视频单独一行：混在音频里会让人误以为"pps 50 就是全部正常"，
       而实际上视频可能一个包都没进来 */
    if (s_call.video_on || s_v_rx || s_v_pt_bad)
        printf("[rtp] video stats(%s) pkt=%u pt_bad=%u last_ts=%u\n",
               why, s_v_rx, s_v_pt_bad, s_v_last_ts);
    fflush(stdout);
}

static void rtp_stop_media(void)
{
    app_video_stats_print("hangup");
    app_video_close();
    app_audio_close();
    s_rtp_echo = false;
}

/*---------------------------------------------------------------------------
 * SDP：一路 PCMA/8000 + 一路 H.264 视频（只收不发）
 *
 * m=video 这一行过去是写死 port=0 的（明确拒绝）。原因是 RFC 3264 §6 要求
 * answer 的 m-line **条数与顺序都与 offer 一致**，不支持的类型用 port=0 拒绝。
 * 手机上的 Linphone 默认带视频 offer，如果 answer 里只有 m=audio，它判为
 * SDP 无效 → 通话直接失败，表现为"手机点呼叫就挂"。
 *
 * 现在真的收视频了：
 *   - 有可用视频端口（rtp_local_v != 0）→ 回真实端口 + rtpmap + recvonly
 *   - 否则回落到 port=0 的拒绝写法，纯语音通话不受影响
 *
 * 三个容易错的点：
 *   1) **pt 必须回显 offer 里的号**。软电话给 H.264 常用 96，Linphone Android
 *      常给 102；写死 96 会让对端按 96 发、我们按 102 认，包全丢。
 *   2) recvonly 是刻意的：室内机只看不发，省掉编码器和一路 25fps 的 CPU。
 *   3) profile-level-id 声明 42E01F（baseline 3.1）。声明得比实际能力低没关系，
 *      声明高了会让对端发我们解不动的码流（比如 high profile 的 CABAC）。
 *---------------------------------------------------------------------------*/
static int build_sdp(char *out, size_t sz)
{
    sdp_local_t L;
    memset(&L, 0, sizeof(L));
    L.local_ip   = s_local_ip;
    L.audio_port = s_call.rtp_local;
    L.video_port = s_call.rtp_local_v;      /* 0 = 拒绝视频 */
    L.video_pt   = s_call.video_pt;         /* 0 时 sdp.c 用默认 96 */
    return sdp_build(&L, out, sz);
}

/* 字段扫描取音频/视频端口、IP、payload（实现在 sdp.c，有单测） */
static void parse_remote_sdp(const char *sdp, uint16_t *port, char *ip, size_t ipsz,
                             uint8_t *payload, uint16_t *vport, uint8_t *vpt)
{
    if (!sdp) return;

    sdp_remote_t r;
    sdp_parse_remote(sdp, &r);

    if (port && r.audio_port) *port = r.audio_port;
    if (payload)              *payload = r.audio_pt;
    if (vport)                *vport = r.video_port;      /* 0 = 对端拒绝视频 */
    if (vpt && r.video_pt)    *vpt = r.video_pt;
    if (ip && r.ip[0])        safe_str(ip, ipsz, r.ip);
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

/* 注册事务失败 → 进入指数退避（1,2,4,...,60 秒）。
   设计取舍：不无限快重试。SIP 服务器被大批设备同时重连打挂，比单台掉线严重得多。 */
static void reg_fail(const char *why)
{
    s_reg_pending = false;
    s_registered  = false;
    if (s_reg_fatal) return;

    if (s_reg_backoff_s < 1) s_reg_backoff_s = 1;
    s_reg_retry_at_ms = now_ms() + (uint64_t)s_reg_backoff_s * 1000u;
    printf("[sip] 注册未成功（%s），%d 秒后重试\n", why, s_reg_backoff_s);
    fflush(stdout);
    s_reg_backoff_s = (s_reg_backoff_s < 60) ? s_reg_backoff_s * 2 : 60;

    /* 通话中不能改状态：通话优先级高于注册状态显示 */
    if (s_state != SIP_INCALL && s_state != SIP_INCOMING && s_state != SIP_RINGING)
        set_state(SIP_REGISTERING);
}

static void send_register(void)
{
    osip_message_t *msg = NULL;
    build_register(&msg, NULL);
    if (msg) {
        /* 续期（通话中）不能改状态，否则来电/通话中的界面会被打回"在线" */
        if (s_state == SIP_IDLE || s_state == SIP_ERROR) set_state(SIP_REGISTERING);
        s_reg_pending = true;
        s_reg_sent_ms = now_ms();
        s_reg_cseq    = s_cseq - 1;
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
        s_reg_sent_ms = now_ms();
        s_reg_cseq    = s_cseq - 1;
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
    /* 视频通道随通话一起复位。注意 rtp_local_v 不能清 —— 它是"这个进程
       bind 到了哪个端口"，重新呼叫还要用同一个端口继续 bind 着。 */
    s_call.video_on      = false;
    s_call.rtp_remote_v  = 0;
    s_v_rx = s_v_pt_bad = 0;
    s_v_last_ts = 0;
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
    /* offer 里能不能带视频，取决于视频端口此刻有没有绑上。
       所以这两步必须在 build_invite 之前 —— 放到"接通时"就晚了，
       INVITE 已经带着 m=video 0 发出去了。 */
    app_video_open();
    rtp_open();
    rtp_open_video();

    s_call.active = true;
    s_call.outgoing = true;
    s_call.cseq = 1;
    s_call.payload = 8;                  /* 默认 PCMA，最终以对端应答的 SDP 为准 */
    s_call.rtp_remote = 0;
    /* 我们作为 offer 方时指定视频 pt=96（动态负载类型里最常见的一个）。
       对端如果 answer 里换成别的号，parse_remote_sdp 会覆盖它 —— 
       这也是"回显 offer 的 pt"这一步的由来。 */
    s_call.video_pt = 96;
    s_call.rtp_remote_v = 0;
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
    rtp_print_stats("hangup");
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
    if (is_reg_response(msg) && code == 200) {
        s_reg_pending = false;
        s_registered = true;
        s_last_register_ms = now_ms();
        s_reg_backoff_s    = 1;
        s_reg_retry_at_ms  = 0;
        s_reg_fatal        = false;
        /* 续期点随机化到 50%~80%：整批设备同一秒续期，会把服务器打出一个
           周期性的尖峰（现场表现是"每小时整点全网卡一下"） */
        s_reg_refresh_pct  = 50 + (int)(rand() % 31);
        /* 通话/来电/呼出中收到的是"续期"响应，不能把界面状态打回"在线" */
        if (s_state != SIP_INCALL && s_state != SIP_INCOMING && s_state != SIP_RINGING)
            set_state(SIP_ONLINE);
        printf("[sip] registered (expires=%d)\n", s_expires);
        fflush(stdout);
        /* 自测用：注册成功后自动呼出（SIP_AUTOCALL=1002） */
        try_autocall_once();
        return;
    }
    if (is_reg_response(msg) && (code == 401 || code == 407)) {
        osip_www_authenticate_t *wa = NULL;
        osip_message_get_www_authenticate(msg, 0, &wa);
        if (!wa) osip_message_get_proxy_authenticate(msg, 0, &wa);
        if (!wa || s_auth_tried) {
            printf("[sip] auth failed%s —— 密码错就是密码错，重试只会把账号锁掉\n",
                   s_auth_tried ? "（带 Authorization 仍被拒）" : "（没拿到挑战头）");
            fflush(stdout);
            s_reg_pending = false;
            s_reg_fatal   = true;          /* 停下来，等人改配置，不再自旋 */
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

    /* 注册的其它失败分支。分两类：配置问题停手，服务器问题退避重试 */
    if (is_reg_response(msg) && code >= 300) {
        if (code == 400 || code == 403 || code == 404 || code == 405) {
            printf("[sip] 注册被拒（%d）：账号 / 密码 / 域 / 端口对不上，已停止重试\n", code);
            fflush(stdout);
            s_reg_pending = false;
            s_reg_fatal   = true;
            set_state(SIP_ERROR);
            return;
        }
        char why[32];
        snprintf(why, sizeof(why), "%d", code);
        reg_fail(why);
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
                             &s_call.payload,
                             &s_call.rtp_remote_v, &s_call.video_pt);

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
                             &s_call.payload,
                             &s_call.rtp_remote_v, &s_call.video_pt);
        if (!s_call.remote_ip[0])
            safe_str(s_call.remote_ip, sizeof(s_call.remote_ip), inet_ntoa(from->sin_addr));

        /* 应答里的 m=video 也得是真端口，所以视频资源在这里就要备好（不是等接通） */
        app_video_open();
        rtp_open();
        rtp_open_video();
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
        rtp_print_stats("peer bye");
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

    /* 带内按键（RFC 2976 INFO），软电话发 DTMF 常用这个而不是 RFC 2833 */
    if (strcmp(method, "INFO") == 0) {
        sip_hdrs_t h;
        capture_request(msg, from, &h);
        send_reply(&h, 200, "OK", NULL, NULL);
        return;
    }

    /* 兜底：任何没实现的方法都必须回一个终态响应。
       RFC 3261 §8.2.1 要求 UAS 对不支持的方法回 405（或 501），
       不回的实际后果 = 对端按 Timer E/G 反复重传，日志里每秒一条，
       手机端表现为"注册一直失败 / 呼叫发不出去"，而抓包看我们全收到了。
       点对点模式（no_register=1）下，Linphone 建账号时正好会把
       REGISTER 发到板子上，就落进这个分支。 */
    {
        sip_hdrs_t h;
        capture_request(msg, from, &h);
        send_reply(&h, 405, "Method Not Allowed",
                   "Allow: INVITE, ACK, BYE, CANCEL, OPTIONS, INFO\r\n", NULL);
        printf("[sip] %s not supported -> 405\n", method);
        fflush(stdout);
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

    /* 序号连续性：这是"音频有没有问题"最客观的一个数。
       丢包 → 声音断续/咔哒；乱序 → 抖动了但能播（缓冲吸收）；
       重复 → 通常是网络里有环路或对端重传逻辑有问题。
       只看 rx 计数是不够的 —— 发了 250 个、收了 250 个，也可能丢了 5 个又重复 5 个。 */
    {
        uint16_t seq = (uint16_t)(((uint16_t)buf[2] << 8) | buf[3]);
        if (!s_rtp_seq_ok) {
            s_rtp_seq_exp = (uint16_t)(seq + 1);
            s_rtp_seq_ok  = true;
        } else {
            int16_t diff = (int16_t)(seq - s_rtp_seq_exp);   /* 有符号回绕比较 */
            if (diff == 0) {
                s_rtp_seq_exp = (uint16_t)(seq + 1);
            } else if (diff > 0) {
                s_rtp_lost += (uint32_t)diff;                /* 中间少了 diff 个 */
                s_rtp_seq_exp = (uint16_t)(seq + 1);
            } else if (diff > -100) {
                s_rtp_ooo++;                                 /* 迟到/乱序 */
            } else {
                s_rtp_ooo++;                                 /* 剧烈跳变（对端重启了 ts/seq） */
                s_rtp_seq_exp = (uint16_t)(seq + 1);
            }
        }
    }

    /* 对称 RTP（RFC 4961）：以"实际收到包的源地址"为准回发，而不是照 SDP 里写的发。
       手机 Linphone 开了 ICE 时 offer 的 c= 行是 0.0.0.0，NAT 后面端口也会变，
       照 SDP 发等于发给空气 —— 症状是"手机能听到板子？不，是板子听不到手机 / 手机听不到板子"。
       只在地址发生变化时校正一次，日志可见。 */
    {
        char ipbuf[64];
        safe_str(ipbuf, sizeof(ipbuf), inet_ntoa(from.sin_addr));
        uint16_t p = ntohs(from.sin_port);
        if (strcmp(s_call.remote_ip, ipbuf) != 0 || s_call.rtp_remote != p) {
            safe_str(s_call.remote_ip, sizeof(s_call.remote_ip), ipbuf);
            s_call.rtp_remote = p;
            printf("[rtp] peer learned %s:%u (symmetric rtp)\n", ipbuf, p);
            fflush(stdout);
        }
    }

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
 * 视频 RTP 收包
 *
 * 独立 socket、独立计数 —— "音频通了视频没通"这种问题，第一眼要看的就是
 * 视频包有没有进来，而不是去翻解码日志。
 *---------------------------------------------------------------------------*/
static void rtp_video_on_readable(void)
{
    static uint8_t vbuf[2048];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n = (int)recvfrom(s_rtp_v, (char *)vbuf, sizeof(vbuf), 0,
                          (struct sockaddr *)&from, &fromlen);
    if (n <= 12) return;

    /* pt 校验：对端可能把音频也发到视频端口（SDP 写错、端口撞车、
       或者路由器做了奇怪的端口映射）。pt 不匹配就丢 —— 把 G.711 当 H.264
       送去解包，结果只会是"解包器把几十个字节全丢了"，但日志看着像丢包，
       排查方向会跑偏。 */
    uint8_t pt = vbuf[1] & 0x7F;
    if (s_call.video_pt && pt != s_call.video_pt) {
        s_v_pt_bad++;
        return;
    }

    const uint8_t *payload = vbuf + 12;
    int plen = n - 12;
    if (vbuf[0] & 0x10) {
        if (plen < 4) return;
        int ext_words = (payload[2] << 8) | payload[3];
        int skip = 4 + ext_words * 4;
        if (skip >= plen) return;
        payload += skip;
        plen -= skip;
    }
    if (plen <= 0) return;

    uint32_t ts = ((uint32_t)vbuf[4] << 24) | ((uint32_t)vbuf[5] << 16) |
                  ((uint32_t)vbuf[6] << 8)  |  (uint32_t)vbuf[7];

    s_v_rx++;
    s_v_last_ts = ts;
    app_video_on_rtp(payload, plen, ts);
}

/*---------------------------------------------------------------------------
 * RTP 媒体发：每 20ms 一帧 160 采样（G.711 固定 64kbps）
 *   节奏由 recv_loop 的 select 超时驱动（呼叫中 20ms 一次），
 *   没有麦克风数据时补静音 —— 保持稳定的包间隔，避免对端抖动缓冲"饿死"
 *---------------------------------------------------------------------------*/
static void rtp_tx_tick(void)
{
    if (s_state != SIP_INCALL || !s_call.answered) { s_next_frame_ms = 0; return; }
    if (s_rtp_echo || s_rtp == SOCK_INVALID || s_call.rtp_remote == 0) { s_next_frame_ms = 0; return; }
    /* 音源取不到数（没麦克风、也没配文件源/tone、或 src=none）= 只收不发。
       这不是"故障"，是自测里"关掉麦克风只验接收"的正当姿态。 */
    if (!app_audio_can_record()) { s_next_frame_ms = 0; return; }

    /* 按"墙上时钟"补帧：一次 tick 里把落后的帧补齐（最多 3 帧），
       否则 select 每轮 33ms 就只能发 1 帧 → 32kbps 而不是 64kbps，
       对端播放缓冲会被慢慢抽干（实测 underrun 290 次）。 */
    uint64_t now = now_ms();
    if (s_next_frame_ms == 0) s_next_frame_ms = now;
    if (s_next_frame_ms > now + 500) s_next_frame_ms = now;   /* 时钟跳变保护 */

    int sent = 0;
    while (s_next_frame_ms <= now && sent < 3) {
        rtp_send_frame();
        s_next_frame_ms += 20;
        sent++;
    }
    if (sent == 0 && s_next_frame_ms < now && (int64_t)(now - s_next_frame_ms) > 1000)
        s_next_frame_ms = now;   /* 长时间没发（切网/挂起），直接对齐，不要一次补几百帧 */
}

/* 发一帧 20ms，编码按协商的 payload（8=PCMA / 0=PCMU）
   音源由 app_audio 决定：mic / file / tone 都走这里，上层不再关心。 */
static void rtp_send_frame(void)
{
    uint8_t pkt[12 + G711_FRAME_SAMPLES];
    int16_t pcm[G711_FRAME_SAMPLES];

    int got = app_audio_read(pcm, G711_FRAME_SAMPLES);
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
        if (s_rtp_v != SOCK_INVALID) { FD_SET(s_rtp_v, &rfds); if (s_rtp_v > maxfd) maxfd = s_rtp_v; }

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
            /* ① 注册事务超时：32s ≈ RFC 3261 Timer F（UDP 非 INVITE 事务）的工程值。
                  没有这一步，包丢了就永远是"注册中"，设备要靠重启恢复。 */
            if (s_reg_pending && s_reg_sent_ms && (now_ms() - s_reg_sent_ms) > 32000u) {
                reg_fail("事务超时");
            }
            /* ② 到点重试：网络恢复后自动回到在线，不需要人工干预 */
            if (!s_registered && !s_reg_pending && !s_reg_fatal &&
                s_reg_retry_at_ms && now_ms() >= s_reg_retry_at_ms) {
                s_reg_retry_at_ms = 0;
                s_auth_tried = 0;
                send_register();
            }
            /* ③ 续期：Expires 的 50%~80%（每个周期现随机）。
                  固定 50% 是不够的——几百台设备同时上线，续期会在同一秒撞在一起。 */
            if (s_registered && !s_reg_pending && s_expires > 0 &&
                (now_ms() - s_last_register_ms) >
                    (uint64_t)((long long)s_expires * s_reg_refresh_pct / 100) * 1000u) {
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
        if (s_rtp_v != SOCK_INVALID && FD_ISSET(s_rtp_v, &rfds)) rtp_video_on_readable();
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
/*---------------------------------------------------------------------------
 * 配置：编译默认 → sip.ini → 环境变量（后者覆盖前者）
 *
 * 产品形态是"上电即用"：出厂把 sip.ini 烧进 /etc/indoor/，现场不需要任何
 * 环境变量，也不该让安装工碰命令行。开发机上反过来——环境变量随手改，
 * 方便换服务器、压测、做 CI。
 *
 * 字符串一律落在静态缓冲里：getenv 的返回值生命周期不由我们控制，
 * ini 的行缓冲每轮就被覆盖，直接用指针存下来必然踩野指针。
 *---------------------------------------------------------------------------*/

static char *cfg_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static void cfg_copy(char *dst, size_t dsz, const char *v)
{
    if (!v) return;
    snprintf(dst, dsz, "%s", v);
}

/* 极简 ini 解析：`key = value`，`#`/`;` 起头为注释，`[section]` 直接跳过。
   刻意不引第三方解析库——语法就这么几行，多一个依赖不值得。 */
static bool cfg_load_ini(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[256];
    int  used = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = cfg_trim(line);
        if (!*p || *p == '#' || *p == ';' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = cfg_trim(p);
        char *v = cfg_trim(eq + 1);
        /* 行内注释：` ; ...` / ` # ...`（前面必须有空白，否则会把密码里的符号切掉） */
        for (char *c = v; *c; c++) {
            if ((*c == ';' || *c == '#') && (c == v || c[-1] == ' ' || c[-1] == '\t')) {
                *c = 0;
                break;
            }
        }
        v = cfg_trim(v);
        size_t vl = strlen(v);
        if (vl >= 2 && (v[0] == '"' || v[0] == '\'') && v[vl - 1] == v[0]) { v[vl - 1] = 0; v++; }

        if      (!strcmp(k, "server"))      { cfg_copy(s_cfg_server, sizeof(s_cfg_server), v);
                                              s_cfg_has_server = (*v != 0); }
        else if (!strcmp(k, "port"))        s_cfg_sport      = (uint16_t)atoi(v);
        else if (!strcmp(k, "user"))        cfg_copy(s_cfg_user, sizeof(s_cfg_user), v);
        else if (!strcmp(k, "password"))    cfg_copy(s_cfg_pass, sizeof(s_cfg_pass), v);
        else if (!strcmp(k, "local_port"))  s_cfg_lport      = (uint16_t)atoi(v);
        else if (!strcmp(k, "local_ip"))    cfg_copy(s_cfg_localip, sizeof(s_cfg_localip), v);
        else if (!strcmp(k, "expires"))     s_cfg_expires    = atoi(v);
        else if (!strcmp(k, "no_register")) s_cfg_p2p        = (atoi(v) != 0 || !strcmp(v, "true"));
        else continue;                          /* 未知键忽略：向前兼容旧/新配置 */
        used++;
    }
    fclose(f);
    printf("[sip] ini %s: %d 项\n", path, used);
    return true;
}

bool app_sip_init_from_env(void)
{
    /* ① 配置文件：SIP_INI 指定 > /etc/indoor/sip.ini（板子）> ./sip.ini（开发机） */
    const char *ini = getenv("SIP_INI");
    if (ini && *ini) {
        if (!cfg_load_ini(ini)) printf("[sip] ini %s 打不开，改用默认值\n", ini);
    } else if (!cfg_load_ini("/etc/indoor/sip.ini")) {
        cfg_load_ini("sip.ini");
    }

    /* ② 环境变量覆盖（调试/压测/CI 用，优先级最高，语义与之前完全一致） */
    const char *e;
    if ((e = getenv("SIP_SERVER")) && *e)     { cfg_copy(s_cfg_server, sizeof(s_cfg_server), e);
                                                s_cfg_has_server = true; }
    if ((e = getenv("SIP_PORT")) && *e)       s_cfg_sport   = (uint16_t)atoi(e);
    if ((e = getenv("SIP_USER")) && *e)       cfg_copy(s_cfg_user, sizeof(s_cfg_user), e);
    if ((e = getenv("SIP_PASS")))             cfg_copy(s_cfg_pass, sizeof(s_cfg_pass), e);
    if ((e = getenv("SIP_LOCAL_PORT")) && *e) s_cfg_lport   = (uint16_t)atoi(e);
    if ((e = getenv("SIP_LOCAL_IP")) && *e)   cfg_copy(s_cfg_localip, sizeof(s_cfg_localip), e);
    if ((e = getenv("SIP_EXPIRES")) && *e)    s_cfg_expires = atoi(e);
    if (getenv("SIP_NO_REGISTER"))            s_cfg_p2p     = true;

    s_cfg_expires = (s_cfg_expires < 60) ? 60 : s_cfg_expires;   /* 太快会被服务器当泛洪 */

    /* ③ 判定是否启用：点对点模式（门口机直连 / MicroSIP 直呼）不需要服务器 */
    if (!s_cfg_has_server && !s_cfg_p2p) {
        printf("[sip] 未配置服务器 -> SIP 关闭（纯 UI 模式）\n");
        printf("[sip] 要启用：写好 sip.ini，或设 SIP_SERVER=...（见 config/sip.ini.example）\n");
        return false;
    }

    s_cfg.server      = s_cfg_server;
    s_cfg.server_port = s_cfg_sport;
    s_cfg.user        = s_cfg_user;
    s_cfg.pass        = s_cfg_pass;
    s_cfg.local_port  = s_cfg_lport;

    /* 强迫回声模式（CI 无麦克风时用，验证媒体双向通路而不依赖声卡） */
    s_rtp_echo = getenv("SIP_RTP_ECHO") != NULL;

    srand((unsigned)time(NULL));
    snprintf(s_regid, sizeof(s_regid), "indoor-%08x-%u@%s",
             (unsigned)rand(), proc_id(), s_local_ip);
    printf("[sip] cfg: %s:%u user=%s local=%u expires=%d%s\n",
           s_cfg.server, s_cfg.server_port, s_cfg.user, s_cfg.local_port, s_cfg_expires,
           s_cfg_p2p ? " p2p" : "");
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
