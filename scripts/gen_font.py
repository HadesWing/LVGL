#!/usr/bin/env python3
"""
生成中文子集字体（M4）。

为什么不用 LVGL 内置的 lv_font_source_han_sans_sc_16_cjk：
  内置字库为了控制体积只收了约 1500 个常用字，"灯/调/风/插/线/备"这些
  UI 上必用的字反而不在里面 —— 运行时就画成方框（豆腐块）。

本脚本的做法（也是量产固件的标准做法）：
  ① 扫描源码里的**字符串字面量**（跳过注释），收集所有会显示的中文字；
  ② 加上 ASCII 与常用标点、加一份基础兜底字表；
  ③ 调 lv_font_conv 按 16/24 两个字号生成按需子集。

用法：
    python scripts/gen_font.py            # 生成/更新 src/ui/fonts/ui_font_cjk_*.c
    python scripts/gen_font.py --check    # 只报告字符数，不生成（CI 用）

新增界面文案后重跑一次即可；字体文件已入库，正常编译不依赖本脚本。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "src" / "ui" / "fonts"
SRC_DIRS = [ROOT / "src"]
EXTRA_SCAN = [ROOT / "oem"]           # OEM 配置文件里的文案也要收字

# 字体源：优先用仓库内自带，其次用系统字体
FONT_CANDIDATES = [
    ROOT / "third_party" / "fonts" / "SourceHanSansSC-Regular.otf",
    Path("C:/Windows/Fonts/simhei.ttf"),          # 黑体（Windows 自带）
    Path("C:/Windows/Fonts/Deng.ttf"),            # 等线
    Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"),
]

# 兜底字表：即使当前没用到，也保证常见词能显示，避免改文案就翻车
FALLBACK = (
    "打开关闭开关开关灯灯光窗帘空调新风插座摄像头场景回家离家睡眠观影"
    "客厅卧室厨房卫生间主卧次卧书房阳台全宅全部设备在线离线连接中失败"
    "接听挂断呼叫来电通话中未接听请稍候设置返回确定取消音量亮度温度风速"
    "模式制冷制热自动送风低中高静音定时预约电量信号网络无线密码房间号"
    "门口机室内机对讲视频语音唤醒你好小智当前无连接无法服务异常重试"
    "年月日周星期一二三四五六七八九十零〇秒分钟小时"
)

# ASCII 可见字符 + 常用中文标点
BASE_SYMBOLS = "".join(chr(c) for c in range(0x20, 0x7F)) + "，。、：；！？（）《》【】—…·℃°"


def find_lv_font_conv():
    """定位 lv_font_conv 入口，返回可直接 subprocess 执行的命令前缀。

    Windows 上 .cmd 包装器不能直接 CreateProcess 调用（需要 shell），
    所以优先用 node + lv_font_conv.js；找不到再退回 PATH 里的可执行文件。
    """
    roots = [
        Path(os.environ.get("LV_FONT_CONV_ROOT", "")),
        Path(os.environ.get("WORKBUDDY_NODE_HOME", "")),
        Path.home() / ".workbuddy" / "binaries" / "node" / "workspace",
        Path("/usr/local/lib/node_modules"),
    ]
    for root in roots:
        if not root or not root.exists():
            continue
        js = root / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
        if js.exists():
            node = shutil.which("node") or "node"
            return [node, str(js)]
    which = shutil.which("lv_font_conv")
    if which:
        return [which]
    sys.exit("找不到 lv_font_conv，请先 npm install lv_font_conv")


def collect_chars():
    chars = set(BASE_SYMBOLS) | set(FALLBACK)
    srcs = []
    for d in SRC_DIRS + EXTRA_SCAN:
        if not d.exists():
            continue
        for ext in ("*.c", "*.h", "*.ini", "*.json", "*.txt"):
            srcs.extend(d.rglob(ext))
    for p in srcs:
        try:
            text = p.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        if p.suffix in (".c", ".h"):
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)     # 块注释
            text = re.sub(r"//[^\n]*", "", text)                   # 行注释
            text = "\n".join(re.findall(r'"((?:[^"\\]|\\.)*)"', text))  # 只留字符串
        for ch in text:
            if "\u4e00" <= ch <= "\u9fff" or "\u3000" <= ch <= "\u303f" or "\uff00" <= ch <= "\uffef":
                chars.add(ch)
    return chars


def pick_font():
    for f in FONT_CANDIDATES:
        if f.exists():
            return f
    sys.exit("找不到可用的中文字体源，请放到 third_party/fonts/")


def gen(font_path, size, chars, out_c):
    conv = find_lv_font_conv()
    cmd = conv + [
        "--font", str(font_path),
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-kerning",
        "--symbols", "".join(sorted(chars)),
        "-o", str(out_c),
    ]
    r = subprocess.run(cmd, capture_output=True, text=True, shell=False)
    if r.returncode != 0:
        print((r.stdout or "")[-2000:])
        print((r.stderr or "")[-2000:])
        sys.exit(f"lv_font_conv 失败 (size={size})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    chars = collect_chars()
    cjk = [c for c in chars if "\u4e00" <= c <= "\u9fff"]
    print(f"扫描到字符 {len(chars)} 个（其中汉字 {len(cjk)} 个）")
    if args.check:
        return

    font_path = pick_font()
    print(f"字体源：{font_path}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for size in (16, 24):
        out = OUT_DIR / f"ui_font_cjk_{size}.c"
        gen(font_path, size, chars, out)
        print(f"  -> {out.relative_to(ROOT)}  {out.stat().st_size/1024:.1f} KB")


if __name__ == "__main__":
    main()
