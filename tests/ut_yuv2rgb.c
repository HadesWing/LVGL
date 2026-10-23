/**
 * @file ut_yuv2rgb.c
 * @brief yuv2rgb 的单元测试：等比适配 + 像素换算 + 采样位置 + 缓冲边界
 *
 * 这一组用例存在的理由很具体：
 *   上屏那一路没法靠"看着差不多"验收 —— 花屏、偏色、上下颠倒、只画了半幅，
 *   在 320×480 的小屏上都可能被当成"信号不好"。所以把最后一步转换拆出来，
 *   用确定的 YUV 数值算出确定的 RGB565，逐像素对。
 *
 * 特别说明 px_* 那几条：用**标准色**（BT.601 的纯黑/纯白/纯红/纯绿/纯蓝）
 * 而不是自己算的期望值 —— 如果系数抄错（比如把 298 写成 296），
 * 用自算期望值测不出来（两边一起错），但标准色会对不上。
 */
#include "ut.h"
#include "yuv2rgb.h"
#include "vdec.h"

#include <string.h>

/*---------------------------------------------------------------------------
 * 造帧的小工具
 *---------------------------------------------------------------------------*/
#define FAKE_MAX_DIM 64
typedef struct {
    uint8_t y[FAKE_MAX_DIM * FAKE_MAX_DIM];
    uint8_t u[FAKE_MAX_DIM * FAKE_MAX_DIM];
    uint8_t v[FAKE_MAX_DIM * FAKE_MAX_DIM];
} fake_t;

/* extra = 每行额外的填充字节，用来验证转换是否按 linesize 走 */
static void frame_init(vframe_t *f, fake_t *s, int w, int h, int extra)
{
    memset(s, 0, sizeof(*s));
    memset(f, 0, sizeof(*f));
    f->plane[0] = s->y;
    f->plane[1] = s->u;
    f->plane[2] = s->v;
    f->linesize[0] = w + extra;
    f->linesize[1] = (w + 1) / 2 + extra;
    f->linesize[2] = (w + 1) / 2 + extra;
    f->w = w;
    f->h = h;
    f->keyframe = true;
}

/* 填满一整帧的单色（U/V 全填同一值），用来测"整幅同色" */
static void frame_fill(vframe_t *f, fake_t *s, uint8_t yy, uint8_t uu, uint8_t vv)
{
    int cw = (f->w + 1) / 2, ch = (f->h + 1) / 2;
    for (int y = 0; y < f->h; y++)
        memset(s->y + (size_t)y * f->linesize[0], yy, (size_t)f->w);
    for (int y = 0; y < ch; y++) {
        memset(s->u + (size_t)y * f->linesize[1], uu, (size_t)cw);
        memset(s->v + (size_t)y * f->linesize[2], vv, (size_t)cw);
    }
}

/*===========================================================================
 * 等比适配
 *=========================================================================*/
UT_TEST(yuv2rgb, fit_4x3_into_4x3)
{
    int w = 0, h = 0;
    UT_ASSERT(yuv2rgb_fit(640, 480, 288, 216, &w, &h));
    UT_EQ_INT(w, 288);
    UT_EQ_INT(h, 216);
}

UT_TEST(yuv2rgb, fit_16x9_letterbox)
{
    /* 16:9 的源装进 4:3 的框，宽度是紧约束 → 上下留黑（黑边由布局处理） */
    int w = 0, h = 0;
    UT_ASSERT(yuv2rgb_fit(640, 360, 288, 216, &w, &h));
    UT_EQ_INT(w, 288);
    UT_EQ_INT(h, 162);
}

UT_TEST(yuv2rgb, fit_tall_source)
{
    /* 竖屏源（手机前置）装进横框：高度是紧约束 */
    int w = 0, h = 0;
    UT_ASSERT(yuv2rgb_fit(480, 640, 288, 216, &w, &h));
    UT_EQ_INT(w, 162);
    UT_EQ_INT(h, 216);
}

UT_TEST(yuv2rgb, fit_identity)
{
    int w = 0, h = 0;
    UT_ASSERT(yuv2rgb_fit(288, 216, 288, 216, &w, &h));
    UT_EQ_INT(w, 288);
    UT_EQ_INT(h, 216);
}

