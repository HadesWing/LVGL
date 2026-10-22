#!/usr/bin/env python3
"""统计仓库代码量：自研 vs 第三方。只数参与产品的目录，排除构建产物与 LVGL 的示例/文档。"""
import os, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CODE_EXT = {'.c', '.h', '.cpp', '.hpp', '.cc', '.py', '.sh', '.bat', '.ps1'}
SKIP_DIR = {'.git', 'build', 'build-64k', 'build-echo', 'build-h264', 'build-mem32768',
            'build-mem40960', 'bin', 'logs', '.vscode', '.devcontainer', '__pycache__'}

# LVGL 里不参与产品构建的子目录
LVGL_SKIP = {'lvgl/demos', 'lvgl/examples', 'lvgl/docs', 'lvgl/tests', 'lvgl/zephyr',
             'lvgl/env_support', 'lvgl/scripts', 'lvgl/.github', 'lvgl/.devcontainer',
             'lvgl/configs', 'lvgl/sbom', 'lvgl/include', 'lvgl/libs'}

def classify(rel: str):
    p = rel.replace('\\', '/')
    if p.startswith('lvgl/'):
        for s in LVGL_SKIP:
            if p.startswith(s + '/'):
                return None
        if p.startswith('lvgl/src/libs/'):
            return 'LVGL内嵌可选第三方(默认未编译)'
        return 'LVGL'
    if p.startswith('src/'):
        return '自研-产品代码'
    if p.startswith('tools/'):
        return '自研-调试工具'
    if p.startswith('scripts/'):
        return '自研-构建脚本'
    if p in ('CMakeLists.txt', 'Makefile', 'build.cmd') or p.startswith('config/'):
        return '自研-构建配置'
    if p.startswith('FreeRTOS/'):
        return 'FreeRTOS(空submodule)'
    return None

stats = {}
files = []
for dirpath, dirnames, filenames in os.walk(ROOT):
    rel_dir = os.path.relpath(dirpath, ROOT).replace('\\', '/')
    dirnames[:] = [d for d in dirnames if d not in SKIP_DIR]
    for fn in filenames:
        ext = os.path.splitext(fn)[1].lower()
        if ext not in CODE_EXT:
            continue
        rel = fn if rel_dir == '.' else f"{rel_dir}/{fn}"
        cat = classify(rel)
        if not cat:
            continue
        try:
            raw = (ROOT / rel).read_bytes()
        except OSError:
            continue
        text = raw.decode('utf-8', errors='ignore')
        total = text.count('\n') + (0 if text.endswith('\n') else 1)
        blank = sum(1 for l in text.splitlines() if not l.strip())
        comment = sum(1 for l in text.splitlines() if l.strip().startswith(('//', '/*', '*', '#')))
        code = total - blank - comment
        s = stats.setdefault(cat, {'files': 0, 'total': 0, 'code': 0, 'comment': 0, 'blank': 0})
        s['files'] += 1; s['total'] += total; s['code'] += code
        s['comment'] += comment; s['blank'] += blank
        if cat in ('自研-产品代码', '自研-调试工具'):
            files.append((rel, code))

print(f"{'分类':<30}{'文件':>6}{'总行':>9}{'代码行':>9}{'注释':>9}{'空行':>8}")
print('-' * 72)
order = ['自研-产品代码', '自研-调试工具', '自研-构建脚本', '自研-构建配置',
         'LVGL', 'LVGL内嵌可选第三方(默认未编译)', 'FreeRTOS(空submodule)']
tot_code = 0
for k in order:
    if k not in stats: continue
    s = stats[k]
    print(f"{k:<30}{s['files']:>6}{s['total']:>9}{s['code']:>9}{s['comment']:>9}{s['blank']:>8}")
    tot_code += s['code']
print('-' * 72)
print(f"{'合计':<30}{'':>6}{'':>9}{tot_code:>9}")

print('\n=== LVGL 细分 ===')
for sub in ('lvgl/src/core', 'lvgl/src/display', 'lvgl/src/draw', 'lvgl/src/font',
            'lvgl/src/misc', 'lvgl/src/others', 'lvgl/src/widgets', 'lvgl/src/libs',
            'lvgl/src/lvgl.h', 'lvgl/src/lv_conf_internal.h'):
    pass

print('\n=== 自研文件明细（代码行降序）===')
for rel, code in sorted(files, key=lambda x: -x[1])[:30]:
    print(f"{code:>7}  {rel}")
print(f"\n自研代码合计: {sum(c for _, c in files)} 行, {len(files)} 个文件")
