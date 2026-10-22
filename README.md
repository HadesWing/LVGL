# LVGL 学习仓库

> 从零搭建 LVGL 开发环境 → 跑通 PC 模拟器 → 做出带屏设备界面 demo 的完整记录。
> 目标场景：嵌入式 Linux（RK3308 / 智能家居中控屏 / 可视对讲室内机）上的 LVGL 应用开发。

**仓库地址**：https://github.com/HadesWing/LVGL

> 本机推送说明：受网络代理影响，HTTPS 推送到 github.com 会失败（`CONNECT tunnel failed, 502`）。
> 本机已把远端配成 SSH（`git@github.com:HadesWing/LVGL.git`），并设置了全局改写规则
> `url.git@github.com:.insteadOf = https://github.com/`，以后 `git push` 会自动走 SSH。
> 前提：`~/.ssh/id_rsa` 已加入 GitHub 账号（本机已验证 `ssh -T git@github.com` 通过）。

## 这是什么

| 目录 | 内容 |
|---|---|
| `src/ui/` | **平台无关**界面层：`ui_theme.c`（命名样式 + 明暗主题）、`ui_indoor.c`（室内机主界面） |
| `src/app/` | **平台无关**应用层：`app_events.c`（事件总线，跨线程→主线程收口）、`app_devices.c`（设备模型+场景+持久化）、`app_stats.c`（FPS/内存埋点）、`app_mock.c`（设备事件模拟 + 压测） |
| `src/hal/` | **平台相关**驱动层：`hal_sdl.c`（Win/Linux 桌面）、`hal_fbdev.c`（Linux 真机，P1 待实现） |
| `src/main.c` | 应用入口 + 心跳循环（`lv_init` → `hal_init` → `ui_init` → `lv_timer_handler`） |
| `Makefile` | **一键入口**（转发给 `scripts/dev.sh`） |
| `build.cmd` | 免 PATH 的等价入口（`build.cmd build` / `build.cmd run` / `build.cmd stress` …） |
| `scripts/dev.sh` | 真正的构建/运行/压测脚本（MSYS2 环境、避开中文路径、并发生死锁规避） |
| `lv_conf.h` | **LVGL 配置总开关**（内存池大小、断言、性能监视器，见"踩坑记录"第 4 条） |
| `CMakeLists.txt` | 构建脚本（lvgl 作为子模块引入；`-DLV_MEM_SIZE` / `-DLV_DEBUG_ASSERT` / `-DLVGL_HAL`） |
| `docs/` | 教程 + 部署/移植文档 + **《产品规划_智能家居中控台.md》**（三大功能的架构与里程碑） |
| `scripts/setup_windows_msys2.sh` | 一键环境搭建（Windows + MSYS2 + 国内镜像） |
| `lvgl/` | 上游 LVGL 库（**git submodule，不纳入本仓库**，首次使用需拉取） |

## 分层设计（这份代码为什么能直接搬到板子上）

```
        ┌─────────────────────────────────────────┐
        │  src/ui/   界面层（100% 平台无关）        │  ← 换板子不用改
        │  ui_indoor.c / ui_theme.c               │
        ├─────────────────────────────────────────┤
        │  src/app/  应用层（100% 平台无关）        │  ← 换板子不用改
        │  app_stats.c / app_mock.c               │
        ├─────────────────────────────────────────┤
        │  src/hal/hal.h   唯一的平台接口          │
        │    lv_display_t * hal_init(w, h)        │
        ├───────────────────┬─────────────────────┤
        │ hal_sdl.c         │ hal_fbdev.c         │  ← 只有这一层换
        │ Win/Linux 桌面     │ /dev/fb0 + evdev    │
        └───────────────────┴─────────────────────┘
```

编译期用 `-DLVGL_HAL=sdl|fbdev` 切换，`ui/`、`app/` 一行都不用动 ——
这就是"业务逻辑先在本机做完，等板子到了只写驱动"的前提。


## 环境（已验证可用）

| 组件 | 版本 | 说明 |
|---|---|---|
| Windows | 10/11 x64 | |
| MSYS2 | 2026-09 版，装于 `C:\msys64` | 提供 Windows 原生工具链 |
| GCC | 16.2.0 | `mingw-w64-x86_64-gcc` |
| CMake | 4.4.4 | |
| Ninja | 1.13.2 | 比 mingw32-make 快 |
| SDL2 | 2.32.10 | PC 模拟器的显示/输入后端 |

