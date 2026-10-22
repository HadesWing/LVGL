# SIP 对讲联调环境

> 客户端：`src/app/app_sip.c`（oSIP 编解码 + 自研 UDP 收发/状态机）
> 对端：板端用 `tools/sip_echo`（纯 C 应答器）；PC 上还可以用 miniSIPServer / MicroSIP

## 零、工具与目标机的边界（先看这个）

**目标机上没有 Python，也不一定有 PC。** 所以先把"哪些是 PC 工具、哪些是要上板子的代码"分清楚，
否则会在交付前才发现有一半脚本跑不起来。

| 文件 / 工具 | 跑在哪 | 目标机需要 | 说明 |
|---|---|---|---|
| `src/**/*.c` | 板子 | **是** | 产品代码，交叉编译成 `main` |
| `tools/sip_echo/` | **板子 / PC 都可以** | 可选 | 纯 C 应答器，只依赖 libosip2，无 LVGL/SDL/Python |
| `scripts/gen_font.py` | **仅 PC** | 否 | 构建期工具；产物 `src/ui/fonts/*.c` **已入库**，板子不需要它 |
| `scripts/fake_sip_*.py` | **仅 PC** | 否 | 早期联调脚本，已被 `tools/sip_echo` 取代，仅留作参考 |
| `scripts/dev.sh` / `setup_windows_msys2.sh` | **仅 PC** | 否 | Windows 构建环境封装；板子上直接 `cmake -B build && ninja` |
| `miniSIPServer` / `MicroSIP` | PC | 否 | 开发期真实服务器与标准软电话；量产环境是门口机 + 平台 |

两条硬约束：

1. **构建不依赖 Python**：`CMakeLists.txt` 里没有任何 `execute_process(python ...)`，
   字库/资源的生成物都是提交进仓库的普通 `.c` 文件。
2. **运行不依赖 Python**：对端由 `tools/sip_echo` 这个 C 程序扮演，它可以跟着产品一起
   交叉编译到 RK3308，于是"板子自己测自己"。
   只有在**新增中文文案**时才需要 PC 上重跑一次 `gen_font.py`，并把它生成的 `.c` 一起提交。

## 一、板端自测：内置回声应答器（推荐）

`tools/sip_echo` 是一个 300 行的纯 C 应答器：自动接听 INVITE、原样回送 RTP。
它和被测程序的依赖完全一样（libosip2 + BSD socket），因此**能跟着交叉编译到板子上**。

```bash
# 终端 A：应答器（SIP 5090 / RTP 5092，故意避开 5060 和 miniSIPServer 共存）
make echo
# 交叉编译版：
#   cmake -S tools/sip_echo -B build-echo-arm -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
#         -DOSIP_ROOT=<aarch64 版 libosip2 前缀>
#   cmake --build build-echo-arm

# 终端 B：被测的室内机程序，注册到应答器并自动呼出、6 秒后挂断
SIP_SERVER=127.0.0.1 SIP_PORT=5090 SIP_USER=echo SIP_PASS=echo SIP_LOCAL_PORT=5062 \
SIP_AUTOCALL=echo SIP_AUTOHANGUP=6 SIP_TONE=1 make run
```

`SIP_TONE=1` 很关键：**没有麦克风的板子/CD，用它合成 1kHz 正弦当音源**，
不需要声卡采集也能把"编码 → RTP → 解码 → 播放"整条链路跑满。

实测日志（本机 2026-10-22，`rx=298 tx=299 over 6s = 49.8 pps`）：

```
[sip] >> REGISTER sip:echo@127.0.0.1 SIP/2.0
[sip] << 200 OK            [sip] registered (expires=3600)
[sip] >> INVITE sip:echo@127.0.0.1:5090 SIP/2.0
[sip] << 100 Trying        [sip] << 180 Ringing        [sip] << 200 OK
[sip] >> ACK -> 127.0.0.1:5090
[rtp] media start: 127.0.0.1:5092 pt=8 mode=audio
[sip] >> BYE
[rtp] stats rx=298 tx=299 dur=6s (pps=49.8)
[audio] closed (underrun=1, cap_lost=0)
```

应答器一侧：

```
[echo] << INVITE  (127.0.0.1)
[echo] 来电 → 自动接听；本机媒体地址 127.0.0.1:5092
[echo] >> 100 Trying  >> 180 Ringing  >> 200 OK
[echo] << ACK  [echo] rtp rx=354 tx=354 dur=15s pps=23.4
[echo] << BYE  [echo] >> 200 OK
[echo] 通话结束 rx=669 tx=669 时长=20s
```