UT_TEST(yuv2rgb, fit_upscale_allowed)
{
    /* 源比框小：放大铺满。放大不好看，但比"画面只占一角"强 */
    int w = 0, h = 0;
    UT_ASSERT(yuv2rgb_fit(160, 120, 288, 216, &w, &h));
    UT_EQ_INT(w, 288);
    UT_EQ_INT(h, 216);
}

UT_TEST(yuv2rgb, fit_never_exceeds_box)
{
    /* 常见摄像头分辨率 × 常见框尺寸，逐个确认不越界（越界会画到控件外） */
    const int srcs[][2] = {
        { 640, 480 }, { 640, 360 }, { 1280, 720 }, { 1920, 1080 },
        { 320, 240 }, { 160, 120 }, { 176, 144 }, { 1024, 768 },
        { 480, 640 }, { 720, 1280 }, { 1, 1 }, { 7, 13 },
    };
    const int boxes[][2] = { { 288, 216 }, { 320, 240 }, { 96, 96 }, { 1, 1 } };

    for (unsigned i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
        for (unsigned j = 0; j < sizeof(boxes) / sizeof(boxes[0]); j++) {
            int w = 0, h = 0;
            bool ok = yuv2rgb_fit(srcs[i][0], srcs[i][1], boxes[j][0], boxes[j][1], &w, &h);
            UT_ASSERT_MSG(ok, "fit 返回 false");
            UT_ASSERT_MSG(w >= 1 && h >= 1, "结果小于 1");
            UT_ASSERT_MSG(w <= boxes[j][0], "宽越界");
            UT_ASSERT_MSG(h <= boxes[j][1], "高越界");
        }
    }
}

UT_TEST(yuv2rgb, fit_bad_args)
{
    int w = 7, h = 7;
    UT_ASSERT(!yuv2rgb_fit(0, 480, 288, 216, &w, &h));
    UT_ASSERT(!yuv2rgb_fit(640, 0, 288, 216, &w, &h));
    UT_ASSERT(!yuv2rgb_fit(640, 480, 0, 216, &w, &h));
    UT_ASSERT(!yuv2rgb_fit(640, 480, 288, -1, &w, &h));
    UT_ASSERT(!yuv2rgb_fit(640, 480, 288, 216, NULL, &h));
    /* 失败时不能写输出参数（调用方可能拿着上次的值继续用） */
    UT_EQ_INT(w, 7);
    UT_EQ_INT(h, 7);
}

/*===========================================================================
 * 像素换算：标准色
 *=========================================================================*/
UT_TEST(yuv2rgb, px_black_is_zero)
{
    /* BT.601 有限量程的纯黑：Y=16, U=V=128 */
    UT_EQ_INT(yuv2rgb_565_px(16, 128, 128), 0x0000);
}

UT_TEST(yuv2rgb, px_white_is_max)
{
    /* 有限量程的纯白：Y=235 */
    UT_EQ_INT(yuv2rgb_565_px(235, 128, 128), 0xFFFF);
}

UT_TEST(yuv2rgb, px_red)
{
    /* 纯红在有限量程里的 YUV ≈ (82, 90, 240)，RGB565 应为 0xF800 */
    UT_EQ_INT(yuv2rgb_565_px(82, 90, 240), 0xF800);
}

UT_TEST(yuv2rgb, px_green)
{
    UT_EQ_INT(yuv2rgb_565_px(145, 54, 34), 0x07E0);
}

UT_TEST(yuv2rgb, px_blue)
{
    UT_EQ_INT(yuv2rgb_565_px(41, 240, 110), 0x001F);
}

UT_TEST(yuv2rgb, px_clamps_instead_of_wrapping)
{
    /* 亮度越界：Y=0 比有限量程的黑(16)还暗，Y=255 比白(235)还亮 —— 都必须夹住。
       （注意：越界夹的是**输出通道**，不是输入。所以这里色度填中性 128。） */
    UT_EQ_INT(yuv2rgb_565_px(0, 128, 128), 0x0000);
    UT_EQ_INT(yuv2rgb_565_px(255, 128, 128), 0xFFFF);

    /* 色度越界：会算出负数或超过 255 的通道值。
       夹住的话是对应通道饱和/归零；回绕的话会变成完全对不上的颜色。
       这正是"肤色发青""红色变黑"这类怪象的根源。 */
    uint16_t p = yuv2rgb_565_px(128, 0, 255);          /* 往红方向拉满 */
    UT_ASSERT_MSG(((p >> 11) & 0x1F) == 0x1F, "红通道未饱和");
    UT_ASSERT_MSG((p & 0x1F) == 0, "蓝通道未夹到 0（发生回绕）");

    p = yuv2rgb_565_px(128, 255, 0);                   /* 往蓝方向拉满 */
    UT_ASSERT_MSG(((p >> 11) & 0x1F) == 0, "红通道未夹到 0（发生回绕）");
    UT_ASSERT_MSG((p & 0x1F) == 0x1F, "蓝通道未饱和");
}

