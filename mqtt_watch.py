#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mqtt_watch.py — 最小 MQTT 观察器（纯标准库，不用 pip 装任何东西）

用途：
  python mqtt_watch.py                  # 订阅状态主题，观察 20 秒
  python mqtt_watch.py -t 60            # 观察 60 秒
  python mqtt_watch.py --selftest       # 自测：broker 通不通、收发正不正常
  python mqtt_watch.py --pub status     # 给设备发一条命令（status = 只让它报状态，不按键）
  python mqtt_watch.py --pub press      # 危险：真的会按一下电源键
  python mqtt_watch.py --pub force      # 危险：长按 6 秒硬断电

觉得默认参数不对，就改下面三行，或命令行 --host/--base 覆盖。
"""
import argparse, os, socket, ssl, struct, sys, time

HOST = "broker.emqx.io"
PORT = 8883
BASE = "pcbtn/CHANGEME"      # ← 改成你固件里的主题前缀（等于密码，别提交到公开仓库）

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---------- MQTT 3.1.1 最小编码 ----------
def _vlen(n):
    out = b""
    while True:
        d, n = n % 128, n // 128
        if n:
            d |= 0x80
        out += bytes([d])
        if not n:
            return out

def _str(s):
    b = s.encode()
    return struct.pack("!H", len(b)) + b

def _pkt(t, flags, body):
    return bytes([(t << 4) | flags]) + _vlen(len(body)) + body

def _connect(cid, keepalive=30, user=None, pwd=None, proto=4):
    flags = 0x02
    if user is not None:
        flags |= 0x80
    if pwd is not None:
        flags |= 0x40
    payload = _str(cid) + (_str(user) if user is not None else b"") + (_str(pwd) if pwd is not None else b"")
    return _pkt(1, 0, _str("MQTT") + bytes([proto, flags]) + struct.pack("!H", keepalive) + payload)

def _subscribe(pid, topic, qos=0):
    return _pkt(8, 2, struct.pack("!H", pid) + _str(topic) + bytes([qos]))

def _publish(topic, msg, retain=False):
    return _pkt(3, 0x01 if retain else 0, _str(topic) + msg.encode())

def _ping():
    return _pkt(12, 0, b"")

def _read(sock):
    h = sock.recv(1)
    if not h:
        return None
    t, fl = h[0] >> 4, h[0] & 0x0F
    mult, ln = 1, 0
    while True:
        b = sock.recv(1)
        if not b:
            return None
        ln += (b[0] & 0x7F) * mult
        if not (b[0] & 0x80):
            break
        mult *= 128
    buf = b""
    while len(buf) < ln:
        c = sock.recv(ln - len(buf))
        if not c:
            break
        buf += c
    return t, fl, buf

def connect(host, port, tls=True, cid=None, user=None, pwd=None, proto=4):
    raw = socket.create_connection((host, port), timeout=10)
    if tls:
        ctx = ssl.create_default_context()
        s = ctx.wrap_socket(raw, server_hostname=host)
    else:
        s = raw
    s.sendall(_connect(cid or ("watch_" + os.urandom(4).hex()), user=user, pwd=pwd, proto=proto))
    t, fl, body = _read(s)
    rc = body[1] if body and len(body) > 1 else -1
    if t != 2 or rc != 0:
        raise SystemExit("CONNACK 失败 rc=%d（broker 拒绝连接）" % rc)
    print("✓ 已连着 %s:%d%s（客户端ID %s）" % (host, port, " (TLS)" if tls else " (明文)", cid or "随机"))
    return s

def subscribe(s, topic):
    s.sendall(_subscribe(1, topic))
    t, fl, body = _read(s)
    if t != 9:
        raise SystemExit("SUBACK 没回来")
    print("✓ 已订阅 %s" % topic)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", "--seconds", type=int, default=20)
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--pub", metavar="PAYLOAD")
    ap.add_argument("--topic", help="只订阅这个主题（巴法云用）")
    ap.add_argument("--cid", help="指定客户端 ID（巴法云必须填私钥）")
    ap.add_argument("--no-tls", action="store_true", help="明文 TCP（巴法云 9501 用这个）")
    ap.add_argument("--user")
    ap.add_argument("--pwd")
    ap.add_argument("--proto3", action="store_true", help="用 MQTT 3.1（协议级 3）而不是 3.1.1")
    a = ap.parse_args()

    state_topic = a.topic if a.topic else a.base + "/state"
    cmd_topic = a.topic if a.topic else a.base + "/set"
    s = connect(a.host, a.port, tls=not a.no_tls, cid=a.cid,
                user=a.user, pwd=a.pwd, proto=3 if a.proto3 else 4)

    if a.selftest:
        topic = "pcbtn/selftest/" + os.urandom(3).hex()
        subscribe(s, topic)
        s.sendall(_publish(topic, "hello"))
        try:
            t, fl, body = _read(s)
        except socket.timeout:
            t = None
        if t == 3:
            n = struct.unpack("!H", body[:2])[0]
            print("✓ 自测通过：收到回环消息 ->", body[2 + n:].decode())
        else:
            print("✗ 自测失败：没收到自己发的消息")
        return

    if a.pub is not None:
        s.sendall(_publish(cmd_topic, a.pub))
        print("→ 已发送到 %s : %s" % (cmd_topic, a.pub))
        time.sleep(1)

    subscribe(s, state_topic)
    print("… 观察 %d 秒（Ctrl+C 退出）" % a.seconds)
    t0 = last_ping = time.time()
    s.settimeout(1.0)
    while time.time() - t0 < a.seconds:
        if time.time() - last_ping > 20:
            s.sendall(_ping())
            last_ping = time.time()
        try:
            r = _read(s)
        except (socket.timeout, ssl.SSLWantReadError):
            continue
        if not r:
            print("连接被关闭")
            break
        t, fl, body = r
        if t == 3:
            n = struct.unpack("!H", body[:2])[0]
            print("%s  %s" % (time.strftime("[%H:%M:%S]"), body[2 + n:].decode(errors="replace")))
    print("（观察结束）")

if __name__ == "__main__":
    main()
