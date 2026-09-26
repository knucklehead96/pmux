#!/usr/bin/env python3
"""In-pmux probe application used by the test suite.

Usage: probe.py [--ready PATH] [--winch-mark TEXT] MODE ARGS...

The probe puts its controlling tty (fd 0) into raw mode, installs whatever
signal handlers the mode needs, and only then creates the --ready file (its
content is the probe's pid).  Tests wait for that file before sending any
input so that no byte can be mangled by the pty line discipline.

--winch-mark TEXT makes every SIGWINCH clear the screen and print
"[winch:TEXT]" (the brackets keep the marker from matching the probe's own
command line, e.g. in the pmux list).  Not combinable with "emit ... winch"
or "winsize"/"signals", which install their own SIGWINCH handler.

Modes:
  inlog <path>          Append every input byte to <path> (unbuffered, one
                        write per read).  Exit 0 after TERMINATOR is seen.
  emit <corpus> [winch] On each input read containing START_BYTE (and, with
                        the optional 'winch' flag, on each SIGWINCH) write
                        START_MARKER + corpus + END_MARKER to stdout.  Keeps
                        running afterwards.
  winsize <path>        Append "start R C\\n" once at startup, then
                        "R C\\n" on every SIGWINCH.
  signals <path>        Append the name of every received HUP/WINCH/QUIT/
                        INT/TERM/CONT/USR1 signal, one per line.  Never
                        exits because of them.
  env <path>            Dump os.environ as JSON to <path> (atomically).
  exit <code> [delay]   Exit on the first input byte or after <delay>
                        seconds (if given).  <code> is an int exit status or
                        'sig<N>' to die by signal N.
  draw <script> [key=value ...]
                        Write the bytes of file <script> to stdout once at
                        startup (before the ready file), then idle.  Options:
                          winch=<file>  write <file>'s bytes on every SIGWINCH
                          wlog=<path>   append "R C\n" on every SIGWINCH
                          usr1=<file>   write <file>'s bytes on every SIGUSR1,
                                        then append "1\n" to <file>.done
                          inlog=<path>  append every input byte to <path>
                          exit=<code>   exit with <code> 0.3 s after startup
                        Nothing is ever written in response to input, so the
                        screen only changes through these scripts.
"""
import fcntl
import json
import os
import select
import signal
import struct
import sys
import termios
import time
import tty

TERMINATOR = b"\x00ENDPROBE"
START_BYTE = b"G"
START_MARKER = b"\x1b_PMUXPROBE-START-5f0c9a3e\x1b\\"
END_MARKER = b"\x1b_PMUXPROBE-END-5f0c9a3e\x1b\\"

LOGGED_SIGNALS = ("HUP", "WINCH", "QUIT", "INT", "TERM", "CONT", "USR1")


def write_all(fd, data):
    view = memoryview(data)
    while view:
        n = os.write(fd, view)
        view = view[n:]


def append(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        write_all(fd, data)
    finally:
        os.close(fd)


def atomic_write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.rename(tmp, path)


def winsize():
    packed = fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8)
    rows, cols, _, _ = struct.unpack("HHHH", packed)
    return rows, cols


def read_input(timeout=None):
    """Read from fd 0; returns b'' on EOF/EIO, None on timeout."""
    try:
        r, _, _ = select.select([0], [], [], timeout)
    except InterruptedError:  # pragma: no cover - PEP 475 retries select
        return None
    if not r:
        return None
    try:
        return os.read(0, 65536)
    except OSError:
        return b""


def idle_forever():
    while True:
        data = read_input(1.0)
        if data == b"":
            signal.pause()


def winch_marker(text):
    return b"\x1b[H\x1b[2J[winch:" + text.encode() + b"]\r\n"


