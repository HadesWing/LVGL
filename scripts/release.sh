#!/usr/bin/env bash
# =============================================================================
# 一键发布：自检 → 发布构建 → 组装 → 校验和
#
#   bash scripts/release.sh                    # 当前平台
#   bash scripts/release.sh --allow-dirty      # 工作区有改动也发（不建议）
#   bash scripts/release.sh --skip-tests       # 跳过 UT（**别在真发布时用**）
#   TOOLCHAIN=cmake/arm-linux.cmake bash scripts/release.sh   # 交叉编译板端包
#
# 为什么发布必须由脚本做，而不是"手动 make 一下再 tar"：
#   手动流程里最容易漏的是 **UT**（"这次改动很小，不用跑了吧"）和 **版本号**
#   （改完忘了 bump，现场两个不同的东西报同一个版本）。这两件都在脚本里钉死。
#
# 产物：
#   dist/indoor-unit-<版本>-<平台>/
#   ├── bin/indoor-unit          已 strip 的主程序
#   ├── etc/{sip.ini.example, indoor.service, install.sh}
#   ├── LICENSES/NOTICE.md       第三方组件与 LGPL 义务
#   ├── VERSION
#   └── SHA256SUMS               包内自校验
#   dist/indoor-unit-<版本>-<平台>.tar.gz (+ .sha256)
# =============================================================================
set -e

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SELF_DIR"

ALLOW_DIRTY=0
RUN_TESTS=1
for a in "$@"; do
    case "$a" in
        --allow-dirty) ALLOW_DIRTY=1 ;;
        --skip-tests)  RUN_TESTS=0 ;;
        -h|--help)     sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数：$a"; exit 1 ;;
    esac
done

VER="$(tr -d ' \r\n' < VERSION)"
[ -n "$VER" ] || { echo "❌ VERSION 文件是空的"; exit 1; }

# ---------- 平台标签 ----------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) PLAT="windows-x64" ;;
    Linux)                PLAT="linux-$(uname -m)" ;;
    Darwin)               PLAT="macos-$(uname -m)" ;;
    *)                    PLAT="unknown" ;;
esac
# 交叉编译时平台名要跟着工具链走，否则板端包会被标成 linux-x86_64
if [ -n "${TOOLCHAIN:-}" ]; then
    PLAT="cross-$(basename "$TOOLCHAIN" .cmake)"
fi

if [ -d /c/msys64 ]; then EXE_SUFFIX=".exe"; else EXE_SUFFIX=""; fi
PKG="indoor-unit-${VER}-${PLAT}"

echo "═══ 发布 indoor-unit ${VER}（${PLAT}）═══"
echo ""

# ---------- 1. 工作区必须干净 ----------
# 带着未提交的改动打包，是这个流程里最危险的一步：
# 出厂的二进制和仓库里的源码对不上，事后没人能复现，也没法 git bisect。
if [ "$ALLOW_DIRTY" = "0" ]; then
    if [ -n "$(git status --porcelain 2>/dev/null)" ]; then
        echo "❌ 工作区有未提交的改动："
        git status --short | sed 's/^/   /'
        echo ""
        echo "   先提交，或者加 --allow-dirty（仅用于本地试打包）。"
        exit 1
    fi
    echo "① 工作区干净 ✓"
else
    echo "① ⚠ --allow-dirty：带着未提交改动打包，产物不可复现"
fi

# ---------- 2. 单元测试 ----------
if [ "$RUN_TESTS" = "1" ]; then
    echo ""
    echo "② 单元测试"
    bash scripts/dev.sh ut > /tmp/ut_rel.log 2>&1 || {
        echo "❌ UT 失败，发布中止："
        tail -20 /tmp/ut_rel.log
        exit 1
    }
    grep -E "总计" /tmp/ut_rel.log | sed 's/^/   /'
else
    echo ""
    echo "② ⚠ --skip-tests：跳过 UT（真发布不要这么干）"
fi

# ---------- 3. 发布构建 ----------
echo ""
echo "③ 发布构建（-O2 + strip）"
bash scripts/dev.sh relbuild | tail -2

REL="bin/indoor-unit${EXE_SUFFIX}"
[ -f "$REL" ] || { echo "❌ 没生成 $REL"; exit 1; }

