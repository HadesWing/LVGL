# OEM 定制指南 —— 一套室内机软件卖给 N 个客户

> 目标：同一份 UI 代码，靠"数据"就能变成不同客户的界面。
> 客户 A 是白底蓝调、宫格首页；客户 B 是石墨底暖金、列表首页、没有场景条 ——
> 两者不需要两条代码分支，**也不需要编译两份不同的代码**（同一份二进制可切换）。

---

## 一、先明确三种"不一样"

接 OEM 时客户的需求通常落在这三层，一定要分开处理，否则会写成意大利面：

| 层次 | 例子 | 改动成本 | 本工程怎么做 |
|---|---|---|---|
| **换皮** | 配色、圆角、字号、间距 | 分钟级 | profile 数据 / `oem.ini` |
| **换结构** | 首页宫格 ↔ 列表，去掉场景条 | 小时级 | `layout.home_style` 选择构建器 |
| **换功能** | 要不要语音、要不要视频对讲 | 天级 | `caps` 位掩码 + 对应模块开关 |

**铁律：UI 代码里不允许出现品牌字面量。**
文案走 `oem_text()`，颜色走样式表（样式表读 `oem_color()`），尺寸走 `oem()->layout`。

---

## 二、接口（`src/oem/oem.h`）

```c
typedef struct {
    const char *id;       /* "default" / "alt" */
    const char *vendor;   /* 公司名 */
    const char *model;    /* 型号 */
    const char *version;
    uint32_t    colors[OEM_COLOR_COUNT];   /* 11 个色槽 */
    const char *text[OEM_TXT_COUNT];       /* 20 条文案槽 */
    oem_layout_t layout;                   /* 尺寸 + 结构 + 显示开关 */
    uint32_t    caps;                      /* OEM_CAP_SIP/VOICE/VIDEO */
} oem_profile_t;

void         oem_init(void);                 /* 启动时一次 */
const oem_profile_t *oem(void);
uint32_t     oem_color(oem_color_id_t id);
const char  *oem_text(oem_text_id_t id);
bool         oem_has(uint32_t cap);
void         oem_dump(void);                 /* 打印当前生效配置 */
```

色槽：`PRIMARY / BG / SURFACE / CARD / BAR / BAR_TEXT / TEXT / TEXT_MUTED / BORDER / OK / DANGER`

文案槽：`PRODUCT / BRAND / SIP_*`（离线/在线/呼出/来电/通话）、`SCENE_0..3`、`ROOM_0/1/ALL`、`CAT_*`（灯光…摄像头）

布局参数：

```c
typedef struct {
    oem_home_style_t home_style;   /* GRID | LIST */
    int16_t bar_h, title_h, scene_h;
    int16_t card_w, card_h, gap, radius, pad;
    bool show_scene_bar, show_clock, show_brand, show_voice_bar;
} oem_layout_t;
```

---

## 三、三种生效路径（从粗到细）

### ① 环境变量选 profile —— 现场最快

```bash
OEM_PROFILE=alt ./bin/main.exe        # 列表式 + 石墨金
OEM_DUMP=1 ./bin/main.exe             # 打印当前生效配置（验货用）
```

### ② `oem.ini` 选 profile + 微调 —— 不用重新编译

把 `config/oem.ini` 拷到可执行文件目录，去掉注释即可。示例：

```ini
profile=alt
color.primary=0xE53935
color.bg=0x101418
text.product=我的智慧家
layout.card_h=72
layout.show_scene_bar=0
```

查找顺序：`OEM_INI` 指向的文件 → `./oem.ini` → `../config/oem.ini` → `config/oem.ini`

> 客户说"蓝色再深一点"，改一行 ini 重启就能看 —— 这是打样阶段最省事的做法。
> 定型后把值固化进 profile `.c`，现场只留必要差异。

### ③ 新增 profile `.c` —— 正式交付

复制 `src/oem/oem_alt.c` → `oem_xxx.c`，改数据，然后：
1. 在 `oem.c` 的 `s_profiles[]` 里加一行 `&oem_profile_xxx;`
2. CMake 的 `MAIN_SOURCES` 加这个文件
3. 想让它成为出厂默认：编译时加 `-DOEM_DEFAULT_PROFILE=\"xxx\"`

**全程不改任何 `ui_*.c`。**

---

## 四、改布局到底改哪里？

按"影响范围"从大到小，四个入口：

| 想改的东西 | 改哪里 | 例子 |
|---|---|---|
| 卡片大小/间距/状态栏高度 | `layout.*`（ini 或 profile） | `layout.card_w=150` |
| 首页结构（宫格 → 列表） | `layout.home_style` | `layout.home_style=list` |
| 要不要场景条/时钟/语音条 | `layout.show_*` | `layout.show_scene_bar=0` |
| 控件本身的视觉（阴影/边框/按下态） | `ui_theme.c` 的样式表 | 改 `st_card` 的 `shadow_width` |
| 全新的结构（如"房间为中心"布局） | 新写 `build_xxx()` + 加一个枚举 | 见 `ui_home.c` 的 `build_grid/build_list` |

**为什么结构不走 ini**：宫格和列表是两份不同的构建代码，不是几个参数能表达的。
所以结构用枚举选构建器，参数用 ini 调 —— 各归各位，谁也不污染谁。

