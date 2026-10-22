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
| `src/hal/` | **平台相关**驱动层：`hal_sdl.c`（Win/Linux 桌面）、`hal_fbdev.c`（Linux 真机，P1 待实现） |
| `src/main.c` | 应用入口 + 心跳循环（`lv_init` → `hal_init` → `ui_init` → `lv_timer_handler`） |
| `lv_conf.h` | **LVGL 配置总开关**（本仓库已修正矢量图相关宏，见"踩坑记录"第 4 条） |
| `CMakeLists.txt` | 构建脚本（lvgl 作为子模块引入） |
| `docs/` | 《LVGL环境搭建_从零到跑通.md》教程 + 《本地部署流程.md》 + 《跨平台移植说明.md》 |
| `scripts/` | 一键环境搭建脚本（Windows + MSYS2） |
| `lvgl/` | 上游 LVGL 库（**git submodule，不纳入本仓库**，首次使用需拉取） |

## 环境（已验证可用）

| 组件 | 版本 | 说明 |
|---|---|---|
| Windows | 10/11 x64 | |
| MSYS2 | 2026-09 版，装于 `C:\msys64` | 提供 Windows 原生工具链 |
| GCC | 16.2.0 | `mingw-w64-x86_64-gcc` |
| CMake | 4.4.4 | |
| Ninja | 1.13.2 | 比 mingw32-make 快 |
| SDL2 | 2.32.10 | PC 模拟器的显示/输入后端 |

## 快速开始

```bash
# 1. 拉取上游 LVGL（首次）
git submodule update --init --recursive lvgl

# 2. 构建（在 MSYS2 MINGW64 终端中执行）
mkdir -p build && cd build
# -DLVGL_HAL=sdl（默认，Win/Linux 桌面）或 -DLVGL_HAL=fbdev（Linux 真机）
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release ..
ninja

# 3. 运行
cd ../bin && ./main.exe        # 或直接双击 main.exe
```

产出：`bin/main.exe`（约 3.5MB），运行后弹出 320×480 的 LVGL Widgets demo 窗口。

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

## 踩坑记录（都是真踩的）

| # | 现象 | 根因 | 解法 |
|---|---|---|---|
| 1 | winget 装 MSYS2 卡 16 分钟 | 安装包走 GitHub 源，国内极慢 | 用清华 TUNA 镜像下 `msys2-x86_64-latest.exe`，静默安装 `install --root C:/msys64 --confirm-command` |
| 2 | `pacman` 所有 mingw 仓库 404 | 镜像目录层级不同：TUNA 是 `/msys2/mingw/$repo/`（多一层 `mingw`） | 改 `etc\pacman.d\mirrorlist.mingw` 为 `mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo/` |
| 3 | 首次 `pacman -Syu` 卡死，gpg 烧 14 分钟 CPU | Windows 上 gpg 密钥环初始化极慢 | 临时 `etc\pacman.conf` 中 `SigLevel = Never`（HTTPS 镜像可接受；要恢复校验改回 `Required`） |
| 4 | 编译报 `LV_USE_VECTOR_GRAPHIC requires (LV_USE_DRAW_SW and LV_USE_THORVG)...` | `lv_conf.h` 里 `LV_USE_VECTOR_GRAPHIC=1` 但 `LV_USE_THORVG=0`，宏之间有依赖 | `lv_conf.h` 中 `LV_USE_THORVG` 改 `1` |
| 5 | 编译中途 cc.exe 全家挂起（CPU 时间 0.06s） | 偶发进程死锁 | 杀掉后重跑 `ninja`，增量构建从断点继续 |
| 6 | 双击 main.exe 报「无法定位程序输入点 `clock_gettime64` 于 `libstdc++-6.dll`」 | 系统 PATH 里其他软件的**旧版**运行库被优先加载（DLL 搜索顺序） | 把 `SDL2.dll / libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll / zlib1.dll` 拷到 exe 同目录 —— 程序目录在所有搜索路径之前 |

> 第 6 条排查口诀：`ldd main.exe` 看依赖 → 程序目录优先于 PATH → 要么自带 DLL，要么静态链接（`-static`）。

## 学习路线

- [x] **Day 1**：环境搭建、编译、跑通官方 demo（Widgets）
- [x] **Day 2-a**：样式系统（命名样式 / 状态样式 / 明暗主题热切换 / 换样式表达状态）
- [ ] **Day 2-b**：flex / grid 布局（去掉绝对坐标）
- [ ] **Day 2-c**：多页面 + 页面栈 + 转场动画
- [ ] **Day 2-d**：中文点阵字体（lv_font_conv）+ 图标资源
- [ ] **Day 2-e**：内存与渲染量化（lv_mem_monitor / FPS / draw buffer 对比）
- [ ] **Day 2-f**：动画进阶（timeline 串联、透明度/缩放）
- [ ] **Day 3**：做"室内机主界面"demo —— 状态栏 + 呼叫弹窗 + 接听/挂断按钮 + 视频占位区，加入场动画

## 参考

- LVGL 官方文档：https://docs.lvgl.io/
- PC 模拟器工程（本仓库骨架来源）：https://github.com/lvgl/lv_port_pc_vscode
- 详细教程见 [`docs/LVGL环境搭建_从零到跑通.md`](docs/LVGL环境搭建_从零到跑通.md)

---

> 本仓库的工程骨架来自 [lvgl/lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode)（MIT），
> 在此之上做了配置修正（`lv_conf.h`）+ 环境适配（Windows/MSYS2/TUNA 镜像）+ 踩坑记录沉淀。