> 说明：`pps` 那几行是**从 INVITE 时刻起算的累计均值**，包含建媒体之前的空窗，
> 所以中途看到 23 而不是 50 是正常的 —— 看最后客户端自己报的 `(pps=49.8)` 才准。

### 支持的请求

`REGISTER`（无条件 200，不做鉴权）· `INVITE`（100/180/200+SDP，占线回 486，
重传可识别）· `ACK` · `BYE` · `CANCEL`（200 + 对原 INVITE 回 487）· `OPTIONS` · 其余 405。

### PC 辅助方案（可选，非目标机）

早期用 Python 假服务器验证注册链路，现在仍留着做参考：

```bash
python scripts/fake_sip_responder.py 5060 1001 1234   # REGISTER→401→auth→200
python scripts/fake_sip_caller.py 5062 1001           # 脚本化 UAC
```

> 这两个脚本**只用于 PC**，不要指望它们出现在板子上；能力也已被 `tools/sip_echo` 覆盖。

## 二、真实方案：miniSIPServer（免费版 5 个分机）

**官方站点：`myvoipapp.com`（不是 myvoipuser / minisipserver.com 只是别名）**

| 项 | 值 |
|----|-----|
| 下载页 | https://www.myvoipapp.com/download/index.html |
| 免费版直链（5 clients） | https://www.myvoipapp.com/download/mss_v70/mss_v70_u5.exe |
| 版本 | V70 build 20261020，约 8 MB，需交互安装（一路 Next） |
| 授权 | `u5` = 5 个分机免费无限期；`u20/u50/u100/u300/u500` 为 30 天全功能试用 |

> 本机已下载好安装包：`C:\sip_tools\mss_v70_u5.exe`（免去找链接），双击安装即可。

安装与配置：

1. 安装后启动 `mss.exe`（或桌面快捷方式），默认监听 **UDP 5060**，主界面会显示本机 SIP 地址；
2. **默认已预置三个分机**：`100 / 101 / 102`，密码与分机号相同 —— 想省事直接用；
   要加自己的分机：菜单 `数据(D) → 分机`，填「分机号 / 密码 / 描述」，默认拨号计划 `default`；
3. 客户端起两个实例（本机端口必须错开，否则 bind 失败）：

```bash
# 室内机（本工程），1001
SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=1001 SIP_PASS=1234 SIP_LOCAL_PORT=5062 ./bin/main.exe

# 对端：MicroSIP 注册 1002（或再起一个 main.exe，SIP_LOCAL_PORT=5064）
```

4. miniSIPServer 默认**按用户名+密码做 401 认证**（也可勾 "IP address authorization" 改成 IP 放行）；
   本客户端已实现 Digest（qop=auth 与无 qop 两种），注册失败先看服务端日志窗口；
5. 呼叫：分机互拨直接拨对方号码（如 `1001`），出群呼叫要在号码前加前缀 `9`。

## 三、已知坑（客户端侧，均已修复/规避）

| # | 坑 | 说明 |
|---|-----|------|
| 1 | **解析崩溃（0xC0000005）** | oSIP 的 `osip_message_parse` 依赖 `parser_init()` 建的头域哈希表，而它只被 `osip_init()` 触发。启动后必须先 `osip_init(&osip)`，退出 `osip_release(osip)`。只"组包不解析"不会暴露此坑 |
| 2 | **字面量传给 set_method** | `osip_message_set_method/set_version` 接管指针所有权，传 `"REGISTER"` 字面量 → free 时堆损坏。必须 `osip_strdup()` |
| 3 | **头文件顺序** | `<winsock2.h>` 必须在 `<osip2/osip.h>` 之前（后者用到 `struct timeval`） |
| 4 | **401 重放死循环** | 带认证重发后再收 401 直接置 ERROR，不再重试（`s_auth_tried` 门闩） |

## 四、客户端选型（对面那台"门口机"用什么）

| 客户端 | 版本/体积 | 适合 | 不适合 |
|--------|----------|------|--------|
| **MicroSIP**（首选） | 3.22.20，便携 zip 8MB（解压 19MB），GPL | 音频对讲调参；**Call Manager 能直接看原始 SIP 报文**（调 oSIP 极有用）；支持**不注册直接拨 SIP URI** | 视频能力弱（有但不好用） |
| **Linphone Desktop** | 6.1+（`winget install --id BelledonneCommunications.Linphone -e`） | **视频对讲**（H.264/VP8），室内机产品形态需要 | 体积大、界面重、日志不直观 |
| Zoiper 5 | 免费版 | 界面友好、手机端也有 | 视频/多账号要收费 |
| 本工程自身 | — | 两个实例互打（1001↔1002），验证自家栈 | 不能当参考实现 |

