/* h264_probe —— 用 libavcodec 解 H.264 裸流，量化解码耗时
 *
 * 定位：PC 工具（不进产品）。产品里用的是同一套 API，只是被包在 src/media/vdec_ffmpeg.c
 *      后面。这个工具存在的意义是把"解码"这一步单独拎出来跑，先把它跑对、跑出数字，
 *      再接 RTP、再接屏 —— 不然三个子系统一起坏，你没法定位。
 *
 * 用法：
 *   h264_probe test480p.h264                 只统计
 *   h264_probe test480p.h264 --rgb565 f.raw  额外导出一帧 RGB565（验证上屏格式）
 *
 * 输出：
 *   帧数 / 分辨率 / 像素格式 / 解码耗时 min-avg-max / 折算 fps
 *
 * 注意：解码耗时只是 CPU 成本，不含 RTP 收包和上屏。真机还要加上那两段。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifdef _WIN32
#include <windows.h>
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "用法: %s <input.h264> [--rgb565 <out.raw>]\n"
            "  例: %s test480p.h264\n", argv[0], argv[0]);
        return 2;
    }
    const char *rgb_out = NULL;
    for (int i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], "--rgb565") == 0) rgb_out = argv[i + 1];

    size_t fsize = 0;
    uint8_t *fbuf = read_file(argv[1], &fsize);
    if (!fbuf) { fprintf(stderr, "读不到输入文件\n"); return 1; }

    /* --- 解码器：这就是 ffplay 内部做的第一件事，只是它多了一层容器/协议 --- */
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!dec) { fprintf(stderr, "找不到 H.264 解码器（FFmpeg 编译时被裁掉了？）\n"); return 1; }

    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    if (!ctx) return 1;
    ctx->thread_count = 1;              /* 单线程：贴近 RK3308 单核表现，也便于测单帧耗时 */
    if (avcodec_open2(ctx, dec, NULL) < 0) { fprintf(stderr, "avcodec_open2 失败\n"); return 1; }

    /* --- Annex-B 分帧：裸流没有容器，靠 parser 按起始码切 --- */
    AVCodecParserContext *parser = av_parser_init(dec->id);
    if (!parser) { fprintf(stderr, "parser init 失败\n"); return 1; }

    struct SwsContext *sws = NULL;
    int have_dst = 0;
    uint8_t *dst[4] = {0};
    int dst_linesize[4] = {0};
    int out_w = 0, out_h = 0;

    size_t off = 0;
    int frames = 0;
    double t_sum = 0, t_min = 1e9, t_max = 0;
    double t_first_ms = -1;
    enum AVPixelFormat pix = AV_PIX_FMT_NONE;
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frm = av_frame_alloc();

    while (off < fsize) {
        uint8_t *pkt_data = NULL;
        int pkt_size = 0;
        int used = av_parser_parse2(parser, ctx, &pkt_data, &pkt_size,
                                    fbuf + off, (int)(fsize - off),
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) break;
        off += (size_t)used;
        if (pkt_size <= 0) continue;

        pkt->data = pkt_data;
        pkt->size = pkt_size;
        /* 计时必须把 send 包进去：FFmpeg 的 H.264 解码器在 send_packet 里真正做解码，
           receive_frame 只是从内部队列取帧（耗时≈0）。分开计时会得到"0.00ms"的假数据。 */
        double t0 = now_ms();
        if (avcodec_send_packet(ctx, pkt) < 0) continue;

        while (1) {
            int rc = avcodec_receive_frame(ctx, frm);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { fprintf(stderr, "解码出错 %d\n", rc); break; }
            double dt = now_ms() - t0;
            t0 = now_ms();   /* 一个包产多帧时，后续帧单独计时 */

            if (frames == 0) {
                t_first_ms = dt;
                out_w = frm->width;
                out_h = frm->height;
                pix = (enum AVPixelFormat)frm->format;
                printf("[probe] 首帧: %dx%d  pix_fmt=%s\n",
                       out_w, out_h, av_get_pix_fmt_name(pix));
            }
            frames++;
            t_sum += dt;
            if (dt < t_min) t_min = dt;
            if (dt > t_max) t_max = dt;

            /* --- 导出一帧 RGB565LE：这就是喂给 lv_image 的格式 --- */
            if (rgb_out && frames == 1) {
                sws = sws_getContext(out_w, out_h, (enum AVPixelFormat)frm->format,
                                     out_w, out_h, AV_PIX_FMT_RGB565LE,
                                     SWS_BILINEAR, NULL, NULL, NULL);
                if (sws && av_image_alloc(dst, dst_linesize, out_w, out_h,
                                          AV_PIX_FMT_RGB565LE, 16) >= 0) {
                    sws_scale(sws, (const uint8_t *const *)frm->data, frm->linesize,
                              0, out_h, dst, dst_linesize);
                    have_dst = 1;
                }
            }
            av_frame_unref(frm);
        }
    }

    /* flush：把解码器里缓存的帧吐出来 */
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frm) == 0) { frames++; av_frame_unref(frm); }

    printf("\n===== 汇总 =====\n");
    printf("输入        : %s (%zu 字节)\n", argv[1], fsize);
    printf("解码帧数    : %d\n", frames);
    if (frames) {
        double avg = t_sum / frames;
        printf("首帧耗时    : %.2f ms（含解码器初始化，真机首次要先跑一次暖机）\n", t_first_ms);
        printf("单帧耗时    : min %.2f / avg %.2f / max %.2f ms（单线程）\n", t_min, avg, t_max);
        printf("等效解码上限: %.1f fps（只算 CPU，不含收包/上屏）\n", avg > 0 ? 1000.0 / avg : 0);
        printf("15fps 占用  : %.0f%% 的 CPU（单核）\n", avg * 15 / 10.0);
    }

    if (have_dst && rgb_out) {
        FILE *fo = fopen(rgb_out, "wb");
        if (fo) {
            /* 逐行写，跳过 stride 填充 */
            for (int y = 0; y < out_h; y++)
                fwrite(dst[0] + (size_t)y * dst_linesize[0], 1, (size_t)out_w * 2, fo);
            fclose(fo);
            printf("RGB565 帧   : %s (%d x %d x 2 = %d 字节/帧)\n",
                   rgb_out, out_w, out_h, out_w * out_h * 2);
        }
    }

    if (sws) sws_freeContext(sws);
    if (have_dst) av_freep(&dst[0]);
    av_parser_close(parser);
    av_packet_free(&pkt);
    av_frame_free(&frm);
    avcodec_free_context(&ctx);
    free(fbuf);
    return 0;
}