UT_TEST(yuv2rgb, px_gray_has_equal_channels)
{
    /* 灰阶：R=G=B，RGB565 里三个通道都应该是各自的上界（0x1F/0x3F/0x1F） */
    for (int y = 16; y <= 235; y += 1) {
        uint16_t p = yuv2rgb_565_px(y, 128, 128);
        int r = (p >> 11) & 0x1F;
        int g = (p >> 5)  & 0x3F;
        int b =  p        & 0x1F;
        int ri = r << 3, gi = g << 2, bi = b << 3;
        UT_ASSERT_MSG(ri - bi <= 8 && bi - ri <= 8, "灰阶偏色");
        UT_ASSERT_MSG(gi - bi <= 8 && bi - gi <= 8, "灰阶偏色");
    }
}

UT_TEST(yuv2rgb, px_monotonic_luma)
{
    /* 亮度必须单调：Y 越大越亮（这是"图像是否发灰/发暗"最直接的检查） */
    int prev = -1;
    for (int y = 16; y <= 235; y += 5) {
        uint16_t p = yuv2rgb_565_px(y, 128, 128);
        int lum = ((p >> 11) & 0x1F) + ((p >> 5) & 0x3F) + (p & 0x1F);
        UT_ASSERT_MSG(lum >= prev, "亮度非单调");
        prev = lum;
    }
}

/*===========================================================================
 * 整帧转换
 *=========================================================================*/
UT_TEST(yuv2rgb, frame_same_size_is_pixel_exact)
{
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 0);
    frame_fill(&f, &s, 82, 90, 240);      /* 纯红 */

    uint16_t dst[16];
    UT_ASSERT(yuv2rgb_565(&f, dst, 4, 4));
    for (int i = 0; i < 16; i++) UT_EQ_INT(dst[i], 0xF800);
}

UT_TEST(yuv2rgb, frame_center_sampling_2x_downscale)
{
    /* 4×4 → 2×2，像素中心对齐后应采到源图的 (1,1) 和 (3,3) 等位置：
         x=0 → sx=(2*0+1)*4/(2*2)=1
         x=1 → sx=(2*1+1)*4/(2*2)=3
       这条用例专治"用 x*sw/dw 而不是像素中心"导致的整体偏左上。 */
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 0);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s.y[y * f.linesize[0] + x] = (uint8_t)(16 + y * 40 + x * 4);
    /* 色度填中性，只让亮度参与判断 */
    for (int y = 0; y < 2; y++) {
        s.u[y * f.linesize[1] + 0] = s.u[y * f.linesize[1] + 1] = 128;
        s.v[y * f.linesize[2] + 0] = s.v[y * f.linesize[2] + 1] = 128;
    }

    uint16_t dst[4];
    UT_ASSERT(yuv2rgb_565(&f, dst, 2, 2));

    /* 采样点：行 sy=1,3；列 sx=1,3 → Y = 16+40*y+x*4 */
    UT_EQ_INT(dst[0], yuv2rgb_565_px(16 + 40 * 1 + 4 * 1, 128, 128));
    UT_EQ_INT(dst[1], yuv2rgb_565_px(16 + 40 * 1 + 4 * 3, 128, 128));
    UT_EQ_INT(dst[2], yuv2rgb_565_px(16 + 40 * 3 + 4 * 1, 128, 128));
    UT_EQ_INT(dst[3], yuv2rgb_565_px(16 + 40 * 3 + 4 * 3, 128, 128));
}