### 新增一种首页结构的步骤

```c
/* 1. oem.h 里加枚举 */
typedef enum { OEM_HOME_GRID = 0, OEM_HOME_LIST, OEM_HOME_ROOM } oem_home_style_t;

/* 2. oem.c 的 ini 解析加分支 */
if (!strcmp(v, "room")) s_cur.layout.home_style = OEM_HOME_ROOM;

/* 3. ui_home.c 写构建器并挂上 */
if (L->home_style == OEM_HOME_ROOM) build_by_room(scr, L);
```

---

## 五、新增一个客户的完整清单（约 30 分钟）

1. **拿客户 VI**：主色、深色/浅色底、logo 文案、产品名。
2. `cp src/oem/oem_alt.c src/oem/oem_acme.c`，改 `id/vendor/model/colors/text/layout`。
3. `oem.c`：`extern` + `s_profiles[]` 加一行。
4. `CMakeLists.txt`：`MAIN_SOURCES` 加文件。
5. **重跑字库**：`python scripts/gen_font.py`（新增文案里的字必须进字库，否则显示方框）。
6. 验证：
   ```bash
   OEM_PROFILE=acme OEM_DUMP=1 ./bin/main.exe          # 看配置
   OEM_PROFILE=acme LVGL_UI_TEST=1 ./bin/main.exe      # 自动跑一遍页面栈
   ```
7. 客户点头后：把最终值固化进 `oem_acme.c`，出厂默认 `-DOEM_DEFAULT_PROFILE=\"acme\"`。

---

## 六、字库必须跟着文案走（最容易翻车的地方）

中文字库是**按需子集**（`scripts/gen_font.py` 扫描源码字符串生成），
所以只要新增了中文文案，就必须重跑：

```bash
python scripts/gen_font.py          # 重新生成 src/ui/fonts/ui_font_cjk_{16,24}.c
python scripts/gen_font.py --check  # 只看收了多少字，不生成
```

> **`gen_font.py` 是 PC 上的构建期工具，目标机不跑 Python。**
> 它生成的两个 `.c` 文件是提交进仓库的普通源码，板子上交叉编译时直接编译它们 ——
> 也就是说"新增文案 → PC 上重跑一次 → 提交生成物"是**唯一**需要 Python 的环节，
> 而这一步在出厂前的开发期就做完了，量产编译链路零 Python 依赖。

判断标准：**界面出现"□"就是字库缺字**，不是字体没生效。
体积账：4bpp 下 16px 约 128 B/字、24px 约 288 B/字；当前 212 个汉字 ≈ 100 KB 常量。

> 客户如果要求"任意输入都显示"（比如设备名来自用户输入），才需要全字库（GB2312 6763 字），
> 那时体积会到 1 MB 级，要按 2bpp + 压缩重新算账。

---

## 七、验收清单（交付前逐条过）

- [ ] `OEM_DUMP=1` 打印的 profile/model 与合同一致
- [ ] 界面**没有方框**（缺字）、没有被截断的文案
- [ ] 品牌色出现在：图标、Tab 选中态、强调文字（不是"到处都是"）
- [ ] 深浅底色的可读性：文字与背景对比度够（浅色底别用浅灰字）
- [ ] 关掉场景条后，底部不留空白（flex `grow` 生效）
- [ ] `LVGL_UI_TEST=1` 自动跑 3 层页面压栈/出栈，无断言、内存回落到基线
- [ ] `make mem64` 下峰值 < 60%（不同 OEM 布局的控件数量不同，要复测）

---

## 八、已实测结果

两套 profile 在同一份二进制里切换，各跑 6 秒：

| | default（宫格） | alt（列表） |
|---|---|---|
| 首页结构 | 3×2 宫格 | 单列 6 行 |
| 主色 | `0x2F80ED` 蓝 | `0xD4A03B` 金 |
| 底色 | `0xECEFF1` 浅灰 | `0x15171A` 石墨 |
| 场景条 | 有 | 无 |
| 状态栏 | 34px | 40px，含品牌名 |
| 渲染警告 | 0 | 0 |
| 崩溃/断言 | 0 | 0 |

复现命令：

```bash
OEM_PROFILE=alt OEM_DUMP=1 ./bin/main.exe
```

---

## 九、常见坑

1. **ini 里的中文乱码**：文件必须存成 UTF-8（无 BOM）。GBK 会让比较与显示都错。
2. **改了 ini 没生效**：`oem.ini` 的查找是"当前工作目录优先"，从 IDE 启动时 cwd 可能是别处 ——
   用 `OEM_INI=<绝对路径>` 或看启动日志里的 `[oem] ini: ...` 确认读了哪个文件。
3. **颜色不生效**：主题只在启动时套一次；`ui_theme_init()` 必须在 `oem_init()` 之后。
4. **env 变量残留**：`OEM_PROFILE=alt` 只在当前命令行有效；Windows 上如果用 `setx` 会持久化，
   排查"为什么客户机器上是金色界面"先看环境变量。
5. **caps 关掉 SIP 但 UI 还在**：`caps` 是"能力声明"，界面里要主动判断
   `oem_has(OEM_CAP_SIP)` 再决定要不要显示入口（后续加语音模块时一起补）。
