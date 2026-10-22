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
| `docs/` | 教程 + 部署/移植文档 + **《产品规划_智能家居中控台.md》**（三大功能的架构与里程碑）；**《SIP注册与配置体系.md》**（交付形态 / 配置分层 / 注册状态机）；**《开源组件清单与占比.md》**（依赖清单、许可义务、可裁剪空间）；**《音频通路自测方案.md》**（不用声卡、不用手机，两条通路各自验收；第 9 节为双端互通 `make duo`） |
| `config/sip.ini.example` | **现场配置模板**（服务器/账号/端口/续期）；实际 `sip.ini` 不入库 |
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
| `make duo` | 本机起两个实例（室内机 + PC 客户端）对打，**双向音频各验一次**（SNR + 丢包 + 落盘） | 双端互通 |
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
| 12 | 界面中文显示成方框（"□光""空□""SIP 在□"） | LVGL 内置思源黑体只收了约 1500 常用字，`灯/调/风/插/线/备` 恰好不在里面 | `scripts/gen_font.py` 扫描源码文案生成**按需子集**，并关掉内置字库 |
| 13 | 新字库编译通过，但屏幕上**一个字都画不出来**，日志刷 `Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled` | lv_font_conv 默认输出 RLE 压缩字形，而 `lv_conf.h` 里解码开关是关的 | 开 `LV_USE_FONT_COMPRESSED`（更弱的 MCU 可用 `--no-compress` 重生成） |
| 14 | 通话一接通就自动挂断，RTP 只发出去 1 个包 | Windows WASAPI 首次打开音频设备耗时 1~3s，被算进了通话时长（`t_answer_ms` 早于开声卡） | 计时起点挪到 `rtp_start_media()` **之后** |
| 15 | 声音断续，播放欠载 400+ 次 | SDL 返回的是立体声设备，播放回调每 20ms 被要走 320 采样（单声道需求的两倍） | `SDL_OpenAudioDevice` 的 `allowed_changes` 传 0，强制 8k/单声道 |
| 16 | 两个实例在同一秒启动，第二个呼出去被判成"重传"，打不进对端 | `srand(time(NULL))` 做种子 → 同秒启动的两个进程 `rand()` 序列**完全一样** → Call-ID 撞车，服务器当成同一笔对话 | Call-ID 里再加**进程号 + 通话序号**（`%08x%08x-%u-%u@ip`），三者缺一都可能撞 |
| 17 | IP 直呼怎么都不响，换回走服务器就正常 | 呼出时 INVITE 一律发到 `SIP_SERVER`，而不是 Request-URI 里的 host —— 恰好"服务器地址=对端地址"时才通。UDP 无连接，发错地址**没有任何报错**，只表现为对方不响 | 新增 `uri_addr()` 从 Request-URI 解析目的地址，`send_msg_to()` 显式指定目标；CANCEL 也必须发同一地址 |
| 18 | 手机 Linphone 一点呼叫就挂，日志里连 INVITE 都没走完 | SDP **answer 的 m-line 条数必须与 offer 一致**（RFC 3264 §6），不支持的类型用 `m=video 0` 拒绝。我们原来只回 `m=audio`，带视频 offer 的软电话判为 SDP 非法 | `build_sdp()` 末尾补 `m=video 0 RTP/AVP 0` + `a=inactive`；将来收视频改成 `recvonly` + 回显 offer 的 pt |
| 19 | 不填 `SIP_SERVER` 的点对点模式下，双方都静音、信令却全对 | `detect_local_ip()` 原本朝服务器地址探测网卡，没服务器时探测失败 → `s_local_ip` 停在 `127.0.0.1` → SDP 里 `c=IN IP4 127.0.0.1` 发给对端，对端把音频**发给自己** | 无服务器时回退到 `8.8.8.8:53` 探测默认路由（UDP connect 不发包）；另加 `SIP_LOCAL_IP` 人工覆盖，**板子多网卡必用** |
| 20 | 手机开了 ICE 时，板子发出去的音频对方收不到 | offer 的 `c=` 行是 `0.0.0.0`，NAT 后端口也会变 | 对称 RTP（RFC 4961）：以**实收 RTP 包的源地址**校正回发目标，日志打 `[rtp] peer learned` |
| 21 | 解码耗时测出来是 `0.00 ms`，比理论值低三个数量级 | FFmpeg 的 H.264 解码器在 `avcodec_send_packet()` 里真正做解码，`avcodec_receive_frame()` 只是从内部队列取帧（耗时≈0）。只给 receive 计时当然测不到 | 计时区间必须**包住 send + receive**，见 `tools/h264_probe` |
| 22 | 手机配好账号后一直显示"注册失败"，日志里每秒一条 REGISTER | UAS 收到不支持的方法（REGISTER/MESSAGE/SUBSCRIBE）**静默丢弃**。RFC 3261 §8.2.1 要求回 405/501 终态响应；不回的话对端按 Timer E/G 无限重传，看起来像"板子不响应" | 兜底分支统一回 `405 Method Not Allowed` + `Allow` 头；`INFO`（带内 DTMF）回 200 |
| 23 | 用同一份 WAV 当音源跑环回，每 10 秒听到一声"咔哒" | 音源**结尾停在大幅值、开头从 0 起**，循环时必然有一个跳变（实测相邻采样跳 4104）。而丢包的症状也是"咔哒" —— 差点去查一个不存在的问题 | 生成脚本对首尾各做 10ms 淡入淡出，循环点最大跳变降到 208（正常波形内）。**测试信号自己的毛病会被当成被测对象的毛病** |
| 24 | 两端跑同一份编解码做环回，SNR 很好看，但不知道代码对不对 | 两端同源 → 对称的 bug 会**错得一模一样**，自己跟自己比永远是对的。用 `ffmpeg -c:a pcm_alaw -f alaw` 做独立参照，这才发现 65536 个输入里有 964 个码字不同（1.5%，只差 1 个量化级，集中在段边界） | 结论不是 bug：Sun 参考实现 vs ffmpeg 的就近取整，两种都合法，保留 Sun 那套（网关跑的是它）。**方法**比结论重要 |

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
| SIP 音视频对讲 | **oSIP 客户端全链路**（注册/认证/双向呼叫/取消/G.711 音频，真服务器实测）+ 对讲页 | ALSA 替换 SDL + H.264 硬解 + 板端 AEC |

