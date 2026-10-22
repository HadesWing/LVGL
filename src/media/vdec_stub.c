/**
 * @file vdec_stub.c
 * @brief vdec 抽象层的空实现 —— 没装 FFmpeg 时链接这一份
 *
 * 存在意义只有一个：**让主工程在没有解码器的机器上照样编过、照样跑 UI**。
 *
 * 这一点在嵌入式项目里不是洁癖：交叉编译 RK3308 时不会去编一份 libavcodec
 * （板子用 MPP 硬解），但同一份 CMakeLists 也要能在开发机上出 PC 版本。
 * 如果视频代码硬依赖 ffmpeg，那"板子能编、PC 不能编"或者反过来的情况必然出现，
 * 最后就是两份配置漂移。
 *
 * 行为约定：vdec_available() == false，vdec_open() == NULL。
 * 上层（app_video.c）看到 open 失败就打一行"视频解码不可用"，然后只收包不落盘 ——
 * 不会崩，也不会假装解码成功。
 */
#include "vdec.h"

bool vdec_available(void) { return false; }

const char *vdec_backend_name(void) { return "none"; }

vdec_t *vdec_open(void) { return NULL; }

void vdec_close(vdec_t *v) { (void)v; }

int vdec_feed(vdec_t *v, const uint8_t *au, int len, int64_t pts)
{
    (void)v; (void)au; (void)len; (void)pts;
    return -1;
}

int vdec_get(vdec_t *v, vframe_t *out)
{
    (void)v; (void)out;
    return -1;
}

const vdec_stats_t *vdec_stats(const vdec_t *v) { (void)v; return NULL; }

void vdec_flush(vdec_t *v) { (void)v; }
