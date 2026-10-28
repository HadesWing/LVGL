#!/usr/bin/env python3
"""唤醒词音素变体生成器。

把一句唤醒词展开成多行近音变体，喂给 sherpa-onnx KWS。
动机：KWS 是把关键词当成一条音素序列去做 beam search，若目标词的音素
序列在模型看来概率不够高就永远过不了阈值。写成多行变体等于把搜索面
扩大，给解码器多几条活路。工业界常用做法，代价是误报率略微上升。

用法：
  python scripts/kw_variants.py --model-dir ../../.tools/kws \
      --word 你好小智 --pinyin "n ǐ h ǎo x iǎo zh ì" --out ../../.tools/kws/kw_var.txt

  # 只查 tokens 是否齐备，不生成
  python scripts/kw_variants.py --model-dir ../../.tools/kws --pinyin "n ǐ h ǎo x iǎo" --check

注意：
  - tokens.txt 是音素级（声母/韵母 + 声调符号），不是汉字、不是拼音整数调。
  - 变体只替换声母/韵母/声调，不改变音节数，否则就不是同一个词了。
"""

import argparse
import os
import sys

# 每个音节的候选变体：key 是标准音素，value 是近音替换列表。
# 只收「普通话里可能被听混」的组合，乱加会显著抬高误报。
NEAR = {
    # 声母
    "zh": ["z", "ch"],
    "z": ["zh"],
    "ch": ["zh", "c"],
    "c": ["ch"],
    "sh": ["s"],
    "s": ["sh"],
    "x": ["s", "sh"],
    "n": ["l"],
    "l": ["n"],
    # 韵母（带调）
    "ǎo": ["áo", "ào"],
    "iǎo": ["iáo"],
    "ǐ": ["í", "ī"],
    "ī": ["í", "ǐ"],
    "ì": ["í", "ǐ"],
    "í": ["ì", "ī"],
    "ǎo": ["áo"],
}


def load_tokens(md):
    p = os.path.join(md, "tokens.txt")
    if not os.path.isfile(p):
        sys.exit("找不到 tokens.txt: " + p)
    tok = set()
    with open(p, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if parts:
                tok.add(parts[0])
    return tok


def variants(phones, limit):
    """按槽位展开，最多保留 limit 条（第一条永远是原样）。"""
    out = [list(phones)]
    for i, ph in enumerate(phones):
        for alt in NEAR.get(ph, []):
            v = list(phones)
            v[i] = alt
            if v not in out:
                out.append(v)
            if len(out) >= limit:
                return out
    # 两处同时替换（更容易过 beam，但离原音更远，放最后）
    for i, ph in enumerate(phones):
        for alt in NEAR.get(ph, []):
            for j, ph2 in enumerate(phones):
                if j <= i:
                    continue
                for alt2 in NEAR.get(ph2, [])[:1]:
                    v = list(phones)
                    v[i], v[j] = alt, alt2
                    if v not in out:
                        out.append(v)
                    if len(out) >= limit:
                        return out
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", required=True)
    ap.add_argument("--word", default="", help="显示文本，如 你好小智")
    ap.add_argument("--pinyin", required=True, help="标准音素串，空格分隔，如 'n ǐ h ǎo'")
    ap.add_argument("--out", default="", help="输出关键词文件；省略则只打印")
    ap.add_argument("--limit", type=int, default=8, help="最多生成几条变体")
    ap.add_argument("--check", action="store_true", help="只校验 tokens 是否齐备")
    a = ap.parse_args()

    tok = load_tokens(a.model_dir)
    phones = a.pinyin.split()

    missing = [t for t in phones if t not in tok]
    print("标准音素: %s" % " ".join(phones))
    print("tokens 共 %d 个" % len(tok))
    if missing:
        print("✗ 缺失 token: %s" % " ".join(missing))
        print("  从这个模型里挑不到的发音，KWS 一定拼不出来 → 换词或换 token 写法")
        return 2
    print("✓ 音素齐备")
    if a.check:
        return 0

    vs = variants(phones, a.limit)
    usable = []
    for v in vs:
        miss = [t for t in v if t not in tok]
        if miss:
            print("  跳过（缺 %s）: %s" % (" ".join(miss), " ".join(v)))
        else:
            usable.append(v)

    lines = [" ".join(v) + " @" + (a.word or " ".join(phones)) for v in usable]
    print("\n生成 %d 条变体：" % len(lines))
    for ln in lines:
        print("  " + ln)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        print("\n已写入 %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
