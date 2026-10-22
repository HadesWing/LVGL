#!/usr/bin/env bash
# =============================================================================
# 实际执行编译/运行/压测的脚本（Makefile 只是它的转发壳）
#
# 为什么不让 Makefile 直接调 cmake/ninja：
#   Windows 下 make 启动的子进程环境不完整（PATH 里缺 C:\msys64\mingw64\bin 时，
#   gcc 找不到自己的 cc1/as 会**静默失败**，只报 "compiler is not able to compile
#   a simple test program"，不给任何原因）。这里统一在 MSYS2 环境里跑，稳定可复现。
#
# 用法：bash scripts/dev.sh <build|run|stats|stress|mem64|debug|longrun|clean>
# =============================================================================
set -e

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ---------- 进入 MSYS2 工具链环境（Windows） ----------
if [ -d /c/msys64 ]; then
    export MSYSTEM=${MSYSTEM:-MINGW64}
    export PATH="/c/msys64/usr/bin:/c/msys64/mingw64/bin:$PATH"
    if [ -f /etc/profile ]; then source /etc/profile >/dev/null 2>&1 || true; fi
fi

# ---------- 避开中文路径（Windows 专属坑，必须处理） ----------
# 现象：编译能过，链接一定失败
#   ld.exe: cannot open output file C:\Users\hades\WorkBuddy\????\...\bin\main.exe:
#           No such file or directory
# 原因：cmake 生成链接命令时要经过 cmd.exe，中文在非 UTF-8 代码页下被替换成 '?'，
#       链接器拿到的是一个不存在的路径。编译那几步是 ninja 直接调 gcc，所以没事。
# 办法：在 Windows 上通过纯 ASCII 的目录联接（junction，无需管理员权限）进入工程，
#       让所有生成路径都变成 C:/lvgl_dev/... 这样干干净净的形态。
WORK_DIR="$SELF_DIR"
if [ -d /c/msys64 ]; then
    LINK_DIR="${LVGL_DEV_LINK:-/c/lvgl_dev}"
    if [ ! -d "$LINK_DIR" ]; then
        cmd //c mklink //J "$(cygpath -w "$LINK_DIR")" "$(cygpath -w "$SELF_DIR")" \
            >/dev/null 2>&1 || true
    fi
    # 只有联接真的指向本工程时才用它，避免误编译到别的目录
    if [ -f "$LINK_DIR/CMakeLists.txt" ]; then
        WORK_DIR="$LINK_DIR"
        echo "ℹ 使用 ASCII 路径编译：$WORK_DIR -> $SELF_DIR"
    fi
fi

cd "$WORK_DIR"

BUILD_DIR=build
if [ -d /c/msys64 ]; then EXE_NAME=main.exe; SUFFIX=.exe; else EXE_NAME=main; SUFFIX=; fi
EXE_64="main64$SUFFIX"      # 64KB 内存池版本（模拟小内存设备）
EXE_DBG="main-dbg$SUFFIX"   # 断言版本

configure() {
    cmake -G Ninja -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "$@"
}
compile() {
    # 并发度默认 4：MSYS2 下开太高（-j8 满载）偶发 fork/exec 死锁
    # ——表现为 cc.exe / as.exe CPU 时间停在 0.0x 秒再不动，编译假死。
    # 真卡住了就再执行一次 make build，ninja 会增量续编。
    ninja -C "$BUILD_DIR" -j "${JOBS:-4}"
}
need_exe() {
    [ -f "bin/$1" ] || { echo "还没编译：$1，先执行 make build"; exit 1; }
}

CMD="${1:-build}"
shift 2>/dev/null || true

case "$CMD" in
    build)
        configure
        compile
        echo ""
        echo "✅ 编译完成：$(pwd)/bin/$EXE_NAME"
        ;;
    run)
        need_exe "$EXE_NAME"
        cd bin && exec "./$EXE_NAME"
        ;;
    stats)
        need_exe "$EXE_NAME"
        cd bin && LVGL_STATS=1 LVGL_RUN_SEC="${LVGL_RUN_SEC:-0}" exec "./$EXE_NAME"
        ;;
    stress)
        need_exe "$EXE_NAME"
        cd bin && LVGL_STRESS=1 LVGL_STATS=1 LVGL_RUN_SEC="${LVGL_RUN_SEC:-0}" exec "./$EXE_NAME"
        ;;
    mem64)
        # 独立构建目录 + 独立输出名：换 LV_MEM_SIZE 会触发 lvgl 全量重编，
        # 而且不能覆盖掉默认构建的 bin/main.exe
        BUILD_DIR=build-64k
        configure -DLV_MEM_SIZE=65536 -DMAIN_EXE_NAME=main64
        compile
        echo "✅ 64KB 内存池版本：bin/$EXE_64"
        cd bin && LVGL_STATS=1 LVGL_RUN_SEC="${LVGL_RUN_SEC:-0}" exec "./$EXE_64"
        ;;
    mem)
        # 任意内存池大小：make mem SIZE=32768
        SIZE="${1:-65536}"
        SIZE="${SIZE#SIZE=}"   # 兼容 make mem SIZE=32768 与 build.cmd mem SIZE=32768
        BUILD_DIR="build-mem$SIZE"
        configure -DLV_MEM_SIZE="$SIZE" -DMAIN_EXE_NAME=main64
        compile
        echo "✅ ${SIZE}B 内存池版本：bin/$EXE_64"
        cd bin && LVGL_STATS=1 LVGL_RUN_SEC="${LVGL_RUN_SEC:-0}" exec "./$EXE_64"
        ;;
    debug)
        BUILD_DIR=build-debug
        configure -DLV_DEBUG_ASSERT=1 -DMAIN_EXE_NAME=main-dbg
        compile
        echo "✅ 断言版本：bin/$EXE_DBG"
        cd bin && LVGL_RUN_SEC="${LVGL_RUN_SEC:-0}" exec "./$EXE_DBG"
        ;;
    longrun)
        need_exe "$EXE_NAME"
        cd bin && LVGL_STATS=1 LVGL_RUN_SEC="${LVGL_RUN_SEC:-1800}" exec "./$EXE_NAME"
        ;;
    clean)
        cmake -E rm -rf build build-64k build-debug
        rm -rf build-mem* 2>/dev/null || true
        echo "build/ build-64k/ build-debug/ build-mem*/ 已删除"
        ;;
    help)
        cat <<'EOF'
make              编译 + 运行（最常用）
make build        只编译
make run          只运行
make stats        指标采集：每 5 秒打印 FPS/内存/循环耗时，明细写 bin/stats.csv
make stress       压测：每 200ms 建删 8 个控件，查内存泄漏与碎片
make mem64        64KB 内存池运行（模拟小内存设备）
make mem SIZE=N   指定内存池大小（例：make mem SIZE=40960）
make debug        开 LVGL 断言运行（查越界/野指针，性能会掉）
make longrun      30 分钟长稳，到点打印汇总并退出
make rebuild      全量重编（= clean + build）
make clean        删除所有构建目录

环境变量：
  LVGL_STATS=1      写 bin/stats.csv 并打印采样
  LVGL_RUN_SEC=N    跑 N 秒后自动退出并打印汇总
  LVGL_STRESS=1     压测模式（同 make stress）
  LVGL_MOCK=0       关闭设备事件模拟
  JOBS=N            编译并发度（默认 4，MSYS2 下别超过 6）
EOF
        ;;
    *)
        echo "未知目标: $CMD"; exit 1
        ;;
esac