UT_TEST(yuv2rgb, frame_respects_linesize)
{
    /* linesize > width 是解码器的常态（为了 16/32 字节对齐）。
       不按 linesize 走会读到行尾的对齐垃圾 —— 表现是画面斜切。
       这里把填充区写成 0xFF（纯白），源像素写成纯黑；
       如果实现读错了，输出就会冒出白色。 */
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 4);          /* 每行多 4 字节 */
    memset(s.y, 0xFF, sizeof(s.y));        /* 先全刷白（含填充区） */
    memset(s.u, 0xFF, sizeof(s.u));
    memset(s.v, 0xFF, sizeof(s.v));
    frame_fill(&f, &s, 16, 128, 128);      /* 只覆盖有效区 → 纯黑 */

    uint16_t dst[16];
    UT_ASSERT(yuv2rgb_565(&f, dst, 4, 4));
    for (int i = 0; i < 16; i++) UT_EQ_INT(dst[i], 0x0000);
}

UT_TEST(yuv2rgb, frame_chroma_offset)
{
    /* U/V 的横坐标必须是 sx/2 —— 直接拿 sx 索引会读到隔壁像素的颜色，
       表现是"颜色横向串位"（人脸边缘出现彩色描边）。 */
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 0);
    frame_fill(&f, &s, 128, 128, 128);
    /* 左半色度 = 红，右半 = 蓝 */
    for (int y = 0; y < 2; y++) {
        s.u[y * f.linesize[1] + 0] = 90;    /* 左列 → 红方向 */
        s.v[y * f.linesize[2] + 0] = 240;
        s.u[y * f.linesize[1] + 1] = 240;   /* 右列 → 蓝方向 */
        s.v[y * f.linesize[2] + 1] = 110;
    }

    uint16_t dst[16];
    UT_ASSERT(yuv2rgb_565(&f, dst, 4, 4));
    /* 前两列走左半色度，后两列走右半 */
    UT_EQ_INT(dst[0], yuv2rgb_565_px(128, 90, 240));
    UT_EQ_INT(dst[1], yuv2rgb_565_px(128, 90, 240));
    UT_EQ_INT(dst[2], yuv2rgb_565_px(128, 240, 110));
    UT_EQ_INT(dst[3], yuv2rgb_565_px(128, 240, 110));
}

UT_TEST(yuv2rgb, frame_does_not_write_past_dst)
{
    /* 缓冲越界在上屏这一路最难查：会静默改掉别的槽位数据，
       表现是"偶尔闪一下别的画面"。这里用哨兵值兜住。 */
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 0);
    frame_fill(&f, &s, 82, 90, 240);

    uint16_t buf[16 + 4];
    for (int i = 0; i < 20; i++) buf[i] = 0xBEEF;

    UT_ASSERT(yuv2rgb_565(&f, buf, 4, 4));
    for (int i = 16; i < 20; i++) UT_EQ_INT(buf[i], 0xBEEF);
}

UT_TEST(yuv2rgb, frame_rejects_bad_args)
{
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 4, 4, 0);
    frame_fill(&f, &s, 82, 90, 240);

    uint16_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = 0x1234;

    UT_ASSERT(!yuv2rgb_565(NULL, buf, 4, 4));
    UT_ASSERT(!yuv2rgb_565(&f, NULL, 4, 4));
    UT_ASSERT(!yuv2rgb_565(&f, buf, 0, 4));
    UT_ASSERT(!yuv2rgb_565(&f, buf, 4, -1));
    /* 参数非法时一个字节都不能写 */
    for (int i = 0; i < 16; i++) UT_EQ_INT(buf[i], 0x1234);

    /* 平面指针缺失也要拒绝，不能崩 */
    vframe_t bad = f;
    bad.plane[1] = NULL;
    UT_ASSERT(!yuv2rgb_565(&bad, buf, 4, 4));
}

UT_TEST(yuv2rgb, frame_upscale_to_box)
{
    /* 2×2 放大到 288×216（模拟小图铺满框），只检查不崩、不越界、颜色对 */
    fake_t s;
    vframe_t f;
    frame_init(&f, &s, 2, 2, 0);
    frame_fill(&f, &s, 16, 128, 128);      /* 纯黑 */

    enum { DW = 288, DH = 216 };
    static uint16_t dst[DW * DH];
    UT_ASSERT(yuv2rgb_565(&f, dst, DW, DH));
    UT_EQ_INT(dst[0], 0x0000);
    UT_EQ_INT(dst[DW * DH - 1], 0x0000);
}
