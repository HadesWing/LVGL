/**
 * @file vdec_ffmpeg.c
 * @brief vdec 抽象层的 libavcodec 后端（PC 模拟用）
 *
 * 只用 libavcodec + libavutil，**不用 libavformat**：
 *   我们不读文件、不解析容器、不处理流探测 —— 输入是 RTP 重组好的 Annex-B 访问单元。
 *   少链接一个 libavformat 能省下 ~500KB 体积（见 docs/开源组件清单与占比.md）。
 *
 * 也不做 swscale 转换：解码器输出什么就是什么（H.264 8bit 4:2:0 → YUV420P）。
 * 一旦转成 RGB 落盘，就分不清"解码错了"和"转换错了"，验收判据会变脏。
 */
#include "vdec.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>

#include <stdlib.h>
#include <string.h>

struct vdec {
    AVCodecContext *ctx;
    AVFrame        *frm;
    AVPacket       *pkt;
    bool            draining;
    vdec_stats_t    st;
};

bool vdec_available(void) { return true; }

const char *vdec_backend_name(void) { return "ffmpeg"; }

vdec_t *vdec_open(void)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return NULL;

    vdec_t *v = (vdec_t *)calloc(1, sizeof(*v));
    if (!v) return NULL;

    v->ctx = avcodec_alloc_context3(codec);
    v->frm = av_frame_alloc();
    v->pkt = av_packet_alloc();
    if (!v->ctx || !v->frm || !v->pkt) {
        vdec_close(v);
        return NULL;
    }

    /* 低延迟优先：对讲场景不能容忍"攒够几帧才开始出图"。
       thread_count=1 是刻意的 —— 多线程软解会引入额外的帧延迟，
       而且 RK3308 这种四核 A35 上多线程软解收益很小（真要性能就上 MPP 硬解）。 */
    v->ctx->flags      |= AV_CODEC_FLAG_LOW_DELAY;
    v->ctx->thread_count = 1;

    if (avcodec_open2(v->ctx, codec, NULL) < 0) {
        vdec_close(v);
        return NULL;
    }
    return v;
}

void vdec_close(vdec_t *v)
{
    if (!v) return;
    if (v->ctx) avcodec_free_context(&v->ctx);
    if (v->frm) av_frame_free(&v->frm);
    if (v->pkt) av_packet_free(&v->pkt);
    free(v);
}

int vdec_feed(vdec_t *v, const uint8_t *au, int len, int64_t pts)
{
    if (!v || !v->ctx || !au || len <= 0) return -1;

    v->st.fed++;

    /* 不拷贝会出问题：avcodec_send_packet 在某些路径上会持有 buffer 引用，
       而我们的 AU 来自 h264_depack 的内部缓冲，下一包就被覆盖了。
       一次 I 帧 30~100KB 的 memcpy 完全可接受（相对于解码本身是噪声）。 */
    if (av_new_packet(v->pkt, len) < 0) {
        v->st.errors++;
        return -1;
    }
    memcpy(v->pkt->data, au, (size_t)len);
    v->pkt->pts = pts;
    v->pkt->dts = pts;

    int r = avcodec_send_packet(v->ctx, v->pkt);
    av_packet_unref(v->pkt);
    if (r < 0 && r != AVERROR(EAGAIN)) {
        /* 解码器吞掉坏数据是常态（比如丢了参考帧的 P 帧），不当作致命错误。
           计数留着，用来解释"为什么画面花了一下"。 */
        v->st.errors++;
        return -1;
    }
    return 0;
}

int vdec_get(vdec_t *v, vframe_t *out)
{
    if (!v || !v->ctx || !out) return -1;

    memset(out, 0, sizeof(*out));

    int r = avcodec_receive_frame(v->ctx, v->frm);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;   /* 还没解出来 */
    if (r < 0) {
        v->st.errors++;
        return -1;
    }

    out->w        = v->frm->width;
    out->h        = v->frm->height;
    out->keyframe = (v->frm->flags & AV_FRAME_FLAG_KEY) != 0 ||
                    v->frm->pict_type == AV_PICTURE_TYPE_I;
    out->pts      = v->frm->pts;

    /* 只接受 3 平面格式（YUV420P/YUVJ420P）。其它布局原样交出去会让
       落盘和转 RGB 两边都写错 —— 早点返回错误比默默输出垃圾好。 */
    if (v->frm->format != AV_PIX_FMT_YUV420P &&
        v->frm->format != AV_PIX_FMT_YUVJ420P) {
        v->st.errors++;
        av_frame_unref(v->frm);
        return -1;
    }

    for (int i = 0; i < 3; i++) {
        out->plane[i]    = v->frm->data[i];
        out->linesize[i] = v->frm->linesize[i];
    }
    v->st.frames++;
    return 1;
}

const vdec_stats_t *vdec_stats(const vdec_t *v)
{
    return v ? &v->st : NULL;
}

void vdec_flush(vdec_t *v)
{
    if (!v || !v->ctx) return;
    /* 丢掉解码器内部状态。libavcodec 没有"只清参考帧"的接口，
       最接近的做法是 flush 缓冲（avcodec_flush_buffers 会清掉所有延迟帧，
       但它同时让下一个 P 帧失去参考 —— 这正好是我们要的：
       宁可花屏一帧，也不要让错误顺着参考链一直传下去）。 */
    avcodec_flush_buffers(v->ctx);
}