# ---------- 4. 组装 ----------
echo ""
echo "④ 组装 dist/$PKG"
rm -rf "dist/$PKG"
mkdir -p "dist/$PKG/bin" "dist/$PKG/etc" "dist/$PKG/LICENSES"

cp "$REL"                        "dist/$PKG/bin/"
cp etc/sip.ini.example           "dist/$PKG/etc/"
cp etc/indoor.service            "dist/$PKG/etc/"
cp etc/install.sh                "dist/$PKG/etc/"
chmod 0755 "dist/$PKG/etc/install.sh"
cp LICENSE-NOTICE.md             "dist/$PKG/LICENSES/NOTICE.md"
cp VERSION                       "dist/$PKG/VERSION"

# ---------- LGPL-2.1 §6：必须提供"用修改过的 libosip2 重新链接"的能力 ----------
# 只写一句"可向我们索取"是不够的：用户得先联系你、再等回复，等于没有能力重链接。
# 所以把目标文件、静态库、原始链接命令一起打进去 —— 三样凑齐才算真的可重链接。
mkdir -p "dist/$PKG/relink"
OBJ_ROOT="build-rel/CMakeFiles/main.dir"
if [ -d "$OBJ_ROOT/src" ]; then
    ( cd "$OBJ_ROOT" && tar czf "$SELF_DIR/dist/$PKG/relink/obj.tar.gz" src )
    echo "   relink/obj.tar.gz   我们的目标文件（可替换后重新链接）"
else
    echo "   ⚠ 找不到目标文件目录 $OBJ_ROOT，relink 材料不完整"
fi
for libdir in "${OSIP_ROOT:-/c/osip_build/install}/lib" "third_party/osip-install/lib"; do
    if [ -f "$libdir/libosip2.a" ]; then
        cp "$libdir/libosip2.a"      "dist/$PKG/relink/"
        cp "$libdir/libosipparser2.a" "dist/$PKG/relink/" 2>/dev/null || true
        echo "   relink/libosip2.a   我们链接时用的那个库（替换成你改过的版本即可）"
        break
    fi
done
# 原始链接命令：重链接时要靠它把顺序和库名对上
ninja -C build-rel -t commands main 2>/dev/null | tail -1 > "dist/$PKG/relink/link.txt" || true
if [ -s "dist/$PKG/relink/link.txt" ]; then
    echo "   relink/link.txt     完整链接命令（改库路径后直接跑）"
else
    echo "   ⚠ 取不到链接命令，relink/link.txt 为空"
fi

# 版本自证：装到板子上第一个要能回答的问题就是"这是哪一版"
# 注意不能写成 `cmd | sed || echo` —— 管道的退出码是 sed 的，永远是 0，
# 失败时既不打印版本也不打印警告，静默少一行。先取输出，再判断。
VER_LINE="$("dist/$PKG/bin/indoor-unit${EXE_SUFFIX}" --version 2>/dev/null || true)"
if [ -n "$VER_LINE" ]; then
    echo "   版本自证：$VER_LINE"
else
    echo "   （版本自证跳过：交叉编译产物在本机跑不起来，这是正常的）"
fi

# 包内自校验：解开之后能自己验完整性，不用回头找 .sha256
( cd "dist/$PKG" && find . -type f ! -name SHA256SUMS | sort | \
  while read -r f; do sha256sum "$f"; done > SHA256SUMS )

# ---------- 5. 打包 ----------
echo ""
echo "⑤ 打 tar.gz"
( cd dist && tar czf "$PKG.tar.gz" "$PKG" )
( cd dist && sha256sum "$PKG.tar.gz" > "$PKG.tar.gz.sha256" )

SIZE=$(wc -c < "dist/$PKG.tar.gz")
echo ""
echo "════ 完成 ════"
echo "  dist/$PKG/"
echo "  dist/$PKG.tar.gz          $((SIZE / 1024)) KB"
echo "  dist/$PKG.tar.gz.sha256   $(cut -c1-16 "dist/$PKG.tar.gz.sha256")…"
echo ""
echo "包内结构："
find "dist/$PKG" -type f | sed "s|dist/$PKG|  |" | sort
echo ""
echo "下一步："
echo "  校验  sha256sum -c dist/$PKG.tar.gz.sha256"
echo "  部署  解包后 sudo ./etc/install.sh"
