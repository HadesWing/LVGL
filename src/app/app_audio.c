/**
 * @file app_audio.c
 * @brief 音频后端（桌面用 SDL2；板子换 ALSA 时只改本文件的设备分支）
 *
 * 路由四选一（详见 app_audio.h 顶部的说明）：
 *   上行 SIP_AUDIO_SRC  = mic | file | tone | none
 *   下行 SIP_AUDIO_SINK = spk | file | drop
 *
 * file 模式的用途是**把"房间"换成"文件"**：发送端从一个已知 WAV 循环取数，
 * 接收端把收到的音频存成 WAV。这样整条音频通路可以脱离麦克风和喇叭验证 ——
 * 没有采集设备的机器（比如本机）也能把音频测完。
 */
#include "app_audio.h"
#include "g711.h"
#include "wav.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE       G711_RATE   /* 8000 */
#define RING_LEN   (RATE)      /* 0.5 秒缓冲，够扛网络抖动 */
#define PREBUF     480         /* 60ms 预缓冲后才开播，避免起播爆音 */
#define FRAME      160         /* 20ms @ 8k */

/* 单生产者/单消费者环；用一把锁保护索引，8k 的数据量下几乎没有争用 */
typedef struct {
    int16_t  buf[RING_LEN];
    int      rd, wr, cnt;
    SDL_mutex *lock;
    uint32_t lost;            /* 溢出丢弃的采样数（诊断用） */
} ring_t;

static ring_t           s_cap;      /* 麦克风 → RTP */
static ring_t           s_play;     /* RTP → 喇叭 */
static SDL_AudioDeviceID s_cap_dev, s_play_dev;
static bool             s_open;
static bool             s_cap_ok, s_play_ok;
static bool             s_playing;  /* 预缓冲够了才开始出声 */
static uint32_t         s_underrun;

/*---------------------------------------------------------------------------
 * 路由配置
 *---------------------------------------------------------------------------*/
static audio_src_mode_t  s_src  = AUDIO_SRC_MIC;
static audio_sink_mode_t s_sink = AUDIO_SINK_SPK;

static FILE    *s_src_fp;
static char     s_src_path[260];
static uint32_t s_src_off;        /* data 块起始偏移 */
static uint32_t s_src_len;        /* data 块字节数 */
static uint32_t s_src_pos;        /* 已读字节（相对 data 起点） */
static int      s_src_ch;         /* 1 = 单声道，2 = 双声道（取左） */
static uint32_t s_src_rate;
static uint32_t s_src_loops;      /* 循环次数（回绕次数） */

static FILE    *s_sink_fp;
static char     s_sink_path[260];
static uint32_t s_sink_bytes;     /* 已写 PCM 字节数 */
static uint32_t s_sink_patch;     /* 距上次回填 WAV 头的写入次数 */
static uint32_t s_rx_samples;     /* 下行收到的采样总数（含 drop 模式） */
static int32_t  s_rx_peak;        /* 下行最大绝对值（判断"是不是全静音"） */

static int      s_tone_phase;

/*---------------------------------------------------------------------------
 * 本地提示音引擎（M2）
 *
 * 【为什么是查表 + 相位累加，不是 sin()】
 *   项目原则是不拉 libm（板子上少一个依赖）。450Hz @ 8k 不是整数周期
 *   （8000/450 = 17.78），1kHz 那种 8 点小表用不了，所以用 256 点大表 +
 *   定点相位累加：phase 单位是 1/10 表格项，每采样步进 10*256*freq/RATE，
 *   450Hz = 288/10。频率误差 < 0.4%（表格分辨率 1.4Hz），提示音够用。
 *---------------------------------------------------------------------------*/
#define TONE_AMPL      5000     /* ≈ -10.3dBFS：够醒耳，不炸耳 */
#define TONE_450_IDX   288      /* phase 增量（1/10 表格项/采样）*/

