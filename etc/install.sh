#!/bin/sh
# =============================================================================
# 目标机安装脚本（在板子上跑，不是开发机）
#
#   sudo ./install.sh              # 安装并启动
#   sudo ./install.sh --no-start    # 只装不起（产线烧写时用）
#
# 三条纪律：
#   1. **已有的 sip.ini 绝不覆盖** —— 里面是这台设备的账号密码，
#      升级固件把配置冲掉，等于把设备变砖，现场只能拆机重烧。
#   2. 先装新二进制到临时名、再原子改名为正式名 —— 升级过程中掉电，
#      不会留下一个"半个文件"导致下次开不了机。
#   3. 记一份旧版本，失败能回滚（/usr/bin/indoor-unit.prev）。
# =============================================================================
set -e

PREFIX="${PREFIX:-/usr}"
DEST_BIN="$PREFIX/bin/indoor-unit"
CONF_DIR="/etc/indoor"
CONF="$CONF_DIR/sip.ini"
UNIT="/etc/systemd/system/indoor.service"
STATE_DIR="/var/lib/indoor"
HERE="$(cd "$(dirname "$0")" && pwd)"

NO_START=0
for a in "$@"; do
    [ "$a" = "--no-start" ] && NO_START=1
done

[ "$(id -u)" = "0" ] || { echo "需要 root：sudo $0"; exit 1; }

echo "── 安装到 $DEST_BIN ──"
if [ -f "$DEST_BIN" ]; then
    # 留一份上一版：出问题时 mv 回来即可，不用重新找固件包
    cp -f "$DEST_BIN" "$DEST_BIN.prev"
    echo "   已备份旧版本 → $DEST_BIN.prev"
fi
install -m 0755 "$HERE/bin/indoor-unit" "$DEST_BIN.new"
mv -f "$DEST_BIN.new" "$DEST_BIN"          # 同分区 rename，原子

echo "── 配置 ──"
mkdir -p "$CONF_DIR" "$STATE_DIR"
if [ -f "$CONF" ]; then
    echo "   $CONF 已存在，保留（不覆盖账号密码）"
else
    install -m 0644 "$HERE/etc/sip.ini.example" "$CONF"
    echo "   已写入模板 → $CONF"
    echo "   ⚠ 请在启动前把 server/user/password 改成这台设备的实际值"
fi

echo "── systemd ──"
if command -v systemctl >/dev/null 2>&1; then
    install -m 0644 "$HERE/etc/indoor.service" "$UNIT"
    systemctl daemon-reload
    systemctl enable indoor.service >/dev/null 2>&1 || true
    if [ "$NO_START" = "0" ]; then
        systemctl restart indoor.service
        sleep 1
        systemctl --no-pager --lines=5 status indoor.service || true
    else
        echo "   --no-start：已安装但未启动"
    fi
else
    echo "   没有 systemd（这板子用 init 脚本？），只装二进制和配置"
fi

echo ""
echo "✅ 安装完成。版本："
"$DEST_BIN" --version
echo ""
echo "排障："
echo "  journalctl -u indoor -n 100 -f      # 看日志"
echo "  systemctl stop indoor && $DEST_BIN   # 前台跑一遍看输出"
echo "回滚："
echo "  cp -f $DEST_BIN.prev $DEST_BIN && systemctl restart indoor"