**里程碑**：M1 中控台骨架（flex/grid+设备模型+事件总线）→ M2 语音链路 → M3 SIP 对讲 → M4 中文字体+图标 → M5 量化压测 → M6 上板（fbdev/evdev/audio + 交叉编译）。

## 多客户定制（OEM）

> 详细操作见 [`docs/OEM定制指南.md`](docs/OEM定制指南.md)。

室内机是白牌产品：同一套软硬件卖给不同客户，配色、文案、首页结构都不同。
本工程把差异全部收进 `src/oem/`，UI 代码里**没有一个品牌字面量**：

```c
const oem_profile_t *o = oem();
lv_label_set_text(title, oem_text(OEM_TXT_PRODUCT));      /* 文案 */
lv_obj_set_style_text_color(icon, lv_color_hex(oem_color(OEM_COLOR_PRIMARY)), 0);  /* 颜色 */
lv_obj_set_size(cell, o->layout.card_w, o->layout.card_h); /* 尺寸 */
if (o->layout.home_style == OEM_HOME_LIST) build_list(...); /* 结构 */
```

三层覆盖，从粗到细：

| 方式 | 命令 | 要不要重编译 |
|---|---|---|
| 环境变量选 profile | `OEM_PROFILE=alt ./bin/main.exe` | 否（两套都编进同一份二进制） |
| `oem.ini` 微调颜色/文案/布局 | 改 `config/oem.ini` 后重启 | 否 |
| 新增客户 profile | 新增 `oem_xxx.c` + 注册一行 | 是 |