**结论：现在（音频阶段）用 MicroSIP；等做视频再上 Linphone。**
本机已放便携版：`C:\sip_tools\MicroSIP-3.22.20\MicroSIP.exe`（免安装，双击即用）。

### 3.1 场景 A：不装任何服务器（最快验证 INVITE 收包）

MicroSIP 支持 **direct IP call**，不需要注册、不需要 SIP 服务器 ——
这也是不少真实项目里的部署形态（门口机 ↔ 室内机直连，没有平台）：

1. MicroSIP 某些版本要求 Account 里必须填点东西，随便填 `Username=1002`、`SIP Server` 留空即可；
2. 拨号框直接输入 **`sip:1001@127.0.0.1:5062`**（5062 = 我们客户端的 `SIP_LOCAL_PORT`）；
3. 我们客户端日志会出现 `[sip] << INVITE`，状态转 `SIP_INCOMING`，随后 100/180/200 依次发出。

本端要跑在点对点模式，否则它会先去注册：

```bash
# 不需要 SIP_SERVER，也不需要 registrar
SIP_NO_REGISTER=1 SIP_LOCAL_PORT=5062 ./bin/main.exe
```

反向（我们打给 MicroSIP）也一样：

```bash
SIP_NO_REGISTER=1 SIP_AUTOCALL=sip:1002@127.0.0.1:5060 ./bin/main.exe
```

> **MicroSIP 是终端，不是服务器。** 它在一次事务里既当 UAC（呼出）又当 UAS（呼入），
> 但**不做 Registrar / Proxy**：不会替你登记账号、也不会转发别人的信令。
> 想测"注册 + 路由"还是得挂 miniSIPServer（场景 B）。

### 3.2 场景 B：走 miniSIPServer（完整注册+路由）

见第二节。MicroSIP 侧 Account 填：
- SIP Server: `127.0.0.1:5060`
- Username: `1002`，Password: 与服务器一致
- 勾 **"Register with domain"**，注册成功后状态栏显 Online

### 3.3 编码协商要点（踩过就知道多重要）

- 本工程目标编码是 **PCMA / G.711A（8kHz，20ms 打包，160 字节）** —— 与海思/大华门口机一致；
- MicroSIP 默认会把 Opus/G.722 排在前面，**必须在 Account → Media 里把 PCMA 拖到第一位**，
  否则对端回 200 OK 的 SDP 里带的是 Opus，我们的固定 20ms PCMA 收包会解不出来；
- RTP 端口是**偶数+1 成对**（如 4000/4001），SDP 里 `m=audio 4000 RTP/AVP 8`；
- 对称 RTP（NAT 场景）我们的实现是"往哪来就往哪回"，本机联调不受影响。

### 3.4 抓包与排错

- MicroSIP：`Ctrl+3` 打开 **Call Manager**，每通电话可看完整 SIP 消息文本；
- 我们侧：所有收发都有 `[sip] >>` / `[sip] <<` 日志；
- 需要第三方视角时装 Wireshark，过滤 `sip || rtp`，本机回环流量也能抓到。

### 3.5 同一台 PC 跑两个客户端会怎样（麦克风 / 扬声器）

**不会"抢不到"，但一定会啸叫。** 两件事要分开看：

| 问题 | 结论 | 原因 |
|---|---|---|
| 设备被独占？ | **不会** | Windows WASAPI 默认共享模式，多进程可同时打开同一设备，由系统混音；Linux ALSA 的 `default`（dmix）同理。只有显式请求独占（WASAPI exclusive / `hw:0`）才会失败。SDL2 默认走共享模式 |
| 端口冲突？ | **会** | 两个客户端都想 bind 5060 一定失败，必须错开 `SIP_LOCAL_PORT`（我们用 5062） |
| 声音正常？ | **不会** | 这是**声学回授**：A 的喇叭 → 房间 → B 的麦克风 → 回到 A 的喇叭，正反馈啸叫。本工程**没有 AEC**（回声消除），免提场景必然触发 |

按成本排序的解法：

