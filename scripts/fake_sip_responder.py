#!/usr/bin/env python3
"""
极简 SIP 注册服务器（仅本机自测用，支持 Digest 认证挑战）

流程：
    REGISTER(无认证) → 401 + WWW-Authenticate(challenge)
    REGISTER(Authorization) → 校验 RFC 2617 Digest response → 200 OK / 403

这不是真正的 SIP 服务器（不做注册表、不路由 INVITE），
真实联调用 miniSIPServer / Asterisk，见 docs/SIP联调环境.md。

用法：
    python scripts/fake_sip_responder.py [端口] [用户名] [密码]
"""
import hashlib
import socket
import sys


def header(msg, name):
    for line in msg.split("\r\n"):
        if line.lower().startswith(name.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def auth_param(auth_value, name):
    """从 Authorization 头取参数（去掉引号）"""
    if not auth_value:
        return None
    for part in auth_value.split(","):
        k, _, v = part.strip().partition("=")
        if k.strip().lower() == name.lower():
            return v.strip().strip('"')
    return None


def md5hex(s):
    return hashlib.md5(s.encode()).hexdigest()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5060
    user = sys.argv[2] if len(sys.argv) > 2 else "1001"
    password = sys.argv[3] if len(sys.argv) > 3 else "1001"
    realm = "asterisk"          # 任意即可，客户端只做回显
    nonce = "deadbeef12345678"  # 本机测试固定值即可

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", port))
    print(f"[fake-sip] listening udp/{port}  user={user} pass={password} realm={realm}", flush=True)

    while True:
        data, addr = sock.recvfrom(65535)
        text = data.decode("utf-8", "replace")
        first = text.split("\r\n", 1)[0]
        print(f"[fake-sip] << {addr[0]}:{addr[1]}  {first}", flush=True)

        via = header(text, "Via")
        frm = header(text, "From")
        to = header(text, "To")
        call_id = header(text, "Call-ID")
        cseq = header(text, "CSeq")

        # ---------------- INVITE：当作被叫（UAS），直接 100/180/200 ----------------
        if first.startswith("INVITE"):
            sdp = (
                "v=0\r\n"
                "o=- 2 2 IN IP4 127.0.0.1\r\n"
                "s=fake-uas\r\n"
                "c=IN IP4 127.0.0.1\r\n"
                "t=0 0\r\n"
                "m=audio 4300 RTP/AVP 8\r\n"
                "a=rtpmap:8 PCMA/8000\r\n"
                "a=sendrecv\r\n"
            )
            to_tagged = to + ";tag=uas67890" if to and "tag=" not in to else to

            def send(code, reason, body=None, extra=""):
                head = (
                    f"SIP/2.0 {code} {reason}\r\n"
                    f"Via: {via}\r\n"
                    f"From: {frm}\r\n"
                    f"To: {to_tagged}\r\n"
                    f"Call-ID: {call_id}\r\n"
                    f"CSeq: {cseq}\r\n"
                    f"Contact: <sip:1002@127.0.0.1:{port}>\r\n"
                    + extra
                )
                if body:
                    head += "Content-Type: application/sdp\r\n"
                head += f"Content-Length: {len(body) if body else 0}\r\n\r\n"
                sock.sendto((head + (body or "")).encode(), addr)
                print(f"[fake-sip] >> {code} {reason}", flush=True)

            send(100, "Trying")
            send(180, "Ringing")
            send(200, "OK", sdp)
            continue

        if first.startswith("ACK"):
            print("[fake-sip] << ACK (call established)", flush=True)
            continue

        if first.startswith("BYE"):
            to_tagged = to + ";tag=uas67890" if to and "tag=" not in to else to
            resp = (
                "SIP/2.0 200 OK\r\n"
                f"Via: {via}\r\n"
                f"From: {frm}\r\n"
                f"To: {to_tagged}\r\n"
                f"Call-ID: {call_id}\r\n"
                f"CSeq: {cseq}\r\n"
                "Content-Length: 0\r\n\r\n"
            )
            sock.sendto(resp.encode(), addr)
            print("[fake-sip] >> 200 OK (BYE)", flush=True)
            continue

        if not first.startswith("REGISTER"):
            print("[fake-sip] other request, ignore", flush=True)
            continue

        authz = header(text, "Authorization")

        def reply(code, reason, extra=""):
            to_tag = to + ";tag=srv12345" if to and "tag=" not in to else to
            resp = (
                f"SIP/2.0 {code} {reason}\r\n"
                f"Via: {via}\r\n"
                f"From: {frm}\r\n"
                f"To: {to_tag}\r\n"
                f"Call-ID: {call_id}\r\n"
                f"CSeq: {cseq}\r\n"
                + extra
                + "Content-Length: 0\r\n\r\n"
            )
            sock.sendto(resp.encode(), addr)
            print(f"[fake-sip] >> {code} {reason}", flush=True)

        if not authz:
            # 第一次：下发 401 挑战
            challenge = (
                f'WWW-Authenticate: Digest realm="{realm}", nonce="{nonce}", qop="auth"\r\n'
            )
            reply(401, "Unauthorized", challenge)
            continue

        # 第二次：按 RFC 2617 校验
        resp_client = auth_param(authz, "response")
        uri = auth_param(authz, "uri") or f"sip:{user}@127.0.0.1"
        cnonce = auth_param(authz, "cnonce") or ""
        nc = auth_param(authz, "nc") or "00000001"
        qop = auth_param(authz, "qop") or "auth"

        ha1 = md5hex(f"{user}:{realm}:{password}")
        ha2 = md5hex(f"REGISTER:{uri}")
        expect = md5hex(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")

        if resp_client == expect:
            print(f"[fake-sip] digest OK  uri={uri}", flush=True)
            reply(200, "OK")
        else:
            print(f"[fake-sip] digest FAIL  expect={expect} got={resp_client}", flush=True)
            reply(403, "Forbidden")


if __name__ == "__main__":
    main()