static const int16_t k_sine256[256] = {
         0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
      6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
     12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
     18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
     23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
     27245, 27684, 28106, 28510, 28898, 29268, 29621, 29956,
     30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
     32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
     32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285,
     32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
     30273, 29956, 29621, 29268, 28898, 28510, 28106, 27684,
     27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
     23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868,
     18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
     12539, 11793, 11039, 10278,  9512,  8739,  7962,  7179,
      6393,  5602,  4808,  4011,  3212,  2410,  1608,   804,
         0,  -804, -1608, -2410, -3212, -4011, -4808, -5602,
     -6393, -7179, -7962, -8739, -9512,-10278,-11039,-11793,
    -12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,
    -18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,
    -23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,
    -27245,-27684,-28106,-28510,-28898,-29268,-29621,-29956,
    -30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,
    -32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,
    -32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,
    -32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,
    -30273,-29956,-29621,-29268,-28898,-28510,-28106,-27684,
    -27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,
    -23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,
    -18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,
    -12539,-11793,-11039,-10278, -9512, -8739, -7962, -7179,
     -6393, -5602, -4808, -4011, -3212, -2410, -1608,  -804,
};

static struct {
    bool            active;
    audio_tone_id_t id;
    uint32_t        start_ms;   /* 本次 start 的时刻（SDL_GetTicks） */
    uint32_t        on_ms, off_ms, max_ms;
    uint32_t        phase;      /* 1/10 表格项 */
    bool            warned;     /* 没喇叭只警告一次 */
} s_tone2;

static void ring_push(ring_t *r, const int16_t *src, int n);   /* 定义在环操作一节 */

static void tone_pattern(audio_tone_id_t id, uint32_t *on, uint32_t *off, uint32_t *max)
{
    if (id == AUDIO_TONE_RINGBACK) { *on = 1000; *off = 4000; *max = 0; }
    else                           { *on = 350;  *off = 350;  *max = 2000; }
}

static bool tone_enabled(void)
{
    static int en = -1;
    if (en < 0) en = getenv("SIP_NO_TONE") ? 0 : 1;
    return en == 1;
}

void app_audio_tone_start(audio_tone_id_t id)
{
    if (!tone_enabled()) return;
    if (s_tone2.active && s_tone2.id == id) return;   /* 幂等：状态机反复触发别打断 */

    tone_pattern(id, &s_tone2.on_ms, &s_tone2.off_ms, &s_tone2.max_ms);
    s_tone2.id       = id;
    s_tone2.start_ms = SDL_GetTicks();
    s_tone2.phase    = 0;
    s_tone2.active   = true;

    if (!s_open || s_sink != AUDIO_SINK_SPK || !s_play_ok) {
        if (!s_tone2.warned) {
            s_tone2.warned = true;
            printf("[audio] 提示音静默：喇叭路由没开，只记账不出声\n");
        }
    } else {
        printf("[audio] tone %s 开始\n", id == AUDIO_TONE_RINGBACK ? "振铃/回铃" : "忙音");
        fflush(stdout);
    }
}

void app_audio_tone_stop(void)
{
    if (s_tone2.active) {
        printf("[audio] tone 停止（响了 %u ms）\n",
               (unsigned)(SDL_GetTicks() - s_tone2.start_ms));
        fflush(stdout);
    }
    s_tone2.active = false;
}

void app_audio_tone_tick(void)
{
    if (!s_tone2.active) return;

    uint32_t now = SDL_GetTicks();
    if (s_tone2.max_ms && now - s_tone2.start_ms >= s_tone2.max_ms) {
        app_audio_tone_stop();
        return;
    }
    /* 没喇叭就只推进时钟（max_ms 到点自动停的逻辑要工作） */
    if (!s_open || s_sink != AUDIO_SINK_SPK || !s_play_ok) return;

    /* 本次 tick 该补多少 PCM：按真实时间差算，select 卡一下也不走音 */
    static uint32_t last_ms;
    if (last_ms == 0 || now < last_ms) last_ms = now;
    uint32_t span = now - last_ms;
    if (span < 20) return;
    if (span > 100) span = 100;              /* 卡久了也别一次灌 1 秒 */
    last_ms = now;

    int n = (int)(span * RATE / 1000);
    int16_t buf[800];                        /* 100ms @ 8k 的上限 */
    uint32_t cyc = s_tone2.on_ms + s_tone2.off_ms;
    uint32_t base = now - s_tone2.start_ms;

    for (int i = 0; i < n; i++) {
        uint32_t t = base + (uint32_t)i * 1000u / (uint32_t)RATE;
        if (t % cyc < s_tone2.on_ms) {       /* 通段：450Hz 查表 */
            buf[i] = (int16_t)((k_sine256[s_tone2.phase / 10] * TONE_AMPL) / 32767);
            s_tone2.phase += TONE_450_IDX;
            if (s_tone2.phase >= 2560) s_tone2.phase -= 2560;
        } else {
            buf[i] = 0;                      /* 断段：静音 */
        }
    }
    ring_push(&s_play, buf, n);
}