1. **戴耳机**（物理隔离，最省事）；
2. **单端静音**：一端只发不收（我们这边 `SIP_TONE=1` + 不开播放），单向验证链路；
3. **虚拟声卡**（VB-CABLE / VoiceMeeter）：把 A 的输出接到虚拟设备、B 从虚拟设备取音 —— 纯 PC 手段，板子上没有；
4. **两机对测**（PC ＋ 板子）最接近真实形态；
5. **板端做 AEC**（speexdsp / webrtc-audio-processing）—— 免提对讲的量产必做项，已列入 M6。

> 本机实测：WASAPI 报 `can't find requested audio endpoint: 找不到元素` ——
> **这台机器根本没有采集设备**，所以本机上"双向语音"不可能成立，
> 一律用 `SIP_TONE=1` 合成 1kHz 音源来验媒体链路（见 §7.2）。

## 五、自测开关（无界面也能跑全链路）

客户端内置四个环境变量开关，用于 CI / 无人工点击的回归：

| 变量 | 作用 |
|------|------|
| `SIP_AUTOANSWER=1` | 收到 INVITE 自动 200 接听（跳过 UI 点"接听"） |
| `SIP_AUTOCALL=<号码>` | 注册成功后自动呼出指定号码 |
| `SIP_AUTOHANGUP=<秒>` | 通话/振铃 N 秒后自动挂断（通话发 BYE，振铃发 CANCEL） |
| `SIP_TONE=1` | 用 1kHz 正弦代替麦克风（无声卡也能验媒体链路 / 板端对讲自检） |
| `SIP_RTP_ECHO=1` | 强制回声模式（不打开声卡，收到 RTP 原样回送） |
| `SIP_EXPIRES=<秒>` | 注册有效期（默认 3600；测试续期用 60） |
| `SIP_NO_REGISTER=1` | 点对点模式：不注册直接在线；此时 `SIP_SERVER` 可不填（IP 直呼 / 接 MicroSIP） |
| `SIP_NO_AUDIO=1` | 完全不碰声卡（纯信令回归用） |
| `SIP_DEBUG=1` | 打印 ACK 原文等调试细节 |

### 双实例互拨（走真服务器，最接近产品形态）

```bash
# 终端 1：101 自动接听
SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=101 SIP_PASS=101 \
  SIP_LOCAL_PORT=5064 SIP_AUTOANSWER=1 ./bin/main.exe

# 终端 2：100 注册后自动呼 101，通话 3 秒挂断
SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=100 SIP_PASS=100 \
  SIP_LOCAL_PORT=5062 SIP_AUTOCALL=101 SIP_AUTOHANGUP=3 ./bin/main.exe
```

实测通过（miniSIPServer V70）：
```
A(100): REGISTER→401→auth→200 → INVITE → 100 → 200 → ACK → BYE → 200
B(101): INVITE → 100 Trying → 180 Ringing → 200(SDP) → ACK → BYE → 200
两侧通话页压栈/出栈正常，内存回落基线 28.5KB（无泄漏）
```

### 脚本化 UAC（不需要第二个实例）

不用再写 Python 了，用板端的应答器 + `SIP_AUTOCALL`：

```bash
make echo                                   # 终端 A
SIP_SERVER=127.0.0.1 SIP_PORT=5090 SIP_AUTOCALL=echo SIP_AUTOHANGUP=6 make run   # 终端 B
# INVITE → 100/180/200 → ACK → 50pps RTP（全数回声）→ BYE → 200
```

## 六、本轮踩到的三个协议坑（都是真问题）

| 坑 | 现象 | 根因/解法 |
|----|------|-----------|
| **ACK 的 CSeq 不能自增** | 对端不停重传 200 OK，日志刷屏 | 2xx 的 ACK 必须与 INVITE **同号**（都是 1）；只有 BYE 才是新事务 +1 |
| **ACK 要用 Contact 地址** | 发到代理地址，对话匹配失败 | ACK/BYE 是端到端，Request-URI 取 200 OK 的 Contact；解析失败才退回响应源地址 |
| **To 头必须原样回填** | 同上（匹配不上对话） | 用 200 OK 里的 To 原文（含 tag），不要自己拼 URI |

## 七、真实音频（M3 收尾）

### 7.1 链路

```
麦克风 → SDL采集回调(20ms/160采样) → 采集环 → G.711A 编码 → RTP/12B头+160B → UDP
UDP → RTP解包(跳过扩展头) → G.711A 解码 → 播放环(60ms预缓冲) → SDL播放回调 → 喇叭
```