已实测两套差异很大的配置（宫格浅色 vs 列表石墨金），同一份二进制切换，均 0 警告 0 断言。

**新增中文文案后必须重跑** `python scripts/gen_font.py`，否则字库缺字会显示方框。

## 交付形态：目标机上没有 Python

产品最终跑在 RK3308 上，**板子里没有解释器，车间/客户现场也没有 PC**。
所以代码库按"能不能上板"分成三类，边界写在 `docs/SIP联调环境.md` 第 0 节：

| 类别 | 例子 | 上板吗 |
|---|---|---|
| **产品代码** | `src/**/*.c` | 是（交叉编译成 `main`） |
| **板端工具** | `tools/sip_echo/`（纯 C 应答器，只依赖 libosip2） | 是，可一起交叉编译 —— 板子自己测自己 |
| **PC 工具** | `scripts/gen_font.py`、`scripts/fake_sip_*.py`、`scripts/dev.sh` | 否（构建期 / 开发期） |

两条硬约束：

1. **构建不吃 Python**：`CMakeLists.txt` 里没有一处 `execute_process(python ...)`；
   字库等生成物都是**提交进仓库的普通 `.c`**，板子上 `cmake -B build && ninja` 即可。
   `gen_font.py` 只在**新增中文文案**时于 PC 上跑一次，生成结果随代码一起提交。
2. **运行不吃 Python**：联调对端从 Python 假服务器换成 `tools/sip_echo`（300 行 C），
   配合 `SIP_TONE=1`（无麦克风合成 1kHz 音源）和 `SIP_AUTOCALL` / `SIP_AUTOHANGUP`，
   全程脚本化，无需人工点击：

```bash
make echo                                                                    # 终端 A：应答器
SIP_SERVER=127.0.0.1 SIP_PORT=5090 SIP_AUTOCALL=echo SIP_AUTOHANGUP=6 \
SIP_TONE=1 make run                                                          # 终端 B：被测端
# → [rtp] stats rx=298 tx=299 dur=6s (pps=49.8)
```

**音频甚至可以完全不碰声卡**：`SIP_AUDIO_SRC=file` 用 WAV 顶替麦克风、
`SIP_AUDIO_SINK=file` 把收到的音频存成 WAV，两端各跑一份同一个二进制，
就能把"发"和"收"两条通路分开验收，再用 SNR 比对代替"我听着像是对的"。
详见 [`docs/音频通路自测方案.md`](docs/音频通路自测方案.md)。

```bash
# 12 秒通话，实测 tx=600 / rx=600 / lost=0 / loss=0.00%，落盘 96000 采样 = 12.00 秒
SIP_AUDIO_SRC=file SIP_AUDIO_SRC_FILE=test_8k.wav SIP_AUDIO_SINK=file \
SIP_AUDIO_SINK_FILE=rx.wav SIP_NO_REGISTER=1 make run
python scripts/audio_compare.py test_8k.wav rx.wav     # → SNR 43.1 dB
```

## 学习路线