/*---------------------------------------------------------------------------
 * 环操作（调用方负责加锁）
 *---------------------------------------------------------------------------*/
static void ring_init(ring_t *r)
{
    r->rd = r->wr = r->cnt = 0;
    r->lost = 0;
    r->lock = SDL_CreateMutex();
}

static int ring_free(const ring_t *r) { return RING_LEN - r->cnt; }

static void ring_push(ring_t *r, const int16_t *src, int n)
{
    SDL_LockMutex(r->lock);
    int space = ring_free(r);
    if (n > space) {
        r->lost += (uint32_t)(n - space);
        n = space;                 /* 满了就丢最新的尾巴，绝不阻塞回调 */
    }
    for (int i = 0; i < n; i++) {
        r->buf[r->wr] = src[i];
        r->wr = (r->wr + 1) % RING_LEN;
    }
    r->cnt += n;
    SDL_UnlockMutex(r->lock);
}

static int ring_pop(ring_t *r, int16_t *dst, int n)
{
    SDL_LockMutex(r->lock);
    if (n > r->cnt) n = r->cnt;
    for (int i = 0; i < n; i++) {
        dst[i] = r->buf[r->rd];
        r->rd = (r->rd + 1) % RING_LEN;
    }
    r->cnt -= n;
    SDL_UnlockMutex(r->lock);
    return n;
}

static int ring_count(ring_t *r)
{
    SDL_LockMutex(r->lock);
    int c = r->cnt;
    SDL_UnlockMutex(r->lock);
    return c;
}

/*---------------------------------------------------------------------------
 * WAV 落盘（只支持 PCM 16bit；8k 单声道是目标格式）
 *
 * 头的生成与解析都搬到了 wav.c —— 那部分原先在这里是静态函数，
 * 想单测就得把 SDL 拖进测试工程。现在只留两个字节序小工具给采样用。
 *---------------------------------------------------------------------------*/
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 44 字节标准头由 wav.c 生成（那里有单测）。data 大小未知时先写 0，
   落盘过程中反复回填 —— 这样即使进程被 kill，文件也是「能播到断开处」的合法 WAV。 */
static void sink_wav_patch(void)
{
    if (!s_sink_fp) return;
    long cur = ftell(s_sink_fp);
    uint8_t h[WAV_HEADER_BYTES];
    wav_write_header(h, s_sink_bytes, 1, RATE);
    fseek(s_sink_fp, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), s_sink_fp);
    fseek(s_sink_fp, cur, SEEK_SET);
    fflush(s_sink_fp);
    s_sink_patch = 0;
}

static bool sink_wav_open(const char *path)
{
    s_sink_fp = fopen(path, "wb");
    if (!s_sink_fp) {
        printf("[audio] 打不开落盘文件 %s（目录不存在？）\n", path);
        return false;
    }
    uint8_t h[WAV_HEADER_BYTES];
    wav_write_header(h, 0, 1, RATE);
    fwrite(h, 1, sizeof(h), s_sink_fp);
    s_sink_bytes = 0;
    s_sink_patch = 0;
    printf("[audio] 下行落盘 → %s (8k mono s16 wav)\n", path);
    return true;
}

