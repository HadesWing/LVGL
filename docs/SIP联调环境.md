# SIP 对讲联调环境

> 客户端：`src/app/app_sip.c`（oSIP 编解码 + 自研 UDP 收发/状态机）
> 服务端：本机二选一 —— 假应答器（纯 Python，够测注册）或 miniSIPServer（真实 SIP 服务器）

## 一、零安装方案：假应答器（当前默认）

支持完整 Digest 注册闭环：`REGISTER → 401 挑战 → REGISTER(Authorization) → 200 OK`。
不做注册表/不处理 INVITE —— 用于回归注册链路。

```bash
# 终端 1：起服务（用户名/密码跟在端口后面，默认 1001/1001）
python scripts/fake_sip_responder.py 5060 1001 1234

# 终端 2：起室内机（环境变量驱动，见 src/app/app_sip.c 的 app_sip_init_from_env）
SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=1001 SIP_PASS=1234 SIP_LOCAL_PORT=5062 \
  ./build/main    # Windows: bin\main.exe
```

预期日志：

```
[sip] << 401 Unauthorized
[sip] 401 challenge: realm="asterisk" nonce=deadbeef... qop=auth
[sip] REGISTER(auth) sent
[sip] << 200 OK
```

服务端打印 `digest OK` 表示 RFC 2617 摘要两端算对。

## 二、真实方案：miniSIPServer（免费版 ≤5 用户）

下载：https://www.myvoipuser.com/download/ （`mss_v70_u5.exe`，Windows）

1. 安装后启动，默认监听 UDP 5060；
2. 添加两个分机：`1001` / `1002`，密码自定；
3. 客户端起两个实例（注意本机端口错开）：

```bash
# 室内机（本工程）
SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=1001 SIP_PASS=1234 SIP_LOCAL_PORT=5062 ./bin/main.exe

# 对端软终端：MicroSIP / Zoiper 注册 1002（或再起一个 main.exe 配 1002）
```

4. 真实服务器通常开启 401 认证 —— 本客户端已实现 Digest（qop=auth 与无 qop 两种），
   若注册失败看服务端日志里 nonce 是否刷新（重放检测）。

## 三、已知坑（客户端侧，均已修复/规避）

| # | 坑 | 说明 |
|---|-----|------|
| 1 | **解析崩溃（0xC0000005）** | oSIP 的 `osip_message_parse` 依赖 `parser_init()` 建的头域哈希表，而它只被 `osip_init()` 触发。启动后必须先 `osip_init(&osip)`，退出 `osip_release(osip)`。只"组包不解析"不会暴露此坑 |
| 2 | **字面量传给 set_method** | `osip_message_set_method/set_version` 接管指针所有权，传 `"REGISTER"` 字面量 → free 时堆损坏。必须 `osip_strdup()` |
| 3 | **头文件顺序** | `<winsock2.h>` 必须在 `<osip2/osip.h>` 之前（后者用到 `struct timeval`） |
| 4 | **401 重放死循环** | 带认证重发后再收 401 直接置 ERROR，不再重试（`s_auth_tried` 门闩） |

## 三、客户端选型（对面那台"门口机"用什么）

| 客户端 | 版本/体积 | 适合 | 不适合 |
|--------|----------|------|--------|
| **MicroSIP**（首选） | 3.22.20，便携 zip 8MB（解压 19MB），GPL | 音频对讲调参；**Call Manager 能直接看原始 SIP 报文**（调 oSIP 极有用）；支持**不注册直接拨 SIP URI** | 视频能力弱（有但不好用） |
| **Linphone Desktop** | 6.1+（`winget install --id BelledonneCommunications.Linphone -e`） | **视频对讲**（H.264/VP8），室内机产品形态需要 | 体积大、界面重、日志不直观 |
| Zoiper 5 | 免费版 | 界面友好、手机端也有 | 视频/多账号要收费 |
| 本工程自身 | — | 两个实例互打（1001↔1002），验证自家栈 | 不能当参考实现 |

**结论：现在（音频阶段）用 MicroSIP；等做视频再上 Linphone。**
本机已放便携版：`C:\sip_tools\MicroSIP-3.22.20\MicroSIP.exe`（免安装，双击即用）。

### 3.1 场景 A：不装任何服务器（最快验证 INVITE 收包）

MicroSIP 支持 direct IP call，不需要注册、不需要 SIP 服务器：

1. MicroSIP 菜单 → **Add Account** → 只填 `SIP Server` 留空，`Username` 填 `1002`（任意）；
2. 拨号框直接输入 **`sip:1001@127.0.0.1:5062`**（5062 = 我们客户端的 `SIP_LOCAL_PORT`）；
3. 我们客户端应在日志打 `[sip] << INVITE`，状态转 `SIP_INCOMING`。

> 前提：`app_sip.c` 的 INVITE 收包分支要能应答 100/180/200 —— 见第四节路线图待办。

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

## 四、路线图

- [x] REGISTER / 200 OK（UDP，select 收包线程）
- [x] 401 Digest 认证（MD5，qop=auth / 无 qop 双支持）
- [ ] INVITE / 200 OK / ACK / BYE（对话内 cseq、To-tag 维护）
- [ ] 呼入振铃 UI（`SIP_INCOMING` 状态已有事件，`ui_call.c` 待写）
- [ ] RTP 音频（PCMA 8k，先 loopback 自测）
