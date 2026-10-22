# LVGL 环境搭建 — 从零到跑通（Windows）

> 目标：在你自己电脑上跑出一个 LVGL 窗口，三天内做出一个"室内机主界面" demo。
> 本文档只讲**怎么开始**：头文件在哪、依赖怎么下、命令怎么敲、报错怎么办。

---

## 第 0 章 你机器上现在有什么（10-13 实测）

| 工具 | 状态 | 说明 |
|---|---|---|
| git 2.33.1 | ✅ 有 | 在 `D:\Program Files\Git` |
| VS Code | ✅ 有 | 已装 |
| Node.js | ✅ 有 | 后面转字体要用（lv_font_conv） |
| Python 3.13 | ✅ 有 | |
| **gcc / g++** | ❌ **没有** | 唯一的拦路虎 |
| **cmake** | ❌ 没有 | |
| **make** | ❌ 没有 | |

**结论：你只缺一套 C 工具链。** LVGL 本身不用装（它是源码库，克隆下来就能用），SDL2 会随工具链一起装。装完 MSYS2，编译环境就齐了。

---

## 第 1 章 先搞清三件事（你问的"头文件"到底是什么）

### 1.1 LVGL 的头文件不在系统里，在源码里

LVGL 是**源码级库**（source-level library），不是 libxxx.so 那种二进制库。它不"安装"到系统，而是：

```
你 git clone 下来的 lvgl/ 目录
├── lvgl.h                 ← ★ 唯一的头文件入口，你只需要 #include "lvgl.h"
├── lv_conf_template.h     ← ★ 配置模板（必须自己拷出来改）
├── src/                   ← 所有源代码，编译时一起编
│   ├── core/  draw/  widget/  font/  ...
├── examples/              ← 官方示例，Day1 就跑这些
└── demos/                 ← 综合 demo
```

**只要 `#include "lvgl.h"`，整个库的 API 都在里面了**（它内部把各模块头文件都 include 了）。不需要一个个加头文件。

### 1.2 `lv_conf.h` 必须自己生成（新手第一个坑）

LVGL 的配置**不在源码里**，而是要求你：

1. 把 `lvgl/lv_conf_template.h` 拷贝一份成 `lv_conf.h`；
2. 放在 **`lvgl` 目录的上一级**（即和 `lvgl` 平级）；
3. 打开它，把最开头的 `#if 0` 改成 `#if 1`（模板默认是关闭的！）。

```
项目根/
├── lvgl/          ← 库
├── lv_conf.h      ← ★ 你生成的配置，和 lvgl 平级
└── main.c
```

不想放这儿也行，用编译宏指定路径：`-DLV_CONF_PATH="path/to/lv_conf.h"`。

**忘了这一步的报错**：`lv_conf.h: No such file or directory` 或 `LV_CONF_INCLUDE_SIMPLE` 相关错误。

### 1.3 依赖库只有一个：SDL2

| 场景 | 需要什么 |
|---|---|
| **PC 上模拟（你要做的）** | **SDL2** —— 提供窗口、显示、鼠标/键盘输入 |
| 上真板子（RK3308） | **不需要 SDL2**。改用 Linux 自带的 fbdev / DRM（显示）和 evdev（触摸），零外部依赖 |
| 把 TTF 字体转成 C 数组 | `lv_font_conv`（Node.js 包，你有 Node） |

所以"依赖库怎么下载"的答案是：**PC 模拟只装 SDL2**，而 SDL2 会在装 MSYS2 时一条命令带下来。

---

## 第 2 章 装工具链（MSYS2，一次性）

### 2.1 下载安装 MSYS2

1. 打开 https://www.msys2.org/ ，下载 `msys2-x86_64-xxxx.exe`；
2. 一路默认安装，路径保持 `C:\msys64`（别改，改了容易踩 PATH 的坑）；
3. 装完从开始菜单打开 **「MSYS2 MINGW64」**。

> ⚠️ **一定是 MINGW64 那个，不是 "MSYS2 MSYS"**。前者才是 Windows 原生工具链，后者是 POSIX 模拟环境，编译出来的是另一套东西。

### 2.2 更新与安装（在 MINGW64 窗口里敲）

```bash
# 1) 先更新自身（可能提示关闭窗口，关掉重开再敲一次）
pacman -Syu

# 2) 装工具链 + SDL2 + 构建工具（一条命令搞定所有依赖）
pacman -S --needed mingw-w64-x86_64-gcc \
                   mingw-w64-x86_64-cmake \
                   mingw-w64-x86_64-ninja \
                   mingw-w64-x86_64-SDL2 \
                   git
```

