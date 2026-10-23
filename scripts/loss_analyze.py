#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
丢包恢复验收：逐帧 CRC 核对 + 恢复时序统计

为什么不用"逐帧对齐比像素"（duo_video_test 用的那套）：
   丢包恢复会**故意跳帧** —— 落盘文件的第 N 帧早就不对应参考文件的第 N 帧了，
   按位置比会从丢包点开始整体错位，看到满屏差异，但它其实一帧都没错。
   要验的东西也变了：不再问"第 N 帧对不对"，而是问
   **"写出去的每一帧，是不是都完好"**。所以改用内容指纹（CRC）：
   只要每一帧的 CRC 都能在参考集的指纹表里命中，就证明它逐字节等同某个原始帧；
   被污染的解码结果 CRC 一定落在集合外 —— 这是单向的，不存在"侥幸命中"。

判据（三条，缺一不可）：
   1) 脏帧 = 0        —— 没有任何一帧是被破坏后又拿去显示/落盘的
   2) 有跳帧          —— 证明真的丢了数据，而不是注入器根本没生效（假绿）
   3) 参考帧号单调    —— 帧顺序不能乱（乱序说明解包层在拼错误的东西）

关掉恢复（SIP_VIDEO_RESYNC=0）跑同一个注入，脏帧应当 > 0。
这一正一反才是完整的证明：既证明"恢复有效"，也证明"没有恢复就会坏"。

用法：
  python scripts/loss_analyze.py --log loss_on.log --framelog loss_on.crc \
         --ref bin/ref_640x480.yuv --size 640x480
