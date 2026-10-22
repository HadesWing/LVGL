#!/usr/bin/env python3
"""统计"真正编进二进制"的代码规模与体积。

两个口径同时给：
  A. 有效编译单元：目标文件的 .text 非零才算"这个源文件有代码进来"
     —— LVGL 靠 lv_conf.h 里的宏裁剪，被关掉的文件编译出来是空 obj，
        按源码行数直接统计会把整个上游仓库都算进去，严重高估。
  B. 产物体积：.text + .rdata（只读数据，含字体位图）+ 已初始化 .data，
     不含 .bss（不占文件）。

用法：python scripts/stat_binary.py [build dir]
"""
import struct, sys
from pathlib import Path
from collections import defaultdict

BUILD = Path(sys.argv[1] if len(sys.argv) > 1 else 'build')
ROOT = Path(__file__).resolve().parents[1]


def coff_sections(p: Path):
    try:
        data = p.read_bytes()
    except OSError:
        return {}
    if len(data) < 20:
        return {}
    machine, nsec, _, _, _, optsz, _ = struct.unpack_from('<HHIIIHH', data, 0)
    if machine not in (0x014c, 0x8664, 0xaa64):
        return {}
    off = 20 + optsz
    out = defaultdict(int)
    for i in range(nsec):
        base = off + i * 40
        if base + 40 > len(data):
            break
        name = data[base:base + 8].rstrip(b'\x00').decode('latin1')
        raw, = struct.unpack_from('<I', data, base + 16)
        if name.startswith('.text'):
            out['text'] += raw
        elif name.startswith('.rdata'):
            out['rdata'] += raw
        elif name.startswith('.data'):
            out['data'] += raw
    return out


def src_of(rel: str):
    """obj 相对路径 → 源文件相对路径"""
    r = rel
    for marker in ('lvgl_thorvg.dir/', 'lvgl.dir/', 'main.dir/'):
        if marker in r:
            r = r.split(marker, 1)[1]
            break
    for suf in ('.obj', '.o'):
        if r.endswith(suf):
            r = r[:-len(suf)]
    return r


def bucket_of(rel: str, src: str):
    if 'lvgl_thorvg.dir/' in rel:
        return 'LVGL · libs/thorvg (矢量引擎)'
    if 'lvgl.dir/' in rel:
        sub = src.split('/')          # src/core/lv_obj.c
        mod = sub[1] if len(sub) > 2 else sub[0]
        return f'LVGL · {mod}'
    if 'main.dir/' in rel:
        top = src.split('/')
        name = top[1] if len(top) > 1 else src
        return f'自研 · {name}'
    return '其他'


rows = defaultdict(lambda: {'objs': 0, 'live': 0, 'lines': 0, 'text': 0, 'rdata': 0, 'data': 0})
detail = []

SKIP_TARGET = ('lvgl_examples.dir/', 'lvgl_demos.dir/')
for obj in BUILD.rglob('*.obj'):
    rel = obj.relative_to(BUILD).as_posix()
    if any(t in rel for t in SKIP_TARGET):
        continue   # 示例与 demo 不参与产品链接，只是 CMake 默认给编了
    src = src_of(rel)
    sect = coff_sections(obj)
    text = sect.get('text', 0)
    key = bucket_of(rel, src)
    r = rows[key]
    r['objs'] += 1
    r['text'] += text
    r['rdata'] += sect.get('rdata', 0)
    r['data'] += sect.get('data', 0)
    live = text > 0
    if live:
        r['live'] += 1
        sp = ROOT / ('lvgl/' + src if rel.startswith('lvgl/') else src)
        if sp.exists():
            n = len(sp.read_text(encoding='utf-8', errors='ignore').splitlines())
            r['lines'] += n
            detail.append((key, src, n, text))

tot = {k: sum(v[k] for v in rows.values()) for k in ('objs', 'live', 'lines', 'text', 'rdata', 'data')}
tot_size = tot['text'] + tot['rdata'] + tot['data']

print(f"{'模块':<24}{'obj':>5}{'有效':>5}{'源码行':>8}{'.text':>9}{'.rdata':>9}{'体积':>10}{'占比':>7}")
print('-' * 82)
for k, v in sorted(rows.items(), key=lambda x: -(x[1]['text'] + x[1]['rdata'] + x[1]['data'])):
    size = v['text'] + v['rdata'] + v['data']
    pct = 100.0 * size / tot_size if tot_size else 0
    print(f"{k:<24}{v['objs']:>5}{v['live']:>5}{v['lines']:>8}{v['text']:>9}{v['rdata']:>9}{size:>10}{pct:>6.1f}%")
print('-' * 82)
pct = 100.0
print(f"{'合计':<24}{tot['objs']:>5}{tot['live']:>5}{tot['lines']:>8}{tot['text']:>9}{tot['rdata']:>9}{tot_size:>10}{pct:>6.1f}%")

exe = ROOT / 'bin/main.exe'
if exe.exists():
    print(f"\nbin/main.exe = {exe.stat().st_size:,} 字节 ({exe.stat().st_size/1048576:.2f} MB)"
          f"  [MSVC Release，含未裁掉的符号表]")
