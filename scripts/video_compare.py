#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""逐帧比对两段 YUV420P 裸流，回答"室内机解出来的画面和参考是不是同一幅"。

判据分两档，这个区分很重要：

    bit-exact（逐字节相同）
        同一个解码器（libavcodec）解同一段码流，H.264 是**确定性**的 ——
        输出必须一字节不差。不相同就说明链路里有问题（重组错、丢包、
        喂进去的 AU 边界不对），不存在"正常误差"这种解释。
        这是 PC 端的合格标准。

    PSNR ≥ 35 dB
        板端 MPP 硬解与软解的运动补偿实现不同，逐字节不可能相同。
        35dB 以上人眼看不出差别，是"解码正确"的合理标准。
        低于 30dB 就要怀疑色彩空间或 stride 处理错了。

为什么不用"肉眼看着差不多"当判据：
    花屏、绿块、错位一格、色彩偏移 —— 这几种在 7 寸屏上"都挺像"，
    而 PSNR 会从 40dB 直接掉到 12dB。数字比眼睛可靠。

用法：
    python scripts/video_compare.py ref.yuv rx.yuv --size 640x480
    python scripts/video_compare.py ref.yuv rx.yuv --size 640x480 --align
    python scripts/video_compare.py ref.yuv rx.yuv --size 640x480 --max-frames 100

    --align   接收流可能不是从第 0 帧开始（中途开始推流/丢首帧），
              用它拿接收端第一帧在参考里找位置。