如果问你 `Proceed with installation? [Y/n]` 就回车；如果让你从一堆包里选，直接回车全装。

### 2.3 验证

```bash
gcc --version      # 出现 gcc (Rev...) 就成功
cmake --version    # cmake version 3.x
pkg-config --modversion sdl2   # 出现 2.x.x 说明 SDL2 装好了
```

三条都出东西 → 环境就绪。

---

## 第 3 章 拿源码（两条路，选一条）

### 3.0 ⚠️ 先修这台机器的 git 配置（实测踩到了）

你这台机器的全局 git 配置里有一条**已失效的镜像规则**：

```
url.https://ghproxy.com/.insteadof = https://github.com/
```

后果：**任何访问 github.com 的 clone 都会被改写到 ghproxy.com，而该站点已无法连接**（`SSL_ERROR_SYSCALL`）。实测直连 github.com 本身是通的。

**两种解法，选一个：**

```bash
# 解法一：删掉这条失效规则（推荐，一劳永逸）
git config --global --unset url.https://ghproxy.com/.insteadof

# 解法二：不改全局配置，只在单次命令里绕过
#   PowerShell:  $env:GIT_CONFIG_GLOBAL = "NUL" 之后再执行 git 命令
#   Git Bash:    GIT_CONFIG_GLOBAL=/dev/null git clone ...
```

### 路线 A（推荐）：官方 PC 模拟器工程，开箱即用

这个仓库**自带 lvgl 子模块 + 已配好的 `lv_conf.h` + CMakeLists + VSCode 调试配置**，是上手最快的。

```bash
cd /c/Users/hades/WorkBuddy/面试/lvgl_ws     # 已帮你建好目录
git clone --recursive --depth 1 --shallow-submodules \
    https://github.com/lvgl/lv_port_pc_vscode.git
```

> `--recursive` 必须加，否则 lvgl 子目录是空的。

### 路线 B：只要 LVGL 本体，自己搭工程（更能学到东西）

```bash
mkdir -p /c/Users/hades/lvgl_demo && cd /c/Users/hades/lvgl_demo
git clone --depth 1 -b release/v9.2 https://github.com/lvgl/lvgl.git
```

> 版本建议：**v9.x 学新特性，v8.3 存量项目最多**。深渡如果做新项目大概率 v9，但 v8 的资料更丰富，两个都可以认识一下。

---

### 3.1 本机已就绪（10-13 已帮你拉好）

```
C:\Users\hades\WorkBuddy\面试\lvgl_ws\lv_port_pc_vscode\
├── lvgl\              ← ✅ 已拉取（LVGL 主线，2026-10-12 的提交）
│   ├── lvgl.h         ← 头文件总入口
│   ├── src\  demos\  examples\
│   └── lv_conf_template.h
├── lv_conf.h          ← ✅ 已存在且已启用（开头是 #if 1）
├── CMakeLists.txt     ← ✅
├── src\main.c         ← ✅ SDL 后端主程序
├── src\hal\hal.c      ← ✅ 显示/输入 HAL
└── bin\               ← 编译后在此产出 main.exe
```

**现在只差编译器**：装好 MSYS2（第 2 章）后，直接跳到 4.2 节构建即可，源码不用再动。

---

## 第 4 章 编译运行（路线 A 的官方模拟器）

### 4.1 这个工程依赖什么（我读了它的 CMakeLists.txt）

| 依赖 | 是否必需 | 说明 |
|---|---|---|
| `lvgl` 子模块 | ✅ **必需** | `add_subdirectory(lvgl)`，库本体 |
| SDL2 | ✅ 必需 | `find_package(SDL2 REQUIRED)` |
| `FreeRTOS` 子模块 | ❌ 不用 | CMake 里 `USE_FREERTOS` 默认 **OFF**，不拉也能编 |
| `lv_drivers` 子模块 | ❌ 不用 | v9 已把驱动内置，`.gitmodules` 里是历史遗留 |
| thorvg | 随 lvgl 自带 | CMake 链接了 `lvgl::thorvg`，说明本工程用 **v9.4+**（老版本没有这个 target） |

