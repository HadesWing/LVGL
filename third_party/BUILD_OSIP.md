# libosip2 5.3.2 源码构建指南（MinGW / Windows）

> 归档：`third_party/libosip2-5.3.2.tar.gz`（官网 GNU 镜像原包）
> 产物目录约定：`third_party/osip-install/`（不进 git，CMake 会自动回退到该目录查找）

## 1. 为什么要自己编

- Debian/Ubuntu 有现成包，但 Windows/MSYS2 没有官方 mingw 包；
- 本项目只需要 **静态库**（`libosip2.a` / `libosipparser2.a`），体积小、免部署。

## 2. 构建步骤（已验证可复现）

```bash
# ① 必须在 ASCII 路径下解压构建！中文路径会导致链接阶段文件名乱码失败
mkdir -p /c/osip_build/src && cd /c/osip_build/src
tar xzf <repo>/third_party/libosip2-5.3.2.tar.gz
cd libosip2-5.3.2

# ② configure（两个关键 CFLAGS 见下方"坑"说明）
./configure --prefix=/c/osip_build/install \
    --disable-shared --enable-static \
    CFLAGS="-DHAVE_PTHREAD_WIN32 -O2"

# ③ autotools 文件时间戳修复（包内时间戳乱序会让 automake 找 aclocal-1.16）
touch aclocal.m4 configure config.h.in Makefile.in src/Makefile.in src/osip2/Makefile.in src/osipparser2/Makefile.in
make -j4 && make install
```

## 3. 四个必踩的坑

| # | 现象 | 根因与解法 |
|---|------|-----------|
| 1 | 链接报错 `cannot open ... ????` | **中文路径**。解压/构建一律放 ASCII 路径（如 `C:/osip_build`） |
| 2 | `conflicting types for osip_thread_t` | MSYS2 环境变量残留。加 `CFLAGS="-DHAVE_PTHREAD_WIN32"` 强制走 win32 线程封装 |
| 3 | `redefinition of struct timespec` | `osip_condv.c` 里 `#if (_MSC_VER < 1900)` 漏了 `_MSC_VER` 未定义时的判断。改为 `#if defined(_MSC_VER) && (_MSC_VER < 1900)` |
| 4 | `error: field 'timer_a_start' has incomplete type` | **业务代码**的 include 顺序：`<winsock2.h>` 必须在 `<osip2/osip.h>` 之前（后者用到 `struct timeval`） |

## 4. 运行期大坑：解析前必须 osip_init()

**症状**：只组包（`osip_message_to_str`）一切正常；一旦 `osip_message_parse()` 解析任何报文，
在 `__osip_message_is_known_header()` 里对 `strcmp(NULL, "via")` 访问违例（0xC0000005 / exit 139）。

**根因**：`osip_parser_cfg.c` 的全局哈希表 `hdr_ref_table[]` 是静态数组，初值全 0（而不是 -1）。
必须由 `parser_init()` 初始化为 -1 并填充表项，而 `parser_init()` 只被 `osip_init()` 调用。
头域查找逻辑 `hdr_ref_table[hash]` 拿到 0 后去 `strcmp(pconfig[0].hname, ...)`，而 `pconfig[0].hname` 为 NULL → 崩溃。

**解法**：程序启动后、第一次解析前调用一次：

```c
osip_t *osip = NULL;
osip_init(&osip);          // 内部: parser_init()
...
osip_release(osip);        // 退出时
```

只发不收（如只测 REGISTER 报文格式）不会暴露此坑 —— 这是它隐蔽的原因。

## 5. 其余 API 使用要点（本项目踩过）

- `osip_message_set_method/set_version` **接管指针所有权**：传字符串字面量 → `osip_message_free` 时
  `free(字面量)` → 堆损坏（表现为 free 之后下一条语句崩溃）。必须 `osip_strdup("REGISTER")`；
- `osip_message_parse` 的 `length` 用**原始字节数**（含 `\0` 之前全部内容），不要 `strlen+1`；
- 解析结果判向：`msg->status_code > 0` 是响应，`msg->sip_method != NULL` 是请求。