- **编码**：PCMA（payload 8）为默认，同时支持 PCMU（0）；**按对端 200 OK 的 SDP 决定**（应答方说了算）。
- **帧率**：20ms 一帧，160 采样 = 160 字节 → 64 kbps，与国际窄带语音一致。
- **发送节奏**：按墙上时钟补帧（落后一次最多补 3 帧）。用"每轮 select 发一帧"会因系统定时器
  精度（Windows 默认 15.6ms）跑成 33fps，对端播放缓冲会被抽干。
- **预缓冲**：播放侧攒够 60ms 才开播，用一点延迟换掉抖动爆音。
- **降级**：没有麦克风 → 只收不放（能听见门口机）；设备全无 → 回声模式（媒体通路自检）。

### 7.2 无麦克风也能验（自测音源）

```bash
# 终端 1：作为被叫（自动接听）
SIP_SERVER=127.0.0.1 SIP_USER=101 SIP_PASS=101 SIP_LOCAL_PORT=5064 \
SIP_AUTOANSWER=1 ./bin/main.exe

# 终端 2：作为主叫，用 1kHz 正弦代替麦克风，5 秒后挂断
SIP_SERVER=127.0.0.1 SIP_USER=100 SIP_PASS=100 SIP_LOCAL_PORT=5062 \
SIP_AUTOCALL=101 SIP_AUTOHANGUP=5 SIP_TONE=1 ./bin/main.exe
```

实测结果（本机跑通）：

```
A(发) [rtp] stats rx=0   tx=250 dur=5s (pps=50.0)   [audio] underrun=0
B(收) [rtp] stats rx=250 tx=0   dur=5s             [audio] underrun=0
```

`pps=50.0` = 20ms 一帧 = 64kbps，**收发两侧计数一致**，说明 G.711 编解码与 RTP 封包时戳都正确。
板子上 `SIP_TONE=1` 还能当"对讲自检"用：喇叭里应听到连续的 1kHz 嘟声。

### 7.3 两个媒体侧的坑

| 坑 | 现象 | 根因/解法 |
|---|---|---|
| **SDL 返回立体声设备** | 播放欠载 400+ 次，声音断续 | `allowed_changes` 传 0 强制单声道；否则每 20ms 被要走 320 采样（2ch） |
| **音频初始化算进通话时长** | 一接通就自动挂断 | Windows WASAPI 首次开设备可耗 1~3s；`t_answer_ms` 必须放在 `rtp_start_media()` **之后** |

## 八、呼出取消（CANCEL）与终态 ACK

呼出中挂断必须发 **CANCEL**，而且：

1. **CANCEL 复用 INVITE 的 branch + 同号 CSeq**（只换方法名）——否则服务器认为取消的是另一笔事务；
2. 收到 `487 Request Terminated` **之后还要回 ACK**（RFC 3261 事务层要求，非 2xx 终态也要 ACK）；
3. 所以**发完 CANCEL 不能立刻清对话状态**（branch/Call-ID 还要用），标记 `cancelled` 等 487 再清；
   5 秒兜底超时，避免一直占线。

实测（100 呼 102，3 秒后取消）：

```
A: >> CANCEL (cseq=1 branch=z9hG4bK104736) → << 200 OK → << 487 → >> ACK
B: << CANCEL → >> 200 OK → >> 487 → << ACK
```

非 2xx 的 ACK 缺失是最容易被忽略的一条：没有它，UAS/代理会按事务定时器**反复重传 480/486/487**，
日志看起来像"服务器抽风"，其实是客户端没确认。

## 九、路线图

- [x] REGISTER / 200 OK（UDP，select 收包线程）
- [x] 401 Digest 认证（MD5，qop=auth / 无 qop 双支持）
- [x] 注册续期（Expires 一半时重发，`SIP_EXPIRES` 可调）
- [x] INVITE 呼入：100 Trying → 180 Ringing → 200 OK(SDP) → ACK → BYE/200
- [x] INVITE 呼出：带 SDP，按 Contact 发 ACK
- [x] 呼出取消 CANCEL + 487 ACK（含 5s 兜底）
- [x] 非 2xx 终态 ACK（480/486/487 不再被重传）
- [x] 对讲页 `ui_call.c`：来电（接听/拒接）+ 通话中（计时、RTP 收发计数），事件驱动压/出栈
- [x] 真实音频：SDL 采集/播放 + G.711 A/μ 编解码 + 60ms 预缓冲 + 补帧节拍
- [x] 无麦克风降级（只收不放）与回声模式（媒体自检）
- [ ] 视频（H.264 解码 + `lv_image` 显示，室内机产品形态必需，**M3 剩余最后一块**）
- [ ] 通话中 401 认证（INVITE 被挑战）、事务层重传（eXosip 迁移时一并解决）