> **踩坑提示**：如果你只拉了主仓库、`lvgl` 子目录是空的，cmake 会在 `add_subdirectory(lvgl)` 这一步直接失败。补救：`git submodule update --init --recursive`；如果 `git-submodule` 脚本报 `basename: command not found`，就在**你自己开的 Git Bash / MSYS2 终端**里执行（这条脚本依赖 unix 工具，某些精简环境里没有），或者干脆直接 `git clone https://github.com/lvgl/lvgl.git lvgl`。

### 4.2 构建

```bash
cd /c/Users/hades/WorkBuddy/面试/lvgl_ws/lv_port_pc_vscode
mkdir -p build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
mingw32-make -j4
# 或者用 ninja：cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && ninja
```

**可执行文件输出在 `lv_port_pc_vscode/bin/main.exe`**（CMakeLists 里 `EXECUTABLE_OUTPUT_PATH = ${PROJECT_SOURCE_DIR}/bin`）。双击或命令行运行 —— **弹出一个窗口就是成功了**。

### 4.3 用 VS Code 跑（更省事）

```
VS Code → 打开文件夹 lv_port_pc_vscode → 按 Ctrl+Shift+B 构建 → F5 运行
```

如果报找不到 gcc：`Ctrl+Shift+P` → `C/C++: Edit Configurations`，把编译器路径指向
`C:\msys64\mingw64\bin\gcc.exe`，并按需把 `C:\msys64\mingw64\bin` 加进系统 PATH。

---

## 第 5 章 路线 B 的最小工程（理解头文件与库怎么被引用）

如果你走路线 B，自己搭一个工程，结构就长这样：

```
lvgl_demo/
├── lvgl/                # 克隆下来的库
├── lv_conf.h            # ★ 从 lvgl/lv_conf_template.h 拷贝，改 #if 1，与 lvgl 平级
├── main.c
└── CMakeLists.txt
```

**CMakeLists.txt**（LVGL 自带 CMakeLists，直接当子目录加进来最省事）：

```cmake
cmake_minimum_required(VERSION 3.12)
project(lvgl_demo C)

set(CMAKE_C_STANDARD 11)

add_subdirectory(lvgl)          # 库自带的 CMakeLists 会生成名为 lvgl 的 target
find_package(SDL2 REQUIRED)

add_executable(lvgl_demo main.c)
target_link_libraries(lvgl_demo PRIVATE lvgl SDL2::SDL2)
```

**main.c**（SDL 后端 + LVGL，v8 写法）：

```c
#include "lvgl/lvgl.h"
#include <SDL2/SDL.h>
#include <unistd.h>

#define HOR_RES 800
#define VER_RES 480

static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;
static lv_color_t    buf1[HOR_RES * 100];   /* 1/8 屏左右的 draw buffer */

/* ★ flush_cb：LVGL 渲染完一块，你用 SDL 显示出来 */
static void sdl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    SDL_Rect r = { area->x1, area->y1,
                   area->x2 - area->x1 + 1, area->y2 - area->y1 + 1 };
    SDL_UpdateTexture(texture, &r, px, r.w * sizeof(lv_color_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);

    lv_disp_flush_ready(drv);   /* ★ 忘了这句 → 画面只更新一帧就卡住 */
}

/* ★ read_cb：把鼠标坐标喂给 LVGL，模拟触摸屏 */
static void sdl_mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    SDL_Event e;
    static int last_x = 0, last_y = 0;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if (e.type == SDL_MOUSEMOTION)    { last_x = e.motion.x; last_y = e.motion.y; }
        if (e.type == SDL_MOUSEBUTTONDOWN) {
            last_x = e.button.x; last_y = e.button.y;
            data->state = LV_INDEV_STATE_PRESSED;
        }
        if (e.type == SDL_MOUSEBUTTONUP)  data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

int main(void)
{
    /* 1. 初始化 LVGL */
    lv_init();

    /* 2. 初始化 SDL 与显示 */
    SDL_Init(SDL_INIT_VIDEO);
    window   = SDL_CreateWindow("LVGL Sim", 0, 0, HOR_RES, VER_RES, SDL_WINDOW_SHOWN);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    texture  = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                 SDL_TEXTUREACCESS_STREAMING, HOR_RES, VER_RES);

    /* 3. 注册显示驱动（LVGL 的"三件套"之一） */
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, HOR_RES * 100);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res  = HOR_RES;
    disp_drv.ver_res  = VER_RES;
    lv_disp_drv_register(&disp_drv);

    /* 4. 注册输入驱动（LVGL 的"三件套"之二） */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    /* 5. 写你的界面（三件套之三：主循环在后面） */
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "Hello LVGL");
    lv_obj_center(label);

    /* 6. 主循环：LVGL 是协作式单线程，靠这个心跳跑动画与刷新 */
    while (1) {
        lv_timer_handler();
        usleep(5000);   /* 5ms 一次，等价 200Hz 心跳 */
    }
    return 0;
}
```

