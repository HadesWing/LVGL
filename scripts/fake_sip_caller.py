#!/usr/bin/env python3
"""
脚本化 SIP 呼叫方（UAC）—— 用来在无界面的情况下验证我们客户端的收话链路

做的事：
    INVITE(带 SDP) → 等 100/180/200 → ACK → 发 RTP 并统计回声 → BYE

用法：
    python scripts/fake_sip_caller.py [目标端口] [目标号码]

前置：被测客户端以 SIP_AUTOANSWER=1 启动（自动接听，无需点界面）
    SIP_SERVER=127.0.0.1 SIP_PORT=5060 SIP_USER=1001 SIP_LOCAL_PORT=5062 SIP_AUTOANSWER=1 ./bin/main.exe
"""
import os
import random
import socket
import struct
import sys
import time

LOCAL_PORT = 4100          # 本脚本的 RTP 端口
SIP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5062
TARGET = sys.argv[2] if len(sys.argv) > 2 else "1001"


def header(msg, name):
    for line in msg.split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def build_invite(call_id, tag, branch, cseq=1):
    sdp = (
        "v=0\r\n"
        "o=- 1 1 IN IP4 127.0.0.1\r\n"
        "s=caller\r\n"
        "c=IN IP4 127.0.0.1\r\n"
        "t=0 0\r\n"
        f"m=audio {LOCAL_PORT + 1} RTP/AVP 8\r\n"
        "a=rtpmap:8 PCMA/8000\r\n"
        "a=sendrecv\r\n"
    )
    return (
        f"INVITE sip:{TARGET}@127.0.0.1:{SIP_PORT} SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{LOCAL_PORT};branch={branch};rport\r\n"
        f"Max-Forwards: 70\r\n"
        f"From: \"门口机\" <sip:1002@127.0.0.1>;tag={tag}\r\n"
        f"To: <sip:{TARGET}@127.0.0.1>\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq} INVITE\r\n"
        f"Contact: <sip:1002@127.0.0.1:{LOCAL_PORT}>\r\n"
        "Content-Type: application/sdp\r\n"
        f"Content-Length: {len(sdp)}\r\n\r\n"
        + sdp
    )


def main():
    sip = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sip.bind(("0.0.0.0", LOCAL_PORT))
    sip.settimeout(3)

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind(("0.0.0.0", LOCAL_PORT + 1))   # 模拟对端 RTCP 占位，避免端口冲突
    rtp.settimeout(0.2)

    call_id = f"{random.getrandbits(32):08x}@127.0.0.1"
    tag = f"vto{random.getrandbits(16):04x}"
    branch = f"z9hG4bK{random.getrandbits(32):08x}"
    cseq = 1
    dst = ("127.0.0.1", SIP_PORT)

    print(f"[caller] INVITE -> {dst}  from 1002 to {TARGET}", flush=True)
    sip.sendto(build_invite(call_id, tag, branch, cseq).encode(), dst)

    to_hdr = None
    status = None
    rtp_remote = None
    deadline = time.time() + 6
    while time.time() < deadline:
        try:
            data, addr = sip.recvfrom(65535)
        except socket.timeout:
            print("[caller] timeout waiting response", flush=True)
            break
        text = data.decode("utf-8", "replace")
        first = text.split("\r\n", 1)[0]
        print(f"[caller] << {first}", flush=True)
        status = int(first.split()[1]) if first.startswith("SIP/2.0") else None
        if status and status >= 200:
            to_hdr = header(text, "To")
        if status == 200:
            # 从应答的 SDP 里取对端 RTP 端口（必须按它的 SDP 发，不能瞎猜）
            body = text.split("\r\n\r\n", 1)[1] if "\r\n\r\n" in text else ""
            for line in body.split("\r\n"):
                if line.startswith("m=audio "):
                    rtp_remote = int(line.split()[1])
        if status in (200, 486, 603):
            break

    if status != 200:
        print(f"[caller] call not answered (status={status})", flush=True)
        return 1
    if not rtp_remote:
        print("[caller] no RTP port in 200 OK SDP", flush=True)
        return 1
    print(f"[caller] remote RTP port = {rtp_remote}", flush=True)

    # ACK
    ack = (
        f"ACK sip:{TARGET}@127.0.0.1:{SIP_PORT} SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{LOCAL_PORT};branch={branch};rport\r\n"
        f"Max-Forwards: 70\r\n"
        f"From: \"门口机\" <sip:1002@127.0.0.1>;tag={tag}\r\n"
        f"To: {to_hdr}\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq} ACK\r\n"
        "Content-Length: 0\r\n\r\n"
    )
    sip.sendto(ack.encode(), dst)
    print("[caller] >> ACK", flush=True)

    # RTP：发 50 个 PCMA 静音包（0xD5），统计回声
    seq, ts, ssrc = 1, 0, random.getrandbits(32)
    echoed = 0
    for i in range(50):
        hdr = struct.pack("!BBHII", 0x80, 8, seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
        payload = bytes([0xD5]) * 160
        rtp.sendto(hdr + payload, ("127.0.0.1", rtp_remote))
        seq += 1
        ts += 160
        # 读回声
        try:
            while True:
                rtp.recvfrom(2048)
                echoed += 1
        except socket.timeout:
            pass
        time.sleep(0.02)

    print(f"[caller] RTP sent 50, echoed back {echoed}", flush=True)
    print(f"[caller] RTP echo {'OK' if echoed > 0 else 'FAIL'}", flush=True)

    # BYE
    cseq += 1
    bye = (
        f"BYE sip:{TARGET}@127.0.0.1:{SIP_PORT} SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{LOCAL_PORT};branch=z9hG4bK{random.getrandbits(32):08x};rport\r\n"
        f"Max-Forwards: 70\r\n"
        f"From: \"门口机\" <sip:1002@127.0.0.1>;tag={tag}\r\n"
        f"To: {to_hdr}\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: {cseq} BYE\r\n"
        "Content-Length: 0\r\n\r\n"
    )
    sip.sendto(bye.encode(), dst)
    print("[caller] >> BYE", flush=True)
    try:
        data, _ = sip.recvfrom(65535)
        print("[caller] << " + data.decode("utf-8", "replace").split("\r\n", 1)[0], flush=True)
    except socket.timeout:
        print("[caller] no response to BYE", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
