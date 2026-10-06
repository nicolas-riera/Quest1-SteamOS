#!/usr/bin/env python3
"""Run commands on the headset's native debug shell (busybox telnetd on 192.168.77.1:23).

  qsh.py 'cmd ...'                  run a command, print its output, exit with its status
  qsh.py --put LOCAL REMOTE         copy a file (raw TCP via busybox nc on port 9000)
  echo script | qsh.py -            run a script read from stdin
Runs from WSL (the USB NCM link is reachable through Windows' NAT)."""
import os
import re
import socket
import sys
import time
import uuid

HOST = os.environ.get("QHOST", "192.168.77.1")


def strip_telnet(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b == 255 and i + 1 < len(data):  # IAC
            cmd = data[i + 1]
            i += 3 if cmd in (251, 252, 253, 254) else 2
            continue
        out.append(b)
        i += 1
    return bytes(out)


def run(cmd: str, timeout: float = 600) -> int:
    s = socket.create_connection((HOST, 23), timeout=10)
    tag = uuid.uuid4().hex[:8]
    time.sleep(0.3)
    s.recv(65536)
    # quiet prompt/echo, then run the command and print a marker with its status
    s.sendall(b"stty -echo 2>/dev/null; PS1=''; export PS1\n")
    time.sleep(0.2)
    payload = f"{cmd}\necho __Q{tag}_$?__\n"
    s.sendall(payload.encode())
    buf = b""
    end = re.compile(rf"__Q{tag}_(\d+)__".encode())
    deadline = time.time() + timeout
    s.settimeout(1)
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += strip_telnet(chunk)
        m = end.search(buf)
        if m:
            text = buf[: m.start()].decode(errors="replace").replace("\r", "")
            # drop the stty line's leftovers
            text = re.sub(r"^.*?PS1.*?\n", "", text, count=1, flags=re.S) if "PS1=''" in text else text
            sys.stdout.write(text)
            s.sendall(b"exit\n")
            s.close()
            return int(m.group(1))
    sys.stdout.write(buf.decode(errors="replace"))
    return 124


def put(local: str, remote: str) -> int:
    size = os.path.getsize(local)
    ctl = socket.create_connection((HOST, 23), timeout=10)
    time.sleep(0.3)
    ctl.recv(65536)
    ctl.sendall(f"nc -l -p 9000 > '{remote}'\n".encode())
    time.sleep(0.7)
    with socket.create_connection((HOST, 9000), timeout=30) as d, open(local, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            d.sendall(b)
        d.shutdown(socket.SHUT_WR)
        time.sleep(0.5)
    ctl.close()
    time.sleep(0.5)
    return run(f"[ $(wc -c < '{remote}') -eq {size} ] && echo 'put ok {remote} ({size} bytes)'")


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "--put":
        sys.exit(put(sys.argv[2], sys.argv[3]))
    cmd = sys.stdin.read() if sys.argv[1:] == ["-"] else " ".join(sys.argv[1:])
    sys.exit(run(cmd))