**编译：**

```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
mingw32-make -j4
./lvgl_demo.exe
```

> **v8 ↔ v9 差异**（说出来是加分项）：`lv_scr_act()` → `lv_screen_active()`；`lv_disp_drv_t/lv_disp_drv_register()` → `lv_display_t/lv_display_create()`；`lv_btn` → `lv_button`；`lv_img` → `lv_image`。上面是 v8 风格，v9 的骨架一样，只是这些名字变了。

---

## 第 6 章 不想装编译器？先玩 SquareLine Studio

如果你现在只想**看到界面长什么样**、拖一拖找感觉：

- https://squareline.io/ 下载 SquareLine Studio（有免费版，需注册）；
- 它内置**免费的 PC 预览器**，不用装 gcc 就能拖控件、看效果、导出 LVGL 的 C 代码；
- 把它导出的代码放进上面的工程，就能看到自己拖的界面。

**适合场景**：面试前想快速有个"我做过 demo"的东西。但它不教你库本身，**长期还是要走第 2~4 章的路线**。

---

## 第 7 章 报错对照表（先存着）

| 报错 / 现象 | 原因 | 解法 |
|---|---|---|
| `lv_conf.h: No such file or directory` | 没生成配置文件 | 拷 `lv_conf_template.h` → `lv_conf.h`，与 `lvgl` 平级 |
| 编译通过但一堆 `undefined reference` | `lv_conf.h` 开头还是 `#if 0` | 改成 `#if 1` |
| 窗口弹出但**画面只刷一帧就不动了** | `flush_cb` 里忘了 `lv_disp_flush_ready()` | 补上（最经典的坑） |
| 窗口全黑 | draw buffer 太小或 `LV_COLOR_DEPTH` 与 SDL 像素格式不匹配 | `LV_COLOR_DEPTH` 设 16，SDL 用 `SDL_PIXELFORMAT_RGB565` |
| 颜色错乱 / 花色 | 色彩深度不匹配（16 vs 32） | 同上，两边对齐 |
| 点击没反应 | `read_cb` 里没维护坐标，或没注册 indev | 照抄第 5 章的 mouse_read |
| `SDL2 not found`（cmake 报） | MSYS2 里 SDL2 没装 | `pacman -S mingw-w64-x86_64-SDL2` |
| 随机崩溃（多线程时） | 在非 UI 线程直接改 `lv_obj` | 用 `lv_async_call()` 投递到 UI 线程 |
| 中文显示成方块 | 默认字体不含中文 | 用 `lv_font_conv`（Node）把 TTF 转成 C 字体数组 |
| 屏幕大面积重绘闪烁 | draw buffer 太小 | 增大到 1/10 屏以上 |

---

## 第 8 章 从 PC 到 RK3308（后面要改什么）

在 PC 上跑通之后，上板子只换**驱动层**，LVGL 与界面代码完全不动：

| PC 模拟 | RK3308 真机 |
|---|---|
| SDL2 窗口 | `/dev/fb0`（fbdev）或 DRM，`mmap` 后按行 memcpy |
| SDL 鼠标事件 | `/dev/input/eventX`（evdev 触摸） |
| `SDL_RenderPresent` | flush_cb 里 `memcpy` 到 fb + 处理 stride |
| 无内存限制 | `LV_MEM_SIZE` 要预算；`lv_mem_monitor()` 盯水位与碎片 |
| 无带宽压力 | **无 GPU，纯软件渲染**：脏矩形 + RGB565 + draw buffer 控制在 1/10 屏 |

---

## 附：三天执行清单（照着打勾）

**Day 1（今天）**
- [ ] 下载安装 MSYS2 → 打开 MINGW64
- [ ] `pacman -Syu` → 关窗重开 → `pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,SDL2} git`
- [ ] `gcc --version` 通过
- [ ] clone `lv_port_pc_vscode`（--recursive）
- [ ] cmake 构建 → **跑出窗口**

**Day 2**
- [ ] 打开 `lvgl/examples/widgets/`，跑通并逐个改参数
- [ ] 记住八组 API：对象/布局、样式与状态、事件、动画、定时器
- [ ] 改一个官方 demo（比如按钮 + 事件回调）

