# obsws.py REQUEST [JSON] [--save FILE]: one obs-websocket (v5, built into OBS) request to the OBS running in
# tools/test/vm's VM, on localhost:4455 without a password, and its answer as JSON. --save FILE writes a
# GetSourceScreenshot's image there (its imageData, decoded). The standard library only: a websocket by hand.
import base64
import hashlib
import json
import os
import socket
import struct
import sys


def send(sock, text):
    data = text.encode()
    head = bytes([0x81])  # a final text frame
    n = len(data)
    if n < 126:
        head += bytes([0x80 | n])
    elif n < 65536:
        head += bytes([0x80 | 126]) + struct.pack("!H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack("!Q", n)
    mask = os.urandom(4)
    sock.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def recv_exact(sock, n):
    out = b""
    while len(out) < n:
        chunk = sock.recv(n - len(out))
        if not chunk:
            raise ConnectionError("closed")
        out += chunk
    return out


def recv(sock):
    text = b""
    while True:
        b0, b1 = recv_exact(sock, 2)
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack("!H", recv_exact(sock, 2))[0]
        elif n == 127:
            n = struct.unpack("!Q", recv_exact(sock, 8))[0]
        payload = recv_exact(sock, n)
        if b0 & 0x0F in (0x1, 0x0):
            text += payload
            if b0 & 0x80:
                return json.loads(text)
        elif b0 & 0x0F == 0x8:
            raise ConnectionError("closed by OBS")


def main(args):
    save = None
    if "--save" in args:
        i = args.index("--save")
        save = args[i + 1]
        args = args[:i] + args[i + 2:]
    request = args[0]
    data = json.loads(args[1]) if len(args) > 1 else {}
    sock = socket.create_connection(("127.0.0.1", 4455), timeout=30)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:4455\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                  "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(1)
    accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
    if accept.encode() not in head:
        raise SystemExit(f"obsws.py: no websocket: {head[:200]!r}")
    recv(sock)  # Hello
    send(sock, json.dumps({"op": 1, "d": {"rpcVersion": 1, "eventSubscriptions": 0}}))
    recv(sock)  # Identified
    send(sock, json.dumps({"op": 6, "d": {"requestType": request, "requestId": "h3d", "requestData": data}}))
    while True:
        m = recv(sock)
        if m.get("op") == 7:
            break
    d = m["d"]
    if save and d.get("responseData", {}).get("imageData"):
        img = d["responseData"]["imageData"]
        with open(save, "wb") as f:
            f.write(base64.b64decode(img.split(",", 1)[1]))
        d["responseData"]["imageData"] = f"(saved to {save})"
    print(json.dumps(d))


if __name__ == "__main__":
    main(sys.argv[1:])