static void sink_wav_close(void)
{
    if (!s_sink_fp) return;
    sink_wav_patch();
    fclose(s_sink_fp);
    s_sink_fp = NULL;
    printf("[audio] 落盘完成: %s  %u 采样 (%.2f 秒)  峰值=%d\n",
           s_sink_path, s_rx_samples, (double)s_rx_samples / RATE, s_rx_peak);
}

static void sink_wav_write(const int16_t *pcm, int n)
{
    if (!s_sink_fp) return;
    uint8_t buf[FRAME * 2];
    while (n > 0) {
        int chunk = n > FRAME ? FRAME : n;
        for (int i = 0; i < chunk; i++) {
            put_le16(buf + i * 2, (uint16_t)pcm[i]);
            int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
            if (a > s_rx_peak) s_rx_peak = a;
        }
        fwrite(buf, 1, (size_t)chunk * 2, s_sink_fp);
        s_sink_bytes += (uint32_t)chunk * 2;
        n -= chunk;
        pcm += chunk;
        if (++s_sink_patch >= 50) sink_wav_patch();   /* 每秒刷一次头 */
    }
}

/* 打开音源文件：识别 WAV（RIFF）或裸 PCM(8k mono s16)
   WAV 头解析在 wav.c（有单测），这里只管文件句柄和业务语义 */
static bool src_file_open(const char *path)
{
    s_src_fp = fopen(path, "rb");
    if (!s_src_fp) {
        printf("[audio] 打不开音源文件 %s\n", path);
        return false;
    }

    wav_info_t wi;
    if (!wav_parse(s_src_fp, &wi)) {
        /* 当裸 PCM 处理：整文件就是采样 */
        fseek(s_src_fp, 0, SEEK_END);
        long sz = ftell(s_src_fp);
        fseek(s_src_fp, 0, SEEK_SET);
        s_src_off  = 0;
        s_src_len  = (uint32_t)(sz > 0 ? sz : 0);
        s_src_ch   = 1;
        s_src_rate = RATE;
        printf("[audio] 音源当裸 PCM 处理: %s (%u 字节)\n", path, s_src_len);
    } else {
        s_src_ch   = wi.channels;
        s_src_rate = wi.rate;
        s_src_off  = wi.data_offset;
        s_src_len  = wi.data_bytes;
        if (s_src_rate != RATE || s_src_ch > 2) {
            printf("[audio] 警告: 音源是 %u Hz / %d 声道，期望 %d Hz / 1 声道。"
                   "照 8k 直接送，音调会不对\n", s_src_rate, s_src_ch, RATE);
        }
        printf("[audio] 音源 → %s (%u Hz %dch, %u 字节 = %.2f 秒, 循环)\n",
               path, s_src_rate, s_src_ch, s_src_len,
               (double)s_src_len / (double)(s_src_rate * s_src_ch * 2));
    }
    s_src_pos = 0;
    s_src_loops = 0;
    fseek(s_src_fp, (long)s_src_off, SEEK_SET);
    return true;
}

/* 从文件取满 n 个采样（不够就回绕，永不停） */
static int src_file_read(int16_t *dst, int n)
{
    if (!s_src_fp || s_src_len == 0) return 0;
    int got = 0;
    while (got < n) {
        uint32_t frame_bytes = (uint32_t)s_src_ch * 2u;
        if (s_src_pos + frame_bytes > s_src_len) {
            s_src_pos = 0;
            fseek(s_src_fp, (long)s_src_off, SEEK_SET);
            s_src_loops++;
            if (s_src_len < frame_bytes) break;      /* 文件比一帧还短 */
        }
        int want = n - got;
        if ((uint32_t)(want * (int)frame_bytes) > s_src_len - s_src_pos)
            want = (int)((s_src_len - s_src_pos) / frame_bytes);
        if (want <= 0) break;

        uint8_t raw[FRAME * 4];
        int frames = want > FRAME ? FRAME : want;
        size_t need = (size_t)frames * frame_bytes;
        size_t rd = fread(raw, 1, need, s_src_fp);
        if (rd < need) {                             /* 文件被截断 */
            fseek(s_src_fp, (long)s_src_off, SEEK_SET);
            s_src_pos = 0;
            s_src_loops++;
            break;
        }
        for (int i = 0; i < frames; i++) {
            const uint8_t *p = raw + (size_t)i * frame_bytes;
            dst[got + i] = (int16_t)get_le16(p);     /* 双声道取左 */
        }
        s_src_pos += (uint32_t)need;
        got += frames;
    }
    return got;
}