"""
import os
import sys

# 每帧最多采样这么多个字节来估 MSE。纯 Python 逐字节算 138MB 要好几分钟，
# 采样 20 万点算出来的 PSNR 误差在 0.1dB 以内 —— 对"是不是同一个画面"足够了。
MAX_SAMPLES = 200000

PSNR_PASS = 35.0


def frame_size(w, h):
    return w * h * 3 // 2


def read_frames(path, w, h, max_frames=None):
    """把裸 YUV 读成帧列表。返回 (frames, total_size)。"""
    fs = frame_size(w, h)
    total = os.path.getsize(path)
    n = total // fs
    if max_frames is not None:
        n = min(n, max_frames)
    frames = []
    with open(path, "rb") as f:
        for i in range(n):
            d = f.read(fs)
            if len(d) < fs:
                break
            frames.append(d)
    return frames, total


def psnr(a, b):
    """返回 (psnr_db, mse, exact)。exact=True 时 psnr 为 inf。"""
    if a == b:
        return float("inf"), 0.0, True

    n = len(a)
    step = max(1, n // MAX_SAMPLES)
    sa = a[::step]
    sb = b[::step]
    m = len(sa)
    if m == 0:
        return float("inf"), 0.0, True

    se = 0
    for x, y in zip(sa, sb):
        d = x - y
        se += d * d
    mse = se / m
    if mse <= 0:
        # 采样点全相同但整体不同：极少见（差异落在未采样的位置），
        # 给一个很高的 PSNR 而不是 inf，避免谎报 bit-exact
        return 99.0, 0.0, False
    return 10.0 * __import__("math").log10(255.0 * 255.0 / mse), mse, False


def fingerprint(frame, w, h, points=256):
    """Y 平面上的稀疏采样，用于在参考里找对齐位置。"""
    stride = max(1, (w * h) // points)
    return frame[: w * h][::stride][:points]


def find_alignment(ref_frames, first_frame, w, h):
    """返回 ref 里与 first_frame 最匹配的帧号，找不到返回 -1。"""
    if not ref_frames:
        return -1
    fp = fingerprint(first_frame, w, h)
    best_i, best_d = -1, None
    for i, fr in enumerate(ref_frames):
        fpr = fingerprint(fr, w, h)
        if len(fpr) != len(fp):
            continue
        d = sum(abs(x - y) for x, y in zip(fp, fpr))
        if best_d is None or d < best_d:
            best_d, best_i = d, i
    # 阈值放宽：对齐只用来定位，真正的判据还是逐帧 PSNR
    return best_i if best_d is not None and best_d < len(fp) * 6 else -1


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        print(__doc__)
        return 2

    ref_path, rx_path = args[0], args[1]
    w = h = 0
    align = False
    max_frames = None
    for i, a in enumerate(args):
        if a == "--size" and i + 1 < len(args):
            w, h = (int(x) for x in args[i + 1].lower().split("x"))
        elif a == "--align":
            align = True
        elif a == "--max-frames" and i + 1 < len(args):
            max_frames = int(args[i + 1])

    if not w or not h:
        print("必须用 --size WxH 指定分辨率（裸 YUV 里没有元信息）")
        return 2

    for p in (ref_path, rx_path):
        if not os.path.exists(p):
            print(f"找不到文件: {p}")
            return 2

    ref, ref_sz = read_frames(ref_path, w, h, max_frames)
    rx, rx_sz = read_frames(rx_path, w, h, max_frames)

    print(f"参考: {ref_path}  {ref_sz/1048576:.1f} MB  {len(ref)} 帧 ({w}x{h})")
    print(f"实测: {rx_path}  {rx_sz/1048576:.1f} MB  {len(rx)} 帧 ({w}x{h})")

    if not ref:
        print("参考流是空的")
        return 1
    if not rx:
        print("❌ 实测流一帧都没有 —— 收端根本没解出画面")
        return 1

    offset = 0
    if align:
        idx = find_alignment(ref, rx[0], w, h)
        if idx < 0:
            print("❌ 对齐失败：接收端第一帧在参考里找不到对应位置")
            print("   （要么画面真的完全不同，要么分辨率先就不对）")
            return 1
        offset = idx
        print(f"对齐: 实测第 0 帧 ↔ 参考第 {idx} 帧")

    n = min(len(ref) - offset, len(rx))
    if n <= 0:
        print("❌ 没有可比对的重叠区间")
        return 1

    exact = 0
    worst = (float("inf"), -1)
    fails = []
    total_psnr = 0.0
    counted = 0

    for i in range(n):
        p, mse, is_exact = psnr(rx[i], ref[i + offset])
        if is_exact:
            exact += 1
        else:
            if p < worst[0]:
                worst = (p, i)
            if p < PSNR_PASS:
                fails.append((i + offset, p))
        if p != float("inf"):
            total_psnr += p
            counted += 1

    print()
    print(f"比对 {n} 帧")
    print(f"  逐字节相同   {exact}/{n}" + ("   ← 全部 bit-exact" if exact == n else ""))
    if counted:
        print(f"  平均 PSNR    {total_psnr/counted:.2f} dB")
        print(f"  最差 PSNR    {worst[0]:.2f} dB（第 {worst[1] + offset} 帧）")

    if fails:
        print()
        print(f"  ⚠ {len(fails)} 帧低于 {PSNR_PASS:.0f} dB，前 10 个：")
        for fr, p in fails[:10]:
            print(f"      第 {fr:4d} 帧  {p:6.2f} dB")
        if len(fails) > 10:
            print(f"      …其余 {len(fails) - 10} 帧略")

    print()
    if exact == n:
        print("✅ 全部帧 bit-exact：解码链路完全正确（重组/解码/落盘都没有引入任何偏差）")
        return 0
    if counted and worst[0] >= PSNR_PASS:
        print(f"✅ 最差帧 {worst[0]:.2f} dB ≥ {PSNR_PASS:.0f} dB：解码正确（差异是不同实现带来的）")
        return 0

    print(f"❌ 有 {len(fails)} 帧低于 {PSNR_PASS:.0f} dB")
    print("   常见原因：")
    print("     · FU-A 分片丢了 → 整帧花屏（看 [video] stats 里的 fu_broken）")
    print("     · AU 边界切错 → 帧序错位（每帧都差一点，PSNR 普遍 20~28dB）")
    print("     · 落盘没按 linesize 逐行拷 → 画面整体斜切（PSNR 极低但帧数正常）")
    print("     · 参考流和实测流分辨率不同 → 这里应该早就报错了")
    return 1


if __name__ == "__main__":
    sys.exit(main())
