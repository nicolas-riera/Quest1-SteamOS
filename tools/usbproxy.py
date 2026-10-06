#!/usr/bin/env python3
"""Tiny HTTP/HTTPS (CONNECT) proxy so the headset reaches the internet over the USB link.

Run on the PC:  python tools/usbproxy.py [listen_addr] [port]   (default 192.168.77.2:3128)
On the headset: export http_proxy=http://192.168.77.2:3128 https_proxy=$http_proxy
"""
import asyncio
import sys

LISTEN = sys.argv[1] if len(sys.argv) > 1 else "192.168.77.2"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3128


async def pipe(reader, writer):
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except (ConnectionError, asyncio.CancelledError):
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass


async def handle(creader, cwriter):
    try:
        head = await creader.readuntil(b"\r\n\r\n")
    except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
        cwriter.close()
        return
    line, _, rest = head.partition(b"\r\n")
    try:
        method, target, version = line.decode("latin-1").split(" ", 2)
    except ValueError:
        cwriter.close()
        return
    try:
        if method == "CONNECT":
            host, _, port = target.rpartition(":")
            sreader, swriter = await asyncio.open_connection(host, int(port))
            cwriter.write(b"HTTP/1.1 200 Connection established\r\n\r\n")
            await cwriter.drain()
        else:
            # absolute-form request: http://host[:port]/path
            if not target.startswith("http://"):
                raise ValueError("bad target")
            hostport, _, path = target[7:].partition("/")
            host, _, port = hostport.partition(":")
            sreader, swriter = await asyncio.open_connection(host, int(port or 80))
            swriter.write(f"{method} /{path} {version}\r\n".encode("latin-1") + rest)
            await swriter.drain()
    except Exception as e:
        cwriter.write(f"HTTP/1.1 502 Bad Gateway\r\n\r\n{e}\r\n".encode())
        cwriter.close()
        return
    await asyncio.gather(pipe(creader, swriter), pipe(sreader, cwriter))


async def main():
    server = await asyncio.start_server(handle, LISTEN, PORT)
    print(f"proxy on {LISTEN}:{PORT}", flush=True)
    async with server:
        await server.serve_forever()


asyncio.run(main())