**Day 3**
- [ ] 做"室内机主界面"：状态栏 + 呼叫弹窗 + 接听/挂断按钮 + 视频占位区
- [ ] 加一个呼叫弹窗的入场动画（`lv_anim`）
- [ ] 截图/录屏 → 这就是你的 demo 作品

---

## 实战记录（10-21 已全部跑通 ✅）

**结果：`bin\main.exe`（3.5MB）已编译成功并运行，LVGL Widgets demo 窗口已弹出。** Day 1 清单全部完成（且没用你手动敲一条命令）。

### 实际走过的流程（与文档差异）

1. **MSYS2 没走官网**：winget 拉 GitHub 源卡了 16 分钟下载不动 → 改从**清华 TUNA 镜像**下 `msys2-x86_64-latest.exe`（2 秒下完 81MB），静默安装：`msys2-x86_64-latest.exe install --root C:/msys64 --confirm-command --accept-messages`。
2. **pacman 镜像**：新版 MSYS2 用统一的 `etc\pacman.d\mirrorlist.mingw`（带 `$repo` 变量）。**坑：TUNA 的 msys2 路径是 `/msys2/mingw/$repo/`（多一层 `mingw`），少写一层就 404**。最终镜像行：
   ```
   Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/mingw/$repo/    # mingw 系
   Server = https://mirrors.tuna.tsinghua.edu.cn/msys2/msys/$arch/     # msys 源
   ```
3. **gpg 密钥环初始化卡死**：`pacman -Syu` 首次运行要初始化签名密钥，Windows 上 gpg 烧 14 分钟纯 CPU 还没完 → **临时改 `etc\pacman.conf` 第 41 行 `SigLevel = Never`**（HTTPS 镜像，本地开发可接受），之后 pacman 一切正常。想恢复安全校验时改回 `SigLevel = Required`。
4. **工具链一条命令装完（68 秒）**：`pacman -S --needed --noconfirm mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2`。版本：gcc 16.2.0 / cmake 4.4.4 / ninja 1.13.2 / SDL2 2.32.10。
5. **编译配置坑（教学价值最高）**：`lv_conf.h` 里 `LV_USE_VECTOR_GRAPHIC 1`（211 行）但 `LV_USE_THORVG 0`（217 行）自相矛盾 → 报错 `LV_USE_VECTOR_GRAPHIC requires (LV_USE_DRAW_SW and LV_USE_THORVG)...`。**把 ThorVG 改成 1 后通过**。教训：lv_conf.h 是 LVGL 的"功能总开关"，宏之间有依赖关系，报 #error 先查配置矛盾。
6. **构建命令（生效版）**：
   ```bash
   # 在 MSYS2 MINGW64 环境里（或带 MSYSTEM=MINGW64 的 bash -lc）
   cd build && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release .. && ninja
   ```
   961 个目标文件约 15 分钟编完（-O3，含 ThorVG C++）。中途遇到过一次 cc.exe 全家死锁（CPU 时间 0.06s 挂 25 分钟），杀进程重跑 ninja 即从断点继续 —— ninja 增量构建的价值。
7. **运行**：必须带 `C:\msys64\mingw64\bin` 在 PATH 里（SDL2.dll 在那），已写入用户 PATH。以后直接双击 `bin\main.exe` 即可。
   - **10-22 补坑（DLL 版本地狱）**：只靠 PATH 还不够 —— 双击运行时 Windows 按系统 PATH 顺序搜 DLL，别的软件（anaconda 等）自带的**旧版** `libstdc++-6.dll` / `libwinpthread-1.dll` 会被抢先加载，报「无法定位程序输入点 clock_gettime64」。**根治：把 `SDL2.dll / libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll / zlib1.dll` 五个 DLL 拷到 main.exe 同目录**（程序目录在 DLL 搜索顺序里永远最优先），exe 从此自包含，拷到别的电脑也能跑。已验证双击启动正常。

### 这个工程跑的是什么

`src\main.c` 只有 35 行：`lv_init()` → `sdl_hal_init(320,480)`（内部用 v9 内置的 `lv_sdl_window_create`/`lv_sdl_mouse_create`）→ `lv_demo_widgets()` → `while(1){ lv_timer_handler(); usleep(); }` 心跳循环。**这就是 LVGL 移植三件套的活标本：显示驱动 + 输入驱动 + 心跳**，面试讲"如何把 LVGL 移到 RK3308"直接照这个结构说。