def main(argv):
    ready = None
    winch_mark = None
    while argv[:1] in (["--ready"], ["--winch-mark"]) and len(argv) >= 2:
        if argv[0] == "--ready":
            ready = argv[1]
        else:
            winch_mark = argv[1]
        argv = argv[2:]
    if not argv:
        sys.stderr.write(__doc__)
        return 2
    mode, args = argv[0], argv[1:]

    if os.isatty(0):
        tty.setraw(0)

    if winch_mark is not None:
        marker = winch_marker(winch_mark)
        signal.signal(signal.SIGWINCH, lambda *_: write_all(1, marker))

    wakeup_r = None
    if mode == "inlog":
        (log_path,) = args
    elif mode == "emit":
        corpus_path = args[0]
        on_winch = "winch" in args[1:]
        with open(corpus_path, "rb") as f:
            payload = START_MARKER + f.read() + END_MARKER
        if on_winch:
            wakeup_r, wakeup_w = os.pipe()
            os.set_blocking(wakeup_w, False)
            signal.signal(signal.SIGWINCH, lambda *_: None)
            signal.set_wakeup_fd(wakeup_w)
    elif mode == "winsize":
        (log_path,) = args
        r, c = winsize()
        append(log_path, b"start %d %d\n" % (r, c))

        def on_winch(*_):
            rr, cc = winsize()
            append(log_path, b"%d %d\n" % (rr, cc))

        signal.signal(signal.SIGWINCH, on_winch)
    elif mode == "signals":
        (log_path,) = args
        for name in LOGGED_SIGNALS:
            signo = getattr(signal, "SIG" + name)
            signal.signal(
                signo,
                lambda s, _f: append(log_path, signal.Signals(s).name[3:].encode() + b"\n"),
            )
    elif mode == "env":
        (env_path,) = args
        atomic_write(env_path, json.dumps(dict(os.environ), sort_keys=True).encode())
    elif mode == "exit":
        code = args[0]
        delay = float(args[1]) if len(args) > 1 else None
    elif mode == "draw":
        opts = dict(a.split("=", 1) for a in args[1:])
        with open(args[0], "rb") as f:
            write_all(1, f.read())
        handlers = {}
        if "winch" in opts or "wlog" in opts:
            handlers[signal.SIGWINCH] = ("winch", opts.get("winch"), opts.get("wlog"))
        if "usr1" in opts:
            handlers[signal.SIGUSR1] = ("usr1", opts["usr1"], None)
        for signo, (_kind, out_file, wlog) in handlers.items():
            out = None
            if out_file:
                with open(out_file, "rb") as f:
                    out = f.read()

            def handler(s, _f, out=out, out_file=out_file, wlog=wlog):
                if s == signal.SIGWINCH and wlog:
                    append(wlog, b"%d %d\n" % winsize())
                if out is not None:
                    write_all(1, out)
                    if s == signal.SIGUSR1:
                        append(out_file + ".done", b"1\n")
            signal.signal(signo, handler)
    else:
        sys.stderr.write("probe: unknown mode %r\n" % mode)
        return 2

    if ready:
        atomic_write(ready, b"%d\n" % os.getpid())

    if mode == "inlog":
        tail = b""
        while True:
            data = read_input()
            if data is None:
                continue
            if data == b"":
                return 0
            append(log_path, data)
            window = tail + data
            if TERMINATOR in window:
                return 0
            tail = window[-(len(TERMINATOR) - 1):]
    elif mode == "emit":
        fds = [0] + ([wakeup_r] if wakeup_r is not None else [])
        stdin_open = True
        while True:
            try:
                r, _, _ = select.select(fds if stdin_open else fds[1:], [], [])
            except InterruptedError:  # pragma: no cover
                continue
            fire = False
            if wakeup_r in r:
                os.read(wakeup_r, 1024)
                fire = True
            if 0 in r:
                try:
                    data = os.read(0, 65536)
                except OSError:
                    data = b""
                if not data:
                    stdin_open = False
                    if wakeup_r is None:
                        signal.pause()
                elif START_BYTE in data:
                    fire = True
            if fire:
                write_all(1, payload)
    elif mode == "draw":
        if "exit" in opts:
            time.sleep(0.3)  # let the daemon drain the output first
            os._exit(int(opts["exit"]))
        log_path = opts.get("inlog")
        while True:
            data = read_input()
            if data == b"":
                signal.pause()
            elif data and log_path:
                append(log_path, data)
    elif mode == "exit":
        read_input(delay)
        if code.startswith("sig"):
            signo = int(code[3:])
            signal.signal(signo, signal.SIG_DFL)
            signal.pthread_sigmask(signal.SIG_UNBLOCK, [signo])
            os.kill(os.getpid(), signo)
            signal.pause()
        os._exit(int(code))
    else:
        idle_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
