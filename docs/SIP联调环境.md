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

## 四、路线图

- [x] REGISTER / 200 OK（UDP，select 收包线程）
- [x] 401 Digest 认证（MD5，qop=auth / 无 qop 双支持）
- [ ] INVITE / 200 OK / ACK / BYE（对话内 cseq、To-tag 维护）
- [ ] 呼入振铃 UI（`SIP_INCOMING` 状态已有事件，`ui_call.c` 待写）
- [ ] RTP 音频（PCMA 8k，先 loopback 自测）