/*---------------------------------------------------------------------------
 * SDL 回调（独立线程！不要碰 LVGL/业务状态）
 *---------------------------------------------------------------------------*/
static void cap_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    ring_push(&s_cap, (const int16_t *)stream, len / (int)sizeof(int16_t));
}

static void play_cb(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    int want = len / (int)sizeof(int16_t);
    int16_t *out = (int16_t *)stream;

    /* 预缓冲不够就先静音，等攒到 PREBUF 再开播 */
    if (!s_playing && ring_count(&s_play) >= PREBUF) s_playing = true;

    if (!s_playing) {
        memset(out, 0, (size_t)len);
        return;
    }
    int got = ring_pop(&s_play, out, want);
    if (got < want) {
        memset(out + got, 0, (size_t)(want - got) * sizeof(int16_t));
        s_underrun++;
    }
}

/*---------------------------------------------------------------------------
 * 路由解析（环境变量；现场产品走 sip.ini，这几个只用于自测）
 *---------------------------------------------------------------------------*/
static void route_parse_env(void)
{
    /* SIP_NO_AUDIO=1 是"纯信令"开关：不许碰声卡。
       但它不该把 tone 也关掉 —— CI 里正是靠 SIP_NO_AUDIO + SIP_TONE 的组合
       在没有声卡的机器上验证"编码→RTP"这一段。所以这里的语义是：
       没显式指定路由时，音源退成 tone/none、音宿退成 drop。 */
    bool no_audio = getenv("SIP_NO_AUDIO") != NULL;
    const char *s = getenv("SIP_AUDIO_SRC");

    if (s && *s) {
        if      (!strcmp(s, "mic"))  s_src = AUDIO_SRC_MIC;
        else if (!strcmp(s, "file")) s_src = AUDIO_SRC_FILE;
        else if (!strcmp(s, "tone")) s_src = AUDIO_SRC_TONE;
        else if (!strcmp(s, "none")) s_src = AUDIO_SRC_NONE;
        else printf("[audio] SIP_AUDIO_SRC=%s 不认识，按 mic 处理\n", s);
    } else if (getenv("SIP_TONE")) {
        s_src = AUDIO_SRC_TONE;          /* 老开关，等价于 src=tone */
    } else if (no_audio) {
        s_src = AUDIO_SRC_NONE;
    }

    const char *k = getenv("SIP_AUDIO_SINK");
    if (k && *k) {
        if      (!strcmp(k, "spk"))  s_sink = AUDIO_SINK_SPK;
        else if (!strcmp(k, "file")) s_sink = AUDIO_SINK_FILE;
        else if (!strcmp(k, "drop")) s_sink = AUDIO_SINK_DROP;
        else printf("[audio] SIP_AUDIO_SINK=%s 不认识，按 spk 处理\n", k);
    } else if (no_audio) {
        s_sink = AUDIO_SINK_DROP;
    }

    const char *sf = getenv("SIP_AUDIO_SRC_FILE");
    snprintf(s_src_path, sizeof(s_src_path), "%s", (sf && *sf) ? sf : "test_8k.wav");
    const char *kf = getenv("SIP_AUDIO_SINK_FILE");
    snprintf(s_sink_path, sizeof(s_sink_path), "%s", (kf && *kf) ? kf : "rx.wav");
}

