#!/usr/bin/env python3
"""
上屏链路的数值验收：抓屏 PPM vs 参考 YUV 帧，逐像素比。

【为什么要这么麻烦】
  视频上屏"看起来对"几乎没有说服力。同一块 320x480 小屏上，
  下面这几种故障都长得像"信号不好"：
     - 花屏/马赛克（FU-A 丢片）
     - 整体偏绿或偏紫（U/V 平面串了）
     - 画面纵向翻转、左右错位（linesize 没按行走）
     - 只画了半边 / 底部一条黑（缩放算错）
     - 颜色发灰（把有限量程当全量程）
  人眼很难区分，尤其只瞄一眼的时候。所以走"抓屏 → 逐像素比"：
  **只要有一个像素不同就能指出来，并且在同一个坐标上说清差多少。**

【为什么能在 565 空间做到逐像素相同】
  链路是：解码器 YUV420P → 本工程 yuv2rgb 缩放 → RGB565 → LVGL 画到屏幕
        → 抓屏回来再量化回 RGB565。
  中间"565 → 屏幕(888) → 565"这一步是可逆的：5 位 → 8 位用位复制补低位，
  再 >>3 就精确回到原值（6 位同理）。所以只要缩放规则一致、没有额外的滤波/
  dithering，逐像素必须完全相同。不同 → 一定有具体问题，不是"误差"。

【缩放规则必须和 C 侧一模一样】
  最近邻 + 像素中心对齐：
      sx = (2*x + 1) * sw // (2 * dw)
  用最近邻而不是双线性，就是为了能在这里逐字节复现 —— 双线性会引入
  浮点舍入差异，比对时说不清是谁的错。

用法：
    python scripts/disp_compare.py --log bin/duo_disp_b.log --ref bin/ref_640x480.yuv
    python scripts/disp_compare.py --shot s.ppm --ref ref.yuv --src 640x480 --frame 30 --box 12,12,288,216
    python scripts/disp_compare.py ... --dump-diff diff.ppm     # 把差异画出来看

退出码：0 通过 / 1 不通过 / 2 参数或读取错误
"""
import argparse
import re
import sys
from pathlib import Path

PSNR_PASS = 35.0          # 与 video_compare.py 同一口径
MAX_REPORT_PIXELS = 12    # 失败时最多列这么多个像素，够定位就行


# ---------------------------------------------------------------------------
# 读输入
# ---------------------------------------------------------------------------
def parse_log(path):
    """从程序日志里抠出抓屏信息：[disp] 抓屏 → p  screen=WxH  img_box=x,y,w,h  dec_frame=N"""
    text = Path(path).read_text(encoding="utf-8", errors="ignore")
    pat = (r"\[disp\]\s*抓屏\s*→\s*(\S+)\s+screen=(\d+)x(\d+)\s+"
           r"img_box=(-?\d+),(-?\d+),(\d+),(\d+)\s+dec_frame=(\d+)")
    # 取**最后一条**：逻辑上只会抓一次，但万一被重复触发，落盘文件是最后一次写的，
    # 那就必须用最后一次的帧号去对，否则"帧号对、文件不对"，结果毫无意义。
    ms = list(re.finditer(pat, text))
    if not ms:
        raise SystemExit(f"日志里没找到抓屏记录（{path}）—— SIP_VIDEO_DISP_SHOT 设了吗？")
    m = ms[-1]
    if len(ms) > 1:
        print(f"⚠️  日志里有 {len(ms)} 条抓屏记录，取最后一条（落盘文件以最后一次为准）")
    return {
        "shot": m.group(1),
        "screen": (int(m.group(2)), int(m.group(3))),
        "box": (int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7))),
        "frame": int(m.group(8)),
    }


def read_ppm(path):
    """只支持 P6（二进制 8 位）。程序里就是按这个格式写的，不需要通用解析器。"""
    data = Path(path).read_bytes()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{path} 不是 P6 PPM")
    # 三段头部：magic / 尺寸 / 最大值，中间可能夹注释（本工程不写，但容错）
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1  # 跳掉 maxval 后面那一个换行
    w, h, maxv = fields
    if maxv != 255:
        raise SystemExit(f"只支持 8 位通道，读到 maxval={maxv}")
    px = data[i:i + w * h * 3]
    if len(px) < w * h * 3:
        raise SystemExit(f"PPM 数据不完整：期望 {w * h * 3} 字节，实际 {len(px)}")
    return w, h, px


