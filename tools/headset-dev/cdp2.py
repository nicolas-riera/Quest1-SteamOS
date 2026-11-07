#!/usr/bin/python3
# cdp2.py "<page title substring>" '<json list of [method, params]>' : send CDP commands, print results (screenshots saved as /tmp/cdpshot<N>.png)
import socket, os, base64, json, struct, sys, urllib.request
title, cmds = sys.argv[1], json.loads(sys.argv[2])
pages = json.load(urllib.request.urlopen('http://127.0.0.1:8080/json'))
pg = ([p for p in pages if title == p["title"]] or [p for p in pages if title in p["title"]])[0]
s = socket.create_connection(('127.0.0.1', 8080))
key = base64.b64encode(os.urandom(16)).decode()
path = pg['webSocketDebuggerUrl'].split('8080', 1)[1]
s.sendall(f'GET {path} HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n'.encode())
buf = b''
while b'\r\n\r\n' not in buf: buf += s.recv(4096)
buf = buf.split(b'\r\n\r\n', 1)[1]
def send(o):
    d = json.dumps(o).encode(); m = os.urandom(4); n = len(d); h = bytes([0x81])
    if n < 126: h += bytes([0x80 | n])
    elif n < 65536: h += bytes([0x80 | 126]) + struct.pack('>H', n)
    else: h += bytes([0x80 | 127]) + struct.pack('>Q', n)
    s.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))
def recvn(n):
    global buf
    while len(buf) < n:
        c = s.recv(1 << 20)
        if not c: raise EOFError
        buf += c
    r, buf = buf[:n], buf[n:]; return r
def recv():
    data = b''
    while True:
        b0, b1 = recvn(2); n = b1 & 0x7f
        if n == 126: n = struct.unpack('>H', recvn(2))[0]
        elif n == 127: n = struct.unpack('>Q', recvn(8))[0]
        data += recvn(n)
        if b0 & 0x80: return json.loads(data)
import time
for i, (m, p) in enumerate(cmds):
    if m == 'sleep': time.sleep(p); continue
    send({'id': i + 1, 'method': m, 'params': p})
    while True:
        r = recv()
        if r.get('id') == i + 1: break
    if m == 'Page.captureScreenshot' and 'result' in r:
        open(f'/tmp/cdpshot{i}.png', 'wb').write(base64.b64decode(r['result']['data'])); print(m, 'saved', f'/tmp/cdpshot{i}.png')
    else:
        print(m, json.dumps(r.get('result', r.get('error')))[:600])