## 快速开始（一条命令）

```bash
make            # 编译 + 运行（最常用）
make build      # 只编译
make run        # 只运行
```

就这三条，不需要记 cmake / ninja 的参数，也不用先进 MSYS2 终端。

> **`make` 提示"不是内部或外部命令"？** 说明当前终端是加 PATH 之前打开的。
> 两种解法：① 关掉重开一个终端；② 直接用 `build.cmd build`（等价，且不依赖 PATH）。

`Makefile` 只是转发壳，真正干活的是 `scripts/dev.sh`（原因见"踩坑记录"第 7、8 条）。
常用变体：

| 命令 | 干什么 | 用在哪 |
|---|---|---|
| `make stats` | 每 5 秒打印 FPS / 内存 / 主循环耗时，明细写 `bin/stats.csv` | 性能量化 |
| `make stress` | 每 200ms 建删 8 个控件 + 切主题 + 触发来电，查**内存泄漏与碎片** | 稳定性自证 |
| `make mem64` | 用 64KB 内存池重编再跑（模拟 RK3308 这类小内存设备），产出独立的 `bin/main64.exe` | 内存预算 |
| `make debug` | 打开 LVGL 断言（越界、野指针、样式/对象一致性） | 查 bug |
| `make longrun` | 30 分钟长稳，到点自动打印汇总并退出 | 长稳 |
| `make rebuild` / `make clean` | 全量重编 / 删构建目录 | |

两个环境变量随时可用（Windows 的 cmd 里用 `set LVGL_RUN_SEC=60` 后再跑）：

| 变量 | 含义 |
|---|---|
| `LVGL_STATS=1` | 写 `stats.csv` + 打印采样（每行：秒, FPS, 循环均值, 循环峰值, 内存占用, 峰值, 占用率, 碎片率, 最大连续空闲） |
| `LVGL_RUN_SEC=N` | 跑 N 秒自动退出并打印汇总（自动化测试用，不设就是一直跑） |

首次使用仍需拉一次上游库：

```bash
git submodule update --init --recursive lvgl
```

产出：`bin/main.exe`（约 3.5MB），运行后弹出 320×480 的室内机主界面窗口。


## LVGL 骨架（面试必答）

`src/main.c` 只有 35 行，却包含了 LVGL 应用的全部骨架：

```c
lv_init();              // 1. 初始化库（内存池、样式、对象系统）
sdl_hal_init(320,480);  // 2. 注册显示驱动 + 输入驱动 + tick 源
lv_demo_widgets();      // 3. 建界面（本质是往 lv_screen_active() 上挂对象树）

while (1) {             // 4. 心跳循环：协作式单线程调度器
    uint32_t ms = lv_timer_handler();   // 处理定时器/动画/刷新/输入
    usleep(ms * 1000);
}
```

**移植到嵌入式板子 = 只换第 2 步的实现**：

| PC 模拟 | RK3308 / 嵌入式 Linux |
|---|---|
| `lv_sdl_window_create()` | `/dev/fb0`（fbdev）或 DRM，`mmap` 后按行 memcpy |
| `lv_sdl_mouse_create()` | `/dev/input/eventX`（evdev 触摸） |
| SDL 定时 | `lv_tick_set_cb()` 绑定 `clock_gettime` / RTOS tick |
| 内存无限 | `LV_MEM_SIZE` 要预算，`lv_mem_monitor()` 盯碎片 |
| 有 GPU 可加速 | 无 GPU：纯软件渲染，脏矩形 + RGB565 + draw buffer ≈ 1/10 屏 |

## 内存 / 性能 / 稳定性：怎么量，量出了什么

嵌入式做界面，功能能跑只是及格线。面试官真正会问的是三句：
**"内存多少？帧率多少？跑一天会不会崩？"** —— 这三句必须能拿出数字回答。

### 三个指标从哪来

| 指标 | 采集方式 | 代码位置 |
|---|---|---|
| 帧率 | 给 display 挂 `LV_EVENT_RENDER_READY` 回调，每刷完一帧 +1，每秒算差 | `app_stats.c: on_render_ready` |
| 内存 | `lv_mem_monitor(&mon)`：`total_size / free_size / max_used / used_pct / frag_pct / free_biggest_size` | `app_stats.c: sample_cb` |
| 主循环耗时 | 包头包尾夹 `lv_timer_handler()`，`QueryPerformanceCounter`（Win）/ `clock_gettime(CLOCK_MONOTONIC)`（Linux） | `main.c` 主循环 + `app_stats.c` |

