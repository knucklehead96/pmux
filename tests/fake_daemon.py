#!/usr/bin/env python3
"""A stand-in pmux daemon for version-handshake tests.

    fake_daemon.py SOCKET PIDFILE LOG [--hello PROTOCOL] [--no-list]
                   [--running NAME:PID]... [--pidfile-pid PID]
                   [--layout current|idle-last-output|idle]

Listens on SOCKET (creating its directory with mode 0700), writes its pid
(or --pidfile-pid) to PIDFILE and appends the type of every frame it gets
to LOG, one per line.  Without --hello it acts like a daemon older than
HELLO and --stop: every request is answered with ERROR "unsupported request"
except LIST, answered with the --running entries (unless --no-list).  With
--hello it answers HELLO with that protocol version, pmux version 9.9.9 and
its pid, and STOP with OK before exiting.  --layout picks the LIST_REPLY
layout: the current one, or those of the builds before 0.1.0 (idle_ms, and
up to c6a31af also last_output).  SIGTERM makes it remove SOCKET
and PIDFILE and exit, as the real daemon does.
"""
import argparse
import os
import selectors
import signal
import socket
import struct
import sys

LIST, LIST_REPLY, ERROR, OK, STOP, HELLO = 1, 2, 14, 15, 17, 20


def pstr(b):
    return struct.pack("<I", len(b)) + b


def frame(typ, payload=b""):
    return struct.pack("<IB", len(payload) + 1, typ) + payload


def proc_list(running, layout):
    out = struct.pack("<I", len(running))
    for i, (name, pid) in enumerate(running, 1):
        out += struct.pack("<I", i) + pstr(name.encode()) + pstr(b"/") + struct.pack("<I", 1)
        out += pstr(b"sleep") + struct.pack("<i", pid)
        if layout == "current":
            out += struct.pack("<QBiQB", 0, 0, 0, 0, 0) + pstr(b"")
        elif layout == "idle-last-output":
            out += struct.pack("<QQBiQQB", 0, 0, 0, 0, 0, 0, 0) + pstr(b"")
        else:
            out += struct.pack("<QQBi", 0, 0, 0, 0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sock")
    ap.add_argument("pidfile")
    ap.add_argument("log")
    ap.add_argument("--hello", type=int)
    ap.add_argument("--no-list", action="store_true")
    ap.add_argument("--running", action="append", default=[])
    ap.add_argument("--pidfile-pid", type=int)
    ap.add_argument("--layout", default="current", choices=("current", "idle-last-output", "idle"))
    a = ap.parse_args()
    running = [(r.split(":")[0], int(r.split(":")[1])) for r in a.running]

    os.makedirs(os.path.dirname(a.sock), mode=0o700, exist_ok=True)

    def cleanup(*_):
        for path in (a.sock, a.pidfile):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        sys.exit(0)

    signal.signal(signal.SIGTERM, cleanup)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(a.sock)
    listener.listen(16)
    with open(a.pidfile, "w") as f:
        f.write("%d\n" % (a.pidfile_pid or os.getpid()))

    sel = selectors.DefaultSelector()
    sel.register(listener, selectors.EVENT_READ)
    bufs = {}
    while True:
        for key, _ in sel.select():
            if key.fileobj is listener:
                conn, _ = listener.accept()
                sel.register(conn, selectors.EVENT_READ)
                bufs[conn] = b""
                continue
            conn = key.fileobj
            data = conn.recv(65536)
            if not data:
                sel.unregister(conn)
                conn.close()
                del bufs[conn]
                continue
            bufs[conn] += data
            while len(bufs[conn]) >= 5:
                n = struct.unpack_from("<I", bufs[conn])[0]
                if len(bufs[conn]) < 4 + n:
                    break
                typ = bufs[conn][4]
                bufs[conn] = bufs[conn][4 + n:]
                with open(a.log, "a") as f:
                    f.write("%d\n" % typ)
                if typ == HELLO and a.hello is not None:
                    conn.sendall(frame(HELLO, struct.pack("<I", a.hello) + pstr(b"9.9.9")
                                       + struct.pack("<I", os.getpid())))
                elif typ == STOP and a.hello is not None:
                    conn.sendall(frame(OK))
                    cleanup()
                elif typ == LIST and not a.no_list:
                    conn.sendall(frame(LIST_REPLY, proc_list(running, a.layout)))
                else:
                    conn.sendall(frame(ERROR, pstr(b"unsupported request")))


if __name__ == "__main__":
    main()