- [x] **Day 1**：环境搭建、编译、跑通官方 demo（Widgets）
- [x] **Day 2-a**：样式系统（命名样式 / 状态样式 / 明暗主题热切换 / 换样式表达状态）
- [x] **Day 2-e**：内存与性能量化（`lv_mem_monitor` / FPS / 主循环耗时 / 30 秒压测零泄漏）
- [x] **Day 2-g**：一键构建（`make build` / `make run` / 内存与压测变体）
- [x] **产品规划**：智能家居中控台三大功能拆解（`docs/产品规划_智能家居中控台.md`）
- [x] **M1**：中控台骨架 —— flex/grid 主页 + **三级信息架构**（分类宫格 → 房间页签 → 设备详情）+ 设备模型 + 事件总线 + ini 持久化
- [ ] **M2**：语音链路 —— voice_engine 四实现（mock/**sherpa-onnx**/思必驰/在线）+ 唤醒状态机 + 语音浮层 + 中文意图解析（**当前用 app_mock 模拟 ASR 事件**）
- [x] **M3**：SIP 对讲 —— oSIP 客户端（REGISTER/Digest/续期、INVITE 双向、ACK/BYE/CANCEL、非 2xx ACK、**G.711 真实音频**）+ `ui_call.c` 对讲页；真服务器 miniSIPServer 实测：**50pps/64kbps，收发 250↔250，欠载 0**；另有板端自测工具 `tools/sip_echo`（纯 C 应答器，陪产品一起上板）；音频收发两条通路已用 file↔file 方式独立验收（SNR 43.1 dB、丢包 0），并用 **`make duo` 双进程对打**做了真双向验证（室内机→客户端 / 客户端→室内机，各 12s：tx/rx 600↔600、loss 0.00%、SNR 43.1 dB）。见 `docs/SIP联调环境.md`、`docs/音频通路自测方案.md`（第 9 节）
- [x] **M4（部分）**：中文按需子集字库（`scripts/gen_font.py` → `src/ui/fonts/`，212 汉字 × 16/24px），**修掉了"灯/调/风/插/线"显示成方框的问题**
- [x] **OEM 定制层**（超出原计划）：`src/oem/` 一套接口 + 两套 profile，运行期 `OEM_PROFILE=alt` 热切换宫格/列表两套首页，见 `docs/OEM定制指南.md`
- [ ] **M5**：全功能量化压测（每个里程碑过一遍 stress，出 stats.csv 曲线）
- [x] **交付形态（产品化第一步）**：配置从"纯环境变量"升级为 **`sip.ini` 三层覆盖**
  （文件 → 环境变量），实测零环境变量启动即可完成注册；注册可靠性补齐
  **32s 事务超时 + 指数退避 1→60s + 续期 50%~80% 随机 + 按 CSeq 区分注册/呼叫响应**。
  见 `docs/SIP注册与配置体系.md`、`config/sip.ini.example`
- [x] **固件裁剪**：关掉未用字号/ThorVG/图片解码器/示例与 demo，目标文件体积 -54%，
  `main.exe` **2.79 MB → 1.82 MB（-35%）**，通话与注册回归通过。
  见 `docs/开源组件清单与占比.md`
- [ ] **M6（等板子）**：`hal_fbdev.c`/`hal_audio.c`（ALSA）+ 交叉编译 + 思必驰/eXosip 实装
- [ ] **M3 剩余**：视频（H.264 解码 + `lv_image` 显示）—— 室内机"可视"对讲的最后一块
  - 测试方案已定：**手机装 Linphone 点对点直呼板子**，不架服务器，手机冒充门外机发声视频，
    板子 `recvonly` 只收不看。分 P0 语音 / P1 `ffplay` 验流 / P2 解码上屏三段。
    见 [`docs/音视频对讲测试方案.md`](docs/音视频对讲测试方案.md)
  - 实现方案已定：音频 G.711 自研查表（无库）；视频 `libavcodec` 软解 / Rockchip MPP 硬解，
    **层抽象成 `vdec` 双后端**，只用 libavcodec 不用 libavformat。见
    [`docs/音视频编解码实现方案.md`](docs/音视频编解码实现方案.md)
  - 工具已就位：`tools/h264_probe`（纯 C + libavcodec，解裸流、量单帧耗时、导 RGB565），
    实测 480p 0.39ms/帧、720p 1.78ms/帧（PC 单线程），可交叉编译上板复核

## 参考

- LVGL 官方文档：https://docs.lvgl.io/
- PC 模拟器工程（本仓库骨架来源）：https://github.com/lvgl/lv_port_pc_vscode
- 详细教程见 [`docs/LVGL环境搭建_从零到跑通.md`](docs/LVGL环境搭建_从零到跑通.md)

---

> 本仓库的工程骨架来自 [lvgl/lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode)（MIT），
> 在此之上做了配置修正（`lv_conf.h`）+ 环境适配（Windows/MSYS2/TUNA 镜像）+ 踩坑记录沉淀。