为什么用 `RENDER_READY` 而不是自己数 `lv_timer_handler` 次数：
**`lv_timer_handler` 被调用 ≠ 真的刷了屏**。LVGL 只在有脏区时才渲染，空闲时 handler 空转。
`RENDER_READY` 是库内部认定的"这一帧真的画完并 flush 了"，是唯一可靠的帧边界。

### 实测数据（320×480，RGB565，Release，PC 模拟）

**空闲**（静态界面，只有 1 个 1s 定时器在采样）：

```
[stats]  5s  FPS  15  loop avg 104 us / max 5939 us  mem 22200/1042624 B (3%)  peak 28496  frag 1%
[stats] 10s  FPS   5  loop avg  51 us / max 4316 us  mem 22344/1042616 B (3%)  peak 28496  frag 1%
```

> FPS 只有 5~15 是**正常的**，不是卡。界面不动就没有脏区，LVGL 不会重绘。
> 真正该看的是"有动画/触摸时能不能顶住"和"循环耗时占多大比例"。

**压测**（每 200ms 建 8 个按钮、切一次主题、触发一次来电，持续 30 秒 ≈ 1200 个控件建删）：

```
[stats]  5s  FPS  30  loop avg 195 us / max 3017 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
[stats] 10s  FPS  28  loop avg 230 us / max 5082 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
[stats] 15s  FPS  30  loop avg 173 us / max 2958 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
[stats] 20s  FPS  29  loop avg 176 us / max 2036 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
[stats] 25s  FPS  30  loop avg 143 us / max 1556 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
[stats] 30s  FPS  29  loop avg 274 us / max 2955 us  mem 22352/1042608 B (3%)  peak 28424  frag 1%
```

**同样的压测，换 64KB 内存池（`make mem64`，这才是设备上的真实配置）**：

```
[stats]  5s  FPS  30  loop avg 290 us / max 3369 us  mem 22296/60608 B (37%)  peak 28424  frag 2%
[stats] 10s  FPS  30  loop avg 235 us / max 7948 us  mem 22296/60608 B (37%)  peak 28424  frag 2%
[stats] 20s  FPS  30  loop avg 361 us / max 3465 us  mem 22296/60608 B (37%)  peak 28424  frag 2%
[stats] 30s  FPS  29  loop avg 138 us / max 1520 us  mem 22296/60608 B (37%)  peak 28424  frag 2%
```

**结论（这三行就是面试答案）**：

1. **无泄漏**：30 秒 150 轮建删，内存占用**始终 22352 B，一个字节都没涨**；
2. **无碎片**：`frag 1%` 全程不变，`free_biggest_size` 稳定 —— LVGL 内存池的首次适配 + 相邻块合并扛住了反复分配；
3. **有性能余量**：最坏一帧的主循环耗时 5ms（含首帧初始化），稳态 150~270µs。
   按 RK3308（4×A35 @1.0GHz，纯软件渲染，无 GPU）粗略折算到 1/5 单核性能，
   仍然是"30 FPS 下每帧 33ms 里只吃掉 1~2ms"的量级 —— 余量足够挂语音 SDK 和 SIP 栈。

> ⚠️ 这三条只对"内存池内部"成立。**内存池之外的开销（图片解码、字体、SDK 自己的 malloc）
> 不归 `lv_mem_monitor` 管**，上板后要配 `/proc/<pid>/status` 的 VmRSS 一起看。这句话面试时主动说出来，是加分项。

### 内存池要开多大：实测阶梯

同一份界面代码，只改 `LV_MEM_SIZE` 重编，结果（Release，320×480）：

| `LV_MEM_SIZE` | 实际池大小 | 稳态占用 | 峰值 | 碎片 | 结论 |
|---|---|---|---|---|---|
| 1 MB（默认） | 1042616 B | 22.2 KB (3%) | 28.5 KB | 1% | 桌面调试用，不代表设备 |
| **64 KB** | 60608 B | 22.3 KB (**37%**) | 28.4 KB (46%) | 1~2% | ✅ **推荐值**，压测 30 秒零增长 |
| 40 KB | 36040 B | 22.4 KB (62%) | 28.5 KB (**79%**) | 3% | ⚠️ 能跑，但只剩 13 KB 连续空闲，没余量 |
| 32 KB | — | — | — | — | ❌ 编不过：LVGL 官方 demo 有编译期硬检查（<38 KB 直接 `#error`） |