const char *app_audio_route_desc(void)
{
    static char buf[192];
    const char *srcs[] = { "mic", "file", "tone", "none" };
    const char *sks[]  = { "spk", "file", "drop" };
    snprintf(buf, sizeof(buf), "src=%s", srcs[s_src]);
    size_t n = strlen(buf);
    if (s_src == AUDIO_SRC_FILE)
        snprintf(buf + n, sizeof(buf) - n, "(%s)", s_src_path);
    n = strlen(buf);
    snprintf(buf + n, sizeof(buf) - n, " sink=%s", sks[s_sink]);
    n = strlen(buf);
    if (s_sink == AUDIO_SINK_FILE)
        snprintf(buf + n, sizeof(buf) - n, "(%s)", s_sink_path);
    return buf;
}

/*---------------------------------------------------------------------------
 * 对外接口
 *---------------------------------------------------------------------------*/
bool app_audio_open(void)
{
    if (s_open) return true;

    route_parse_env();
    s_rx_samples = 0;
    s_rx_peak    = 0;

    ring_init(&s_cap);
    ring_init(&s_play);
    s_playing  = false;
    s_underrun = 0;
    s_cap_ok = s_play_ok = false;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    memset(&have, 0, sizeof(have));   /* 不开设备时它保持全 0，日志里别打垃圾值 */
    want.freq     = RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = G711_FRAME_SAMPLES;   /* 20ms 一帧，与 RTP 节奏对齐 */

    /* 只在真的需要设备时才初始化 SDL 音频子系统 ——
       file/tone/none 模式下完全不碰声卡，没有声卡的机器也能跑。 */
    bool need_dev = (s_src == AUDIO_SRC_MIC) || (s_sink == AUDIO_SINK_SPK);
    if (need_dev) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            printf("[audio] SDL audio init failed: %s\n", SDL_GetError());
        } else {
            /* 采集与播放**分别**尝试：桌面/工控机常常没有麦克风，
               但"能听见门口机说话"仍然是有价值的一半功能，不能整体判失败。

               注意 allowed_changes 传 0：不允许 SDL 改声道数/格式。
               否则设备是立体声时会返回 2ch —— 我们的回调按"单声道 160 采样/20ms"
               取数，实际每 20ms 被要走 320 采样，缓冲被抽干（实测 underrun 415 次）。
               SDL 会在内部把单声道 8k S16 转成设备格式。 */
            if (s_src == AUDIO_SRC_MIC) {
                want.callback = cap_cb;
                s_cap_dev = SDL_OpenAudioDevice(NULL, 1 /*采集*/, &want, &have, 0);
                if (!s_cap_dev) printf("[audio] 无采集设备: %s\n", SDL_GetError());
                else            s_cap_ok = true;
            }
            if (s_sink == AUDIO_SINK_SPK) {
                want.callback = play_cb;
                s_play_dev = SDL_OpenAudioDevice(NULL, 0 /*播放*/, &want, &have, 0);
                if (!s_play_dev) printf("[audio] 无播放设备: %s\n", SDL_GetError());
                else             s_play_ok = true;
            }
        }
    }

    if (s_src == AUDIO_SRC_FILE && !src_file_open(s_src_path))
        s_src = AUDIO_SRC_NONE;          /* 文件打不开：退化成只收不发，不整体失败 */

    bool can_rec = (s_src == AUDIO_SRC_MIC)  ? s_cap_ok :
                   (s_src == AUDIO_SRC_FILE) ? (s_src_fp != NULL) :
                   (s_src == AUDIO_SRC_TONE);
    bool can_play = (s_sink == AUDIO_SINK_SPK)  ? s_play_ok :
                    (s_sink == AUDIO_SINK_FILE) ? true : false;

    if (!can_rec && !can_play) {
        if (s_cap.lock)  SDL_DestroyMutex(s_cap.lock);
        if (s_play.lock) SDL_DestroyMutex(s_play.lock);
        s_cap.lock = s_play.lock = NULL;
        printf("[audio] 没有可用的音源也没有落盘手段 -> 交给上层决定\n");
        return false;
    }

    if (s_cap_dev)  SDL_PauseAudioDevice(s_cap_dev, 0);
    if (s_play_dev) SDL_PauseAudioDevice(s_play_dev, 0);
    s_open = true;
    printf("[audio] open ok: %s  (cap=%s play=%s, %d Hz %d ch)\n",
           app_audio_route_desc(),
           can_rec ? "yes" : "no", can_play ? "yes" : "no",
           have.freq ? have.freq : RATE, have.channels ? have.channels : 1);
    fflush(stdout);
    return true;
}