def read_yuv_frame(path, w, h, idx):
    """取裸 YUV420P 的第 idx 帧（1 基）。整帧 = Y(w*h) + U(w/2*h/2) + V(w/2*h/2)"""
    fs = w * h * 3 // 2
    off = (idx - 1) * fs
    size = Path(path).stat().st_size
    total = size // fs
    if idx < 1 or idx > total:
        raise SystemExit(f"参考帧 #{idx} 超出范围（文件共 {total} 帧，{size} 字节）")
    with open(path, "rb") as f:
        f.seek(off)
        buf = f.read(fs)
    cw, ch = w // 2, h // 2
    Y = buf[:w * h]
    U = buf[w * h: w * h + cw * ch]
    V = buf[w * h + cw * ch:]
    return Y, U, V, total


# ---------------------------------------------------------------------------
# 与 C 侧逐位一致的换算
# ---------------------------------------------------------------------------
def clamp255(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def yuv_to_565(y, u, v):
    """和 src/media/yuv2rgb.c 的 yuv2rgb_565_px 完全一致（含整数右移的向下取整）"""
    c = y - 16
    d = u - 128
    e = v - 128
    r = clamp255((298 * c + 409 * e + 128) >> 8)
    g = clamp255((298 * c - 100 * d - 208 * e + 128) >> 8)
    b = clamp255((298 * c + 516 * d + 128) >> 8)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def scale_to_565(Y, U, V, sw, sh, dw, dh):
    """最近邻缩放 + 565 量化，返回 list[int]（长度 dw*dh）"""
    cw = (sw + 1) // 2
    out = [0] * (dw * dh)
    for y in range(dh):
        sy = ((2 * y + 1) * sh) // (2 * dh)
        if sy >= sh:
            sy = sh - 1
        yrow = sy * sw
        crow = (sy >> 1) * cw
        orow = y * dw
        for x in range(dw):
            sx = ((2 * x + 1) * sw) // (2 * dw)
            if sx >= sw:
                sx = sw - 1
            out[orow + x] = yuv_to_565(Y[yrow + sx], U[crow + (sx >> 1)], V[crow + (sx >> 1)])
    return out


def ppm_to_565(px, sw, sx0, sy0, w, h):
    """从整屏 PPM 里裁出 w×h 的子图，量化回 565"""
    out = [0] * (w * h)
    for y in range(h):
        base = ((sy0 + y) * sw + sx0) * 3
        for x in range(w):
            o = base + x * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            out[y * w + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return out


def psnr_from_565(a, b, w):
    """在 565 展开成 888 的亮度上算 PSNR —— 只在"不相同"时用于给个量级判断"""
    import math
    se = 0
    n = 0
    for i, (p, q) in enumerate(zip(a, b)):
        for shift, mask, bits in ((11, 0x1F, 5), (5, 0x3F, 6), (0, 0x1F, 5)):
            va = ((p >> shift) & mask) << (8 - bits)
            vb = ((q >> shift) & mask) << (8 - bits)
            d = va - vb
            se += d * d
            n += 1
    if se == 0:
        return 99.0
    mse = se / n if n else 0
    if mse == 0:
        return 99.0
    return 10.0 * math.log10(255.0 * 255.0 / mse)


def write_diff(path, ref, shot, w, h):
    """把差异画成 PPM：相同→黑，不同→白（直观看出"差在哪一片"）"""
    buf = bytearray(w * h * 3)
    for i in range(w * h):
        v = 255 if ref[i] != shot[i] else 0
        buf[i * 3:i * 3 + 3] = bytes((v, v, v))
    Path(path).write_bytes(b"P6\n%d %d\n255\n" % (w, h) + bytes(buf))


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="上屏链路的逐像素验收")
    ap.add_argument("--log", help="程序日志（自动抠抓屏路径/区域/帧号）")
    ap.add_argument("--shot", help="抓屏 PPM")
    ap.add_argument("--ref", help="参考裸 YUV420P")
    ap.add_argument("--src", help="参考帧分辨率，如 640x480")
    ap.add_argument("--frame", type=int, help="参考帧序号（1 基）")
    ap.add_argument("--box", help="画面在屏幕上的位置 x,y,w,h")
    ap.add_argument("--dump-diff", help="把差异另存为 PPM")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    info = {}
    if args.log:
        info = parse_log(args.log)
    shot = args.shot or info.get("shot")
    frame = args.frame or info.get("frame")
    src = args.src
    box = args.box or (",".join(str(v) for v in info["box"]) if info.get("box") else None)

    if not (shot and args.ref and src and frame and box):
        ap.error("需要 --ref 与 --src；抓屏信息可以来自 --log 或直接给 --shot/--frame/--box")

    sw, sh = (int(v) for v in src.lower().split("x"))
    bx, by, bw, bh = (int(v) for v in box.replace(" ", "").split(","))

    Y, U, V, total = read_yuv_frame(args.ref, sw, sh, frame)
    ref565 = scale_to_565(Y, U, V, sw, sh, bw, bh)

    pw, ph, px = read_ppm(shot)
    if info.get("screen") and (pw, ph) != info["screen"]:
        print(f"⚠️  抓屏尺寸 {pw}x{ph} 与日志声明 {info['screen'][0]}x{info['screen'][1]} 不符")
    if bx < 0 or by < 0 or bx + bw > pw or by + bh > ph:
        raise SystemExit(f"画面区域 {bx},{by},{bw},{bh} 超出抓屏范围 {pw}x{ph}")
    shot565 = ppm_to_565(px, pw, bx, by, bw, bh)

    same = sum(1 for a, b in zip(ref565, shot565) if a == b)
    total_px = bw * bh
    diff = total_px - same

    print(f"参考帧         #{frame}（参考文件共 {total} 帧）")
    print(f"缩放           {sw}x{sh} → {bw}x{bh}（最近邻，像素中心对齐）")
    print(f"抓屏区域       屏幕({bx},{by}) {bw}x{bh}，整屏 {pw}x{ph}")
    print(f"逐像素相同     {same}/{total_px}", end="")
    if diff:
        worst = max(abs(((a >> s) & m) - ((b >> s) & m))
                    for a, b in zip(ref565, shot565)
                    for s, m in ((11, 0x1F), (5, 0x3F), (0, 0x1F)))
        p = psnr_from_565(ref565, shot565, bw)
        print(f"    不同 {diff} 个    最大通道差 {worst}    PSNR {p:.1f} dB")
    else:
        print("    ← 完全一致")

    if args.dump_diff and diff:
        write_diff(args.dump_diff, ref565, shot565, bw, bh)
        print(f"差异图         {args.dump_diff}（白=不同，黑=相同）")

    if diff == 0:
        print("\n✅ 上屏链路逐像素正确：解码 → 缩放 → RGB565 → LVGL 绘制 全部对得上")
        return 0

    if not args.quiet:
        print(f"\n前 {min(diff, MAX_REPORT_PIXELS)} 个不同的像素：")
        shown = 0
        for i, (a, b) in enumerate(zip(ref565, shot565)):
            if a == b:
                continue
            x, y = i % bw, i // bw
            print(f"  ({x:3d},{y:3d})  参考 {a:04X}  实得 {b:04X}"
                  f"  R{((a >> 11) & 0x1F):02d}/{((b >> 11) & 0x1F):02d}"
                  f" G{((a >> 5) & 0x3F):02d}/{((b >> 5) & 0x3F):02d}"
                  f" B{(a & 0x1F):02d}/{(b & 0x1F):02d}")
            shown += 1
            if shown >= MAX_REPORT_PIXELS:
                break

        # 差异形态 → 病因。这几条是按"看过实际故障"写的，不是穷举。
        rows = sorted({i // bw for i, (a, b) in enumerate(zip(ref565, shot565)) if a != b})
        cols = sorted({i % bw for i, (a, b) in enumerate(zip(ref565, shot565)) if a != b})
        print("\n差异形态判读：")
        if len(rows) == bh and len(cols) == bw:
            print("  · 几乎全屏都不同 → 缩放比例/目标尺寸不对，或抓错了帧（差一帧）")
        elif len(rows) <= 2 and len(cols) == bw:
            print("  · 只有个别整行不同 → 行步长(linesize/stride)有误")
        elif len(cols) <= 2 and len(rows) == bh:
            print("  · 只有个别整列不同 → 横向采样错位（sx 公式错了）")
        elif max(rows) >= bh - 2:
            print("  · 差异集中在底部 → 只画了一部分，或高度算少了几行")
        else:
            print("  · 零散分布 → 可能是 RGB565↔888 的转换不是无损往返，或抓屏时机早了一帧")

    p = psnr_from_565(ref565, shot565, bw)
    ok = p >= PSNR_PASS
    print(f"\n{'✅ 通过' if ok else '❌ 不通过'}（PSNR {p:.1f} dB，阈值 {PSNR_PASS:.0f} dB）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
