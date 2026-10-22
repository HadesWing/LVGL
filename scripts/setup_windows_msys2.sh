#!/usr/bin/env bash
# =============================================================================
# LVGL PC 模拟器 —— Windows + MSYS2 一键环境搭建脚本
#
# 用法（在 MSYS2 MINGW64 终端里执行）：
#     bash scripts/setup_windows_msys2.sh
#
# 做的事：装 MSYS2（若缺）→ 配国内镜像 → 装 gcc/cmake/ninja/SDL2
#         → 拉 lvgl 子模块 → 编译 → 拷贝运行库 DLL
#
# 注意：脚本假设仓库根目录已 clone 到位，且 MSYS2 安装在 C:\msys64
# =============================================================================
set -u

MSYS_ROOT="/c/msys64"
TUNA="https://mirrors.tuna.tsinghua.edu.cn/msys2"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

log() { printf '\n\033[1;36m[%s]\033[0m %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { printf '\n\033[1;31m[失败]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------- 1. 安装 MSYS2
log "检查 MSYS2"
if [ ! -x "$MSYS_ROOT/usr/bin/pacman.exe" ]; then
  log "未检测到 MSYS2，从清华 TUNA 镜像下载"
  TMP_EXE="$TEMP/msys2-latest.exe"
  curl -L --fail -o "$TMP_EXE" "$TUNA/distrib/msys2-x86_64-latest.exe" \
    || die "下载 MSYS2 失败，请检查网络"
  log "静默安装到 C:\\msys64（约 1 分钟）"
  "$TMP_EXE" install --root 'C:/msys64' --confirm-command --accept-messages \
    || die "安装 MSYS2 失败"
  rm -f "$TMP_EXE"
else
  log "MSYS2 已存在，跳过安装"
fi

# ------------------------------------------------------------ 2. 配置国内镜像
log "配置 pacman 镜像（TUNA / USTC）"
ML_DIR="$MSYS_ROOT/etc/pacman.d"
# 注意：TUNA 的 mingw 仓库路径是 /msys2/mingw/$repo/，比直觉多一层 mingw
cat > "$ML_DIR/mirrorlist.mingw" <<EOF
Server = $TUNA/mingw/\$repo/
Server = https://mirrors.ustc.edu.cn/msys2/mingw/\$repo/
EOF
cat > "$ML_DIR/mirrorlist.msys" <<EOF
Server = $TUNA/msys/\$arch/
Server = https://mirrors.ustc.edu.cn/msys2/msys/\$arch/
EOF

# Windows 上 gpg 初始化密钥环可能卡十几分钟，本地开发用 HTTPS 镜像可接受
log "设置 SigLevel = Never（跳过耗时极长的密钥环初始化）"
sed -i 's/^SigLevel *= *Required/SigLevel    = Never/' "$MSYS_ROOT/etc/pacman.conf"

# --------------------------------------------------------------- 3. 装工具链
log "同步软件源"
pacman -Sy --noconfirm || die "pacman -Sy 失败"

log "安装工具链（gcc / cmake / ninja / SDL2）"
pacman -S --needed --noconfirm \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-SDL2 || die "工具链安装失败"

log "版本自检"
gcc --version | head -1
cmake --version | head -1
echo "ninja $(ninja --version)"
echo "SDL2 $(pkg-config --modversion sdl2)"

# ------------------------------------------------------------- 4. 拉 lvgl 源码
cd "$REPO_ROOT" || die "进不了仓库根目录"
if [ ! -f lvgl/lvgl.h ]; then
  log "拉取 lvgl 子模块（首次较慢）"
  git submodule update --init --recursive lvgl \
    || git clone --depth 1 https://github.com/lvgl/lvgl.git lvgl \
    || die "拉取 lvgl 失败"
else
  log "lvgl 源码已存在，跳过"
fi

# ----------------------------------------------------------------- 5. 编译
log "构建（首次约 10-15 分钟，961 个目标文件）"
mkdir -p build && cd build || die "创建 build 目录失败"
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. || die "cmake 配置失败"
ninja || die "编译失败（若 cc.exe 卡死：杀进程后重跑 ninja 即可增量继续）"

# ------------------------------------------------------- 6. 打包运行库 DLL
log "拷贝运行库到 bin/（避免系统 PATH 里的旧版 DLL 抢加载）"
BIN_DIR="$REPO_ROOT/bin"
mkdir -p "$BIN_DIR"
for dll in SDL2.dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll zlib1.dll; do
  [ -f "$MSYS_ROOT/mingw64/bin/$dll" ] && cp -f "$MSYS_ROOT/mingw64/bin/$dll" "$BIN_DIR/"
done

log "完成 ✅  运行：$BIN_DIR/main.exe"
