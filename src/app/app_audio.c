/**
 * @file app_audio.c
 * @brief SDL2 音频后端（桌面模拟）。板子上换成 ALSA 时只改本文件。
 */
#include "app_audio.h"
#include "g711.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE       G711_RATE   /* 8000 */
#define RING_LEN   (RATE)      /* 0.5 秒缓冲，够扛网络抖动 */
#define PREBUF     480         /* 60ms 预缓冲后才开播，避免起播爆音 */

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
 * 对外接口
 *---------------------------------------------------------------------------*/
bool app_audio_open(void)
{
    if (s_open) return true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("[audio] SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    ring_init(&s_cap);
    ring_init(&s_play);
    s_playing  = false;
    s_underrun = 0;

    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq     = RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = G711_FRAME_SAMPLES;   /* 20ms 一帧，与 RTP 节奏对齐 */

    /* 采集与播放**分别**尝试：桌面/工控机常常没有麦克风，
       但"能听见门口机说话"仍然是有价值的一半功能，不能整体判失败。

       注意 allowed_changes 传 0：不允许 SDL 改声道数/格式。
       否则设备是立体声时会返回 2ch —— 我们的回调按"单声道 160 采样/20ms" 取数，
       实际每 20ms 被要走 320 采样，缓冲被抽干（实测 underrun 415 次）。
       SDL 会在内部把单声道 8k S16 转成设备格式。 */
    want.callback = cap_cb;
    s_cap_dev = SDL_OpenAudioDevice(NULL, 1 /*采集*/, &want, &have, 0);
    if (!s_cap_dev) {
        printf("[audio] 无采集设备（只收不放）: %s\n", SDL_GetError());
        s_cap_ok = false;
    } else {
        s_cap_ok = true;
    }

    want.callback = play_cb;
    s_play_dev = SDL_OpenAudioDevice(NULL, 0 /*播放*/, &want, &have, 0);
    if (!s_play_dev) {
        printf("[audio] 无播放设备（只采不放）: %s\n", SDL_GetError());
        s_play_ok = false;
    } else {
        s_play_ok = true;
    }

    if (!s_cap_ok && !s_play_ok) {
        if (s_cap.lock) SDL_DestroyMutex(s_cap.lock);
        if (s_play.lock) SDL_DestroyMutex(s_play.lock);
        s_cap.lock = s_play.lock = NULL;
        return false;
    }

    if (s_cap_dev)  SDL_PauseAudioDevice(s_cap_dev, 0);
    if (s_play_dev) SDL_PauseAudioDevice(s_play_dev, 0);
    s_open = true;
    printf("[audio] open ok: cap=%s play=%s (%d Hz %d ch)\n",
           s_cap_ok ? "yes" : "no", s_play_ok ? "yes" : "no", have.freq, have.channels);
    fflush(stdout);
    return true;
}

void app_audio_close(void)
{
    if (!s_open) return;
    if (s_cap_dev)  { SDL_PauseAudioDevice(s_cap_dev, 1);  SDL_CloseAudioDevice(s_cap_dev);  s_cap_dev = 0; }
    if (s_play_dev) { SDL_PauseAudioDevice(s_play_dev, 1); SDL_CloseAudioDevice(s_play_dev); s_play_dev = 0; }

    SDL_DestroyMutex(s_cap.lock);
    SDL_DestroyMutex(s_play.lock);
    s_cap.lock = s_play.lock = NULL;
    s_open = s_cap_ok = s_play_ok = false;
    printf("[audio] closed (underrun=%u, cap_lost=%u)\n", s_underrun, s_cap.lost);
    fflush(stdout);
}

bool app_audio_is_open(void) { return s_open; }
bool app_audio_can_record(void) { return s_open && s_cap_ok; }
bool app_audio_can_play(void) { return s_open && s_play_ok; }

int app_audio_read(int16_t *pcm, int n)
{
    if (!s_open || !s_cap_ok) return 0;
    return ring_pop(&s_cap, pcm, n);
}

void app_audio_write(const int16_t *pcm, int n)
{
    if (!s_open || !s_play_ok) return;
    ring_push(&s_play, pcm, n);
}

void app_audio_stats(uint32_t *underrun, uint32_t *overrun)
{
    if (underrun) *underrun = s_underrun;
    if (overrun)  *overrun  = s_cap.lost;
}
