# =============================================================================
# LVGL 室内机 demo —— 一键编译 / 运行 / 压测
#
#   make            编译并运行（最常用）
#   make build      只编译
#   make run        只运行
#   make stats      带指标运行（每 5 秒打印 FPS/内存/循环耗时，明细写 bin/stats.csv）
#   make stress     压测（控件反复建删 + 切主题，查泄漏与碎片）
#   make mem64      64KB 内存池编译运行（模拟小内存设备）
#   make mem SIZE=32768   任意内存池大小（找崩溃边界用）
#   make debug      开 LVGL 断言运行（查越界/野指针，性能会下降）
#   make longrun    30 分钟长稳（到点打印汇总并退出）
#   make duo        双端互通：本机起两个实例（室内机 + 客户端），双向音频各验一次
#   make duo-video  视频接收链路：ffmpeg 推 RTP → 室内机解包/解码/落盘 → 与参考 YUV 比对
#   make duo-disp   视频上屏链路：解码 → RGB565 缩放 → LVGL 绘制 → 抓屏逐像素比对
#   make ut         自研代码单元测试（零依赖，PC 与板子同一份）
#   make loss       丢包恢复验收：注入丢包 + 逐帧 CRC 核对（含"关恢复"对照组）
#   make release    发布打包：UT → 发布构建（strip）→ dist/ 出 tar.gz + sha256
#   make rebuild    全量重编
#   make clean      删构建目录
#   make help       显示帮助
#
# 说明：真正干活的是 scripts/dev.sh（在 MSYS2 环境里跑）。
#       原因：make 直接调 cmake 时子进程 PATH 不完整，gcc 会静默失败。
# =============================================================================

ifeq ($(OS),Windows_NT)
  BASH := C:/msys64/usr/bin/bash.exe
else
  BASH := bash
endif

DEV := $(BASH) scripts/dev.sh

.PHONY: all build run rebuild stats stress mem64 mem debug longrun duo duo-video duo-disp ut loss release clean help

all: build run

build:
	@$(DEV) build

run:
	@$(DEV) run

rebuild:
	@$(DEV) clean
	@$(DEV) build

stats:
	@$(DEV) stats

stress:
	@$(DEV) stress

mem64:
	@$(DEV) mem64

mem:
	@$(DEV) mem $(SIZE)

debug:
	@$(DEV) debug

longrun:
	@$(DEV) longrun

duo:
	@$(BASH) scripts/duo_test.sh $(SEC)

duo-video:
	@$(BASH) scripts/duo_video_test.sh $(SEC)

duo-disp:
	@$(BASH) scripts/duo_disp_test.sh $(SEC)

ut:
	@$(DEV) ut

loss:
	@$(BASH) scripts/loss_test.sh $(SEC)

release:
	@$(BASH) scripts/release.sh

clean:
	@$(DEV) clean

help:
	@$(DEV) help