void app_audio_close(void)
{
    if (!s_open) return;
    s_tone2.active = false;   /* 提示音跟着音频子系统一起关，别留到下次通话 */
    if (s_cap_dev)  { SDL_PauseAudioDevice(s_cap_dev, 1);  SDL_CloseAudioDevice(s_cap_dev);  s_cap_dev = 0; }
    if (s_play_dev) { SDL_PauseAudioDevice(s_play_dev, 1); SDL_CloseAudioDevice(s_play_dev); s_play_dev = 0; }

    if (s_sink_fp) {
        /* 落盘模式：这次通话结束就把文件收尾（一次通话一个 wav） */
        sink_wav_close();
    }
    if (s_src_fp) { fclose(s_src_fp); s_src_fp = NULL; }   /* 音源下次 open 重开 */

    if (s_cap.lock)  SDL_DestroyMutex(s_cap.lock);
    if (s_play.lock) SDL_DestroyMutex(s_play.lock);
    s_cap.lock = s_play.lock = NULL;
    s_open = s_cap_ok = s_play_ok = false;
    printf("[audio] closed (underrun=%u, cap_lost=%u, rx=%u 采样)\n",
           s_underrun, s_cap.lost, s_rx_samples);
    fflush(stdout);
}

bool app_audio_is_open(void) { return s_open; }

void app_audio_call_end(void)
{
    /* 落盘模式：一次通话一个 wav，在这里收尾；下次 write 会自动开新文件 */
    if (s_sink_fp) sink_wav_close();
    app_audio_tone_stop();
    s_tone2.warned = false;   /* 新通话重新给一次"没喇叭"的提示机会 */
}

bool app_audio_can_record(void)
{
    if (s_src == AUDIO_SRC_MIC)  return s_open && s_cap_ok;
    if (s_src == AUDIO_SRC_FILE) return s_src_fp != NULL;
    if (s_src == AUDIO_SRC_TONE) return true;
    return false;
}

bool app_audio_can_play(void)
{
    if (s_sink == AUDIO_SINK_SPK)  return s_open && s_play_ok;
    if (s_sink == AUDIO_SINK_FILE) return true;
    return false;
}

int app_audio_read(int16_t *pcm, int n)
{
    switch (s_src) {
    case AUDIO_SRC_MIC:
        if (!s_open || !s_cap_ok) return 0;
        return ring_pop(&s_cap, pcm, n);
    case AUDIO_SRC_TONE: {
        /* 1kHz @ 8k = 每个周期正好 8 个点，查表即可（省掉 math.h 和 libm） */
        static const int16_t sine8[8] = { 0, 11585, 16384, 11585, 0, -11585, -16384, -11585 };
        for (int i = 0; i < n; i++) {
            pcm[i] = sine8[s_tone_phase & 7];
            s_tone_phase++;
        }
        return n;
    }
    case AUDIO_SRC_FILE:
        return src_file_read(pcm, n);
    default:
        return 0;                    /* NONE：只收不发 */
    }
}

void app_audio_write(const int16_t *pcm, int n)
{
    s_rx_samples += (uint32_t)n;

    if (s_sink == AUDIO_SINK_SPK) {
        if (s_open && s_play_ok) ring_push(&s_play, pcm, n);
    } else if (s_sink == AUDIO_SINK_FILE) {
        if (!s_sink_fp && !sink_wav_open(s_sink_path)) s_sink = AUDIO_SINK_DROP;
        sink_wav_write(pcm, n);
    }
    /* DROP：只统计 rx 与峰值，不落地 */
}

void app_audio_stats(uint32_t *underrun, uint32_t *overrun)
{
    if (underrun) *underrun = s_underrun;
    if (overrun)  *overrun  = s_cap.lost;
}