**怎么读这张表**：

- 真实峰值 28.5 KB 里包含了压测时 8 个动态按钮的瞬时分配，静态界面只要 22 KB；
- 工程上留 2 倍余量 → **`LV_MEM_SIZE = 64KB`** 是这块屏的合理值（RK3308 的 DDR 有 128~512MB，
  划 64~128KB 给 LVGL 池毫无压力，但**这块内存是静态数组，会实打实占 BSS**，不能乱开）；
- 32 KB 那条不是运行时崩溃，是 **demo 的编译期下限检查**挡住了 —— 说明"官方 demo 的内存下限 ≠ 你的应用的下限"，
  上板时把 `LV_USE_DEMO_*` 全关掉，既省 flash 也绕过这个检查。

### 一条命令复现

```bash
make stress                 # 30 秒压测（Ctrl+C 随时停）
LVGL_STATS=1 make stats     # 边跑边存 stats.csv，用 Excel 画曲线
make mem64                  # 64KB 内存池
make mem SIZE=40960         # 任意大小，找边界
make debug                  # 开断言跑，查越界/野指针
```



## 踩坑记录（都是真踩的）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | winget 装 MSYS2 卡 16 分钟 | 安装包走 GitHub 源，国内极慢 | 用清华 TUNA 镜像下 `msys2-x86_64-latest.exe`，静默安装 `install --root C:/msys64 --confirm-command` |
| 2 | `pacman` 所有 mingw 仓库 404 | 镜像目录层级不同：TUNA 是 `/msys2/mingw/$repo/`（多一层 `mingw`） | 改 `etc\pacman.d\mirrorlist.mingw` 为 `mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo/` |
| 3 | 首次 `pacman -Syu` 卡死，gpg 烧 14 分钟 CPU | Windows 上 gpg 密钥环初始化极慢 | 临时 `etc\pacman.conf` 中 `SigLevel = Never`（HTTPS 镜像可接受；要恢复校验改回 `Required`） |
| 4 | 编译报 `LV_USE_VECTOR_GRAPHIC requires (LV_USE_DRAW_SW and LV_USE_THORVG)...` | `lv_conf.h` 里 `LV_USE_VECTOR_GRAPHIC=1` 但 `LV_USE_THORVG=0`，宏之间有依赖 | `lv_conf.h` 中 `LV_USE_THORVG` 改 `1` |
| 5 | 编译中途 cc.exe 全家挂起（CPU 时间 0.06s 就不再涨） | MSYS2 在 -j8 满载下偶发 fork/exec 死锁 | 杀进程重跑；`scripts/dev.sh` 已把并发降到 `-j6` |
| 6 | 双击 main.exe 报「无法定位程序输入点 `clock_gettime64` 于 `libstdc++-6.dll`」 | 系统 PATH 里其他软件的**旧版**运行库被优先加载（DLL 搜索顺序） | 把 `SDL2.dll / libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll / zlib1.dll` 拷到 exe 同目录 —— 程序目录在所有搜索路径之前 |
| 7 | `make build` 报 `C compiler is not able to compile a simple test program`，没有任何其它信息 | make 拉起的子进程 PATH 不完整时，**gcc 找不到 cc1/as 会静默失败** | Makefile 只做转发，真正构建放到 `scripts/dev.sh` 里、在 MSYS2 环境中执行 |
| 8 | 编译全过，链接必失败：`cannot open output file C:\...\????\...\main.exe` | 工程在中文目录（`...\面试\...`）下，cmake 生成链接命令要过 `cmd.exe`，中文在非 UTF-8 代码页被替换成 `?`；编译那几步是 ninja 直连 gcc，所以只有链接挂 | `dev.sh` 自动建 ASCII 目录联接 `C:\lvgl_dev` → 工程，所有路径变成 `C:/lvgl_dev/...` |
| 9 | 控制台里 `printf` 的中文全是乱码 | 源码是 UTF-8，Windows 控制台默认代码页 936(GBK) | `main()` 开头 `SetConsoleOutputCP(CP_UTF8)` |
| 10 | VSCode 终端提示符明明写着 `MINGW64`，`make` 却 `command not found` | 那其实是 **Git Bash**（Git for Windows 的提示符也叫 MINGW64，极易认错），不是 MSYS2；Git Bash 里没有 make | 把 `C:\msys64\mingw64\bin\make.exe` 拷一份到 `Git\usr\bin\make.exe`；`dev.sh` 已做双宿主兼容（Git Bash 的 `/etc/profile` 会重置 PATH，绕过不 source） |
| 11 | 从 Git Bash 走 make，链接报 `Cannot create temporary file in C:\Windows\: Permission denied` | TMP 环境变量在 Git Bash → make → msys2 bash 链路上丢了，链接器退回 C:\Windows 建临时文件 | `dev.sh` 里 `export TMP=$(cygpath -w /tmp)` 归一化 |