"""

import argparse
import re
import sys
import zlib


def parse_size(s):
    try:
        w, h = s.lower().split("x")
        return int(w), int(h)
    except ValueError:
        sys.exit(f"--size 格式应为 640x480，收到 {s!r}")


def load_ref_crcs(path, w, h):
    """把参考 YUV 按帧切开算 CRC，返回 {crc: [帧号...]}"""
    frame_bytes = w * h * 3 // 2
    table = {}
    n = 0
    with open(path, "rb") as f:
        while True:
            buf = f.read(frame_bytes)
            if len(buf) < frame_bytes:
                break
            table.setdefault(zlib.crc32(buf), []).append(n)
            n += 1
    return table, n


def load_frame_log(path):
    """读 app_video 写的帧日志：序号 宽 高 crc"""
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            try:
                rows.append((int(parts[0]), int(parts[1]), int(parts[2]),
                             int(parts[3], 16)))
            except ValueError:
                continue
    return rows


def parse_resync(text):
    """从室内机日志里取恢复时序"""
    info = {"starts": [], "dones": [], "stats": None}
    for m in re.finditer(r"\[video\] resync start: gap=(\d+) audrop=(\d+)", text):
        info["starts"].append((int(m.group(1)), int(m.group(2))))
    for m in re.finditer(r"\[video\] resync done: skipped=(\d+) waited=(\d+)ms", text):
        info["dones"].append((int(m.group(1)), int(m.group(2))))

    m = re.search(
        r"\[video\] resync stats\(\w*\) 事件=(\d+) 恢复=(\d+) 跳过帧=(\d+) "
        r"最坏等待=(\d+)ms 放弃=(\d+) PLI请求=(\d+)", text)
    if m:
        info["stats"] = {
            "events":   int(m.group(1)),
            "resumed":  int(m.group(2)),
            "skipped":  int(m.group(3)),
            "max_wait": int(m.group(4)),
            "gaveup":   int(m.group(5)),
            "pli":      int(m.group(6)),
        }
    m = re.search(r"\[rtp\] video stats\(\w*\) pkt=(\d+) pt_bad=(\d+) "
                  r"last_ts=(\d+) pli=(\d+)(?: nack=(\d+))?", text)
    if m:
        info["rtp_pkt"] = int(m.group(1))
        info["rtp_pli"] = int(m.group(4))
        info["rtp_nack"] = int(m.group(5)) if m.group(5) else 0

    # 重排/NACK 层（rtp_reorder）：filled=重传补上的包，gap=判丢交给慢路径的包
    m = re.search(r"\[video\] ro stats\(\w*\) filled=(\d+) gap=(\d+) nack=(\d+) "
                  r"dup=(\d+) late=(\d+) full=(\d+) big=(\d+) reset=(\d+)", text)
    if m:
        info["ro"] = {
            "filled": int(m.group(1)),
            "gap":    int(m.group(2)),
            "nack":   int(m.group(3)),
            "dup":    int(m.group(4)),
            "late":   int(m.group(5)),
            "full":   int(m.group(6)),
            "big":    int(m.group(7)),
            "reset":  int(m.group(8)),
        }

    # 汇总行可能缺失：进程被强杀时，最后几行可能还在缓冲区里没落盘
    # （stdout 重定向到文件时是全缓冲 —— 已在 main.c 改成行缓冲，
    #   但分析器不能假定那一行一定在，否则会把"统计丢失"误判成"没丢包"）。
    # 单条 start/done 是逐次打印的，可靠性更高，缺汇总行就用它们现算。
    if info["stats"] is None and info["starts"]:
        info["stats"] = {
            "events":   len(info["starts"]),
            "resumed":  len(info["dones"]),
            "skipped":  sum(d[0] for d in info["dones"]),
            "max_wait": max((d[1] for d in info["dones"]), default=0),
            "gaveup":   0,
            "pli":      0,
            "synthesized": True,
        }
    return info


def main():
    ap = argparse.ArgumentParser(description="丢包恢复验收")
    ap.add_argument("--log", required=True, help="室内机日志")
    ap.add_argument("--framelog", required=True, help="帧 CRC 日志（SIP_VIDEO_FRAME_LOG）")
    ap.add_argument("--ref", required=True, help="参考 YUV（YUV420P 裸流）")
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--mode", choices=["on", "off", "nack"], default="on",
                    help="on=开了恢复（判据严格）；off=对照实验（期望有脏帧）；"
                         "nack=重传补洞（0 脏帧且全程无感）")
    ap.add_argument("--min-events", type=int, default=1,
                    help="至少应有的丢包事件数（等于注入的丢包次数）")
    ap.add_argument("--max-diff", type=int, default=12,
                    help="最多打印这么多条异常明细")
    args = ap.parse_args()

    w, h = parse_size(args.size)

    ref_table, ref_n = load_ref_crcs(args.ref, w, h)
    if ref_n == 0:
        sys.exit(f"参考 YUV 读不出整帧（{args.ref}，期望尺寸 {w}x{h}）")

    rows = load_frame_log(args.framelog)
    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        log_text = f.read()
    info = parse_resync(log_text)

    print("──── 参考集 ────")
    uniq = sum(1 for v in ref_table.values() if len(v) == 1)
    print(f"参考 {ref_n} 帧，唯一指纹 {len(ref_table)} 个"
          f"（其中 {uniq} 个是单帧指纹，{len(ref_table) - uniq} 个有重复帧）")
    if len(ref_table) < ref_n:
        print("  ⚠ 参考里有完全相同的帧，CRC 无法区分它们 —— 不影响"
              "\"脏/不脏\"的判断，只影响跳帧位置的精确度")

    print()
    print("──── 逐帧核对 ────")
    if not rows:
        print("❌ 帧日志是空的 —— SIP_VIDEO_FRAME_LOG 没生效，或者一帧都没解出来")
        return 1

    dirty = []
    ref_seq = []
    for idx, fw, fh, crc in rows:
        hit = ref_table.get(crc)
        if hit is None:
            dirty.append((idx, fw, fh, crc))
        else:
            ref_seq.append(hit[0])

    print(f"写入 {len(rows)} 帧：干净 {len(rows) - len(dirty)}，脏 {len(dirty)}")

    # --- 顺序与跳帧 ---
    jumps = 0
    backwards = []
    biggest = (0, 0, 0)          # (跳过的帧数, 从, 到)
    for i in range(1, len(ref_seq)):
        d = ref_seq[i] - ref_seq[i - 1]
        if d == 1:
            continue
        if d <= 0:
            backwards.append((i, ref_seq[i - 1], ref_seq[i]))
            continue
        jumps += 1
        if d - 1 > biggest[0]:
            biggest = (d - 1, ref_seq[i - 1], ref_seq[i])

    if ref_seq:
        print(f"参考帧号 {ref_seq[0]} → {ref_seq[-1]}，中途跳过 {jumps} 段")
        if biggest[0]:
            print(f"  最大一次跳帧：跳过 {biggest[0]} 帧"
                  f"（参考帧 {biggest[1]} → {biggest[2]}）")

    if dirty:
        print()
        print(f"❌ 脏帧 {len(dirty)} 个（CRC 落在参考集外 = 解码输出已被破坏）：")
        for idx, fw, fh, crc in dirty[:args.max_diff]:
            print(f"   写出的第 {idx} 帧  {fw}x{fh}  crc={crc:08x}")
        if len(dirty) > args.max_diff:
            print(f"   …（还有 {len(dirty) - args.max_diff} 个）")
        print("   这一帧如果送上屏，用户看到的就是花屏/错位/色块。")

    if backwards:
        print()
        print(f"⚠ 参考帧号出现回退 {len(backwards)} 处（正常应当严格递增）：")
        for pos, a, b in backwards[:args.max_diff]:
            print(f"   第 {pos} 个写入帧：{a} → {b}")

    # --- 恢复时序 ---
    print()
    print("──── 恢复时序 ────")
    if info["stats"]:
        s = info["stats"]
        print(f"丢包事件 {s['events']} 次，成功恢复 {s['resumed']} 次，"
              f"跳过 {s['skipped']} 帧，最坏等待 {s['max_wait']} ms，"
              f"放弃 {s['gaveup']} 次，PLI {s['pli']} 次")
        if s.get("synthesized"):
            print("  （汇总行缺失，上面这几个数是从逐次 start/done 现算的）")
    else:
        print("（日志里没有 resync 记录 —— 恢复没启用？）")
    for i, (sk, wt) in enumerate(info["dones"], 1):
        print(f"  第 {i} 次恢复：跳过 {sk} 帧，耗时 {wt} ms")

    if info.get("rtp_pli"):
        print(f"RTP 侧记录的 PLI 实际发出 {info['rtp_pli']} 个")
    if info.get("rtp_nack"):
        print(f"RTP 侧记录的 NACK 实际发出 {info['rtp_nack']} 个")
    if info.get("ro"):
        r = info["ro"]
        print(f"重排/NACK 层：重传补上 {r['filled']} 包，判丢 {r['gap']} 包，"
              f"请求 {r['nack']} 次，重复 {r['dup']}，迟到 {r['late']}，"
              f"满弃 {r['full']}，流重启 {r['reset']}")

    # --- 判据 ---
    print()
    print("──── 判定 ────")
    ok = True
    events = info["stats"]["events"] if info["stats"] else 0

    def chk(cond, good, bad):
        nonlocal ok
        print(("  ✅ " if cond else "  ❌ ") + (good if cond else bad))
        if not cond:
            ok = False

    if args.mode == "nack":
        # NACK 生效的标志：丢的包全被重传补上 —— 慢路径（resync/PLI）根本不该被惊动
        ro = info.get("ro")
        chk(ro is not None, "日志里有重排/NACK 统计", "没有 ro 统计 —— 重排缓冲没开？")
        if ro:
            chk(ro["filled"] >= args.min_events,
                f"重传补上了 {ro['filled']} 个包（注入生效且 NACK 闭环成功）",
                f"只补上 {ro['filled']} 包，少于注入的 {args.min_events} 段 —— "
                f"NACK 闭环没走通（代理没收到 NACK？缓存没命中？）")
            chk(ro["gap"] == 0,
                "没有包被判丢（20ms 窗口内全部补上，慢路径零惊动）",
                f"{ro['gap']} 个包被判丢 —— 重传比 hold 窗口慢，退到了 PLI 路径")
            chk(events == 0,
                "resync 慢路径全程没被触发（单包丢失无感）",
                f"resync 触发了 {events} 次 —— NACK 没能拦住丢包")
        chk(len(dirty) == 0, "写出的每一帧都完好（0 个脏帧）",
            f"{len(dirty)} 个脏帧被写出去")
        chk(not backwards, "帧号严格递增（没有拼错顺序）",
            "帧号有回退，解包顺序有问题")
        chk(jumps == 0,
            "参考帧号连续（每一帧都在 —— 用户全程无感）",
            f"中途跳过 {jumps} 段帧 —— 有帧没能救回来，走了跳帧")
    elif args.mode == "on":
        chk(len(dirty) == 0, "写出的每一帧都完好（0 个脏帧）",
            f"{len(dirty)} 个脏帧被写出去 —— 恢复策略没拦住被污染的帧")
        chk(events >= args.min_events,
            f"确实丢了 {events} 次包（注入生效，不是假绿）",
            f"只记录到 {events} 次丢包，少于注入的 {args.min_events} 次 —— "
            f"注入器可能没生效，这一轮的\"全对\"没有意义")
        chk(jumps >= min(events, 1) if events else False,
            f"中途跳过 {jumps} 段帧（说明等 IDR 期间确实停了解码）",
            "没有跳帧 —— 恢复流程可能没走到\"停手\"那一步")
        chk(not backwards, "帧号严格递增（没有拼错顺序）",
            "帧号有回退，解包顺序有问题")
        resumed = info["stats"]["resumed"] if info["stats"] else 0
        chk(resumed >= events, f"{resumed} 次全部等到 IDR 恢复",
            f"{events} 次丢包只恢复了 {resumed} 次（其余超时放弃）")
    else:
        # 对照组：关掉恢复后，丢包必须造成脏帧。若一个脏帧都没有，
        # 说明"注入 + 解码"这条链根本没把错误传下去，那么第一轮的全绿也说明不了什么。
        chk(len(dirty) > 0,
            f"关掉恢复后出现 {len(dirty)} 个脏帧 —— 这就是开恢复要消除的东西",
            "关掉恢复也没有脏帧：可能丢的包恰好不在参考链上（比如丢了 SEI），"
            "换个注入位置再试")

    print()
    print("✅ 通过" if ok else "❌ 未通过")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