> 第 6 条排查口诀：`ldd main.exe` 看依赖 → 程序目录优先于 PATH → 要么自带 DLL，要么静态链接（`-static`）。
> 第 7、8 条是同一类问题：**Windows 上构建失败的根因经常不在代码，而在"谁替你启动了编译器"**。

## 产品方向：室内机 → 智能家居中控台

> 详细规划见 [`docs/产品规划_智能家居中控台.md`](docs/产品规划_智能家居中控台.md)。
> 一句话：手能点（中控台）、嘴能说（唤醒+开关指令）、门铃能响（SIP 对讲），全部业务逻辑先在本机做完。

**三大功能**：

| 功能 | 本机做到 | 等板子做 |
|---|---|---|
| 智能家居中控台 UI | flex/grid 主页 + 设备九宫格 + 场景条 + 页面栈 | 真实设备协议（MQTT/RS485） |
| 语音唤醒 + 指令理解 | **voice_engine 统一接口**：mock + **sherpa-onnx 免费离线引擎（真麦克风，现在就能调）**，意图解析 + 唤醒状态机全本机完成 | 换思必驰离线 SDK（板上量产主路径），接口不动 |
| SIP 音视频对讲 | 呼叫状态机 + 来电横幅 + 对讲页，mock VTO 来电 | oSIP/eXosip 实装 + ALSA 音频 + 硬解视频 |

**里程碑**：M1 中控台骨架（flex/grid+设备模型+事件总线）→ M2 语音链路 → M3 SIP 对讲 → M4 中文字体+图标 → M5 量化压测 → M6 上板（fbdev/evdev/audio + 交叉编译）。

## 学习路线

- [x] **Day 1**：环境搭建、编译、跑通官方 demo（Widgets）
- [x] **Day 2-a**：样式系统（命名样式 / 状态样式 / 明暗主题热切换 / 换样式表达状态）
- [x] **Day 2-e**：内存与性能量化（`lv_mem_monitor` / FPS / 主循环耗时 / 30 秒压测零泄漏）
- [x] **Day 2-g**：一键构建（`make build` / `make run` / 内存与压测变体）
- [x] **产品规划**：智能家居中控台三大功能拆解（`docs/产品规划_智能家居中控台.md`）
- [x] **M1**：中控台骨架 —— flex/grid 主页（状态栏/房间页签/设备卡片网格/场景条）+ 设备模型 + 事件总线；页面栈留 M3 对讲页接入
- [ ] **M2**：语音链路 —— voice_engine 四实现（mock/**sherpa-onnx**/思必驰/在线）+ 唤醒状态机 + 语音浮层 + 中文意图解析
- [ ] **M3**：SIP 对讲 —— 呼叫状态机 + 来电横幅 + 对讲页
- [ ] **M4**：中文点阵字体（lv_font_conv）+ 图标资源
- [ ] **M5**：全功能量化压测（每个里程碑过一遍 stress，出 stats.csv 曲线）
- [ ] **M6（等板子）**：`hal_fbdev.c`/`hal_audio.c` + 交叉编译 + 思必驰/eXosip 实装

## 参考

- LVGL 官方文档：https://docs.lvgl.io/
- PC 模拟器工程（本仓库骨架来源）：https://github.com/lvgl/lv_port_pc_vscode
- 详细教程见 [`docs/LVGL环境搭建_从零到跑通.md`](docs/LVGL环境搭建_从零到跑通.md)

---

> 本仓库的工程骨架来自 [lvgl/lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode)（MIT），
> 在此之上做了配置修正（`lv_conf.h`）+ 环境适配（Windows/MSYS2/TUNA 镜像）+ 踩坑记录沉淀。
