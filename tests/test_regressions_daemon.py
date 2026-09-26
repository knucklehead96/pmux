"""Regression tests for daemon / screen bugs.

- libvterm reflow crash: a wrapped line longer than the screen, then an
  attach at a different size (reflow is disabled).
- a double-width character on a 1-column screen (libvterm is kept >= 2x2).
- huge client sizes are clamped (500 rows, 1000 columns).
- the cursor saved with the primary screen when reattaching to an
  alternate-screen app at a different size; libvterm's primary screen after
  the app leaves the alternate screen; DECRC after a shrink.
- RIS inside the alternate screen.
- SIGCHLD inherited as ignored by the daemon.
- signal dispositions / mask and the client's umask in new processes.
- indirect attach loops (X attaches Y, Y attaches X).
- modifyOtherKeys restore.
- daemon process name, ~/.pmux/daemon.log, queue and CSI buffer bounds,
  bounded foreground command.
"""
import os
import re
import select
import shutil
import signal
import socket
import stat
import struct
import subprocess
import termios
import time

import pexpect

from helpers import (PMUX_BIN, PROBE, PYTHON, TIMEOUT, PmuxTestCase, pid_alive, read_file,
                     read_until, wait_until)
from test_restore import WINCH_MARK, RestoreCase, numbered, pane_state
from tui import TuiCase, dump

# Message types (src/common/protocol.hpp).
LIST, LIST_REPLY, ATTACH, INPUT, OK = 1, 2, 7, 9, 15

SIG_BITS = {name: 1 << (getattr(signal, name) - 1)
            for name in ("SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGUSR1", "SIGCHLD", "SIGPIPE")}


def proc_status(pid):
    """{field: value} of /proc/<pid>/status."""
    out = {}
    for line in read_file("/proc/%d/status" % pid).decode().splitlines():
        k, _, v = line.partition(":")
        out[k] = v.strip()
    return out


def cpu_ticks(pid):
    fields = read_file("/proc/%d/stat" % pid).decode().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def is_raw(pid):
    """Whether the terminal on fd 0 of `pid` is in raw mode (the pmux client attached)."""
    try:
        fd = os.open("/proc/%d/fd/0" % pid, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        lflag = termios.tcgetattr(fd)[3]
    except termios.error:
        return False
    finally:
        os.close(fd)
    return not (lflag & (termios.ICANON | termios.ECHO))


# ---------------------------------------------------------------------------
# raw protocol client


def pstr(b):
    return struct.pack("<I", len(b)) + b


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        vals = struct.unpack_from("<" + fmt, self.data, self.pos)
        self.pos += struct.calcsize("<" + fmt)
        return vals[0]

    def str(self):
        n = self.take("I")
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s

    def strs(self):
        return [self.str() for _ in range(self.take("I"))]


class Conn:
    """A daemon connection speaking the frame protocol directly."""

    def __init__(self, px, timeout=TIMEOUT):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(px.sock)
        self.buf = b""

    def close(self):
        self.sock.close()

    def send(self, typ, payload=b""):
        self.sock.sendall(struct.pack("<IB", len(payload) + 1, typ) + payload)

    def recv(self):
        while True:
            if len(self.buf) >= 5:
                n = struct.unpack_from("<I", self.buf)[0]
                if len(self.buf) >= 4 + n:
                    typ, payload = self.buf[4], self.buf[5:4 + n]
                    self.buf = self.buf[4 + n:]
                    return typ, payload
            data = self.sock.recv(1 << 20)
            if not data:
                raise EOFError("daemon closed the connection")
            self.buf += data

    def request(self, typ, payload=b""):
        self.send(typ, payload)
        return self.recv()

    def list(self):
        typ, payload = self.request(LIST)
        assert typ == LIST_REPLY, typ
        r = Reader(payload)
        out = []
        for _ in range(r.take("I")):
            p = {"id": r.take("I"), "name": r.str().decode(), "dir": r.str(), "argv": r.strs(),
                 "pid": r.take("i")}
            # created_ms, exited, wait_status, created, bell (no idle / last-output fields)
            r.take("Q"), r.take("B"), r.take("i"), r.take("Q"), r.take("B")
            p["fg_command"] = r.str()
            out.append(p)
        return out


# ===========================================================================
# 1-3: libvterm crashes and huge sizes


class ScreenCrashes(PmuxTestCase):
    SIZES = [(30, 100), (10, 40), (50, 200), (5, 7), (100, 2), (1, 1), (24, 80), (2, 300)]

    def assertDaemonFine(self, pid, other="other"):
        self.assertTrue(pid_alive(pid), "daemon died")
        self.assertEqual(self.px.daemon_pid(), pid)
        e = self.px.entry(other)
        self.assertIsNotNone(e, "session %s lost" % other)
        self.assertEqual(e["state"], "running")

    def other_session(self):
        log = self.px.path("other.in")
        self.px.create_probe("other", "inlog", log)
        return log

    def check_other_works(self, log):
        c = self.px.attach("other")
        c.send(b"still-alive")
        wait_until(lambda: read_file(log) == b"still-alive", msg=lambda: read_file(log))
        self.detach(c, "other")

    def attach_detach(self, name, rows, cols):
        c = self.px.spawn_attach(["-a", name], rows, cols)
        self.px.wait_raw(c)
        time.sleep(0.1)
        self.detach(c, name)

    def test_long_wrapped_line_then_resize(self):
        log = self.other_session()
        p = self.px.run("-n", "long", "-d", "--", "sh", "-c",
                        "printf '%5000s' '' | tr ' ' x; printf '\\n%5000s' '' | tr ' ' y; exec sleep 600")
        self.assertEqual(p.returncode, 0, p.stderr)
        pid = self.px.daemon_pid()
        wait_until(lambda: self.px.entry("long") and self.px.entry("long")["state"] == "running")
        time.sleep(0.3)
        for rows, cols in self.SIZES:
            self.attach_detach("long", rows, cols)
            self.assertDaemonFine(pid)
        self.check_other_works(log)

    def test_long_line_while_attached_elsewhere(self):
        # The same, with the output arriving while a client is attached and resizing.
        log = self.other_session()
        self.px.create_probe("long", "emit", self.corpus(b"z" * 5000))
        pid = self.px.daemon_pid()
        c = self.px.attach("long", 24, 80)
        c.send(b"G")
        read_until(c, b"z" * 100)
        c.setwinsize(7, 13)
        c.setwinsize(60, 3)
        c.setwinsize(24, 80)
        time.sleep(0.3)
        self.detach(c, "long")
        self.assertDaemonFine(pid)
        self.attach_detach("long", 40, 120)
        self.assertDaemonFine(pid)
        self.check_other_works(log)

    def corpus(self, data):
        path = self.px.path("corpus.%d" % len(data))
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_wide_char_on_one_column(self):
        log = self.other_session()
        wide = "中".encode()
        script, winch = self.corpus(b"a"), self.px.path("wide.winch")
        with open(winch, "wb") as f:
            f.write(wide + b"\r\n" + wide * 3 + b"x" + wide + b"\r\n")
        self.px.create_probe("wide", "draw", script, "winch=" + winch)
        pid = self.px.daemon_pid()
        for rows, cols in ((24, 1), (1, 1), (1, 80), (24, 2)):
            c = self.px.spawn_attach(["-a", "wide"], rows, cols)
            self.px.wait_raw(c)
            read_until(c, wide)
            time.sleep(0.2)
            self.detach(c, "wide")
            self.assertDaemonFine(pid)
        self.check_other_works(log)

    def test_scroll_region_below_new_size(self):
        # libvterm does not clamp the scroll region's top on resize: scrolling a region that
        # starts below the new last row wrote out of bounds.
        log = self.other_session()
        script, winch = self.corpus(b"\x1b[10;22r\x1b[22Hregion"), self.px.path("region.winch")
        with open(winch, "wb") as f:
            f.write(b"\x1b[S\x1b[T" + b"line\n" * 30 + b"end")
        self.px.create_probe("region", "draw", script, "winch=" + winch)
        pid = self.px.daemon_pid()
        for rows, cols in ((5, 40), (1, 1), (24, 80)):
            c = self.px.spawn_attach(["-a", "region"], rows, cols)
            self.px.wait_raw(c)
            read_until(c, b"end")
            time.sleep(0.2)
            self.detach(c, "region")
            self.assertDaemonFine(pid)
        self.check_other_works(log)

    def test_created_on_one_column(self):
        log = self.other_session()
        pid = self.px.daemon_pid()
        c = self.px.spawn_attach(["-n", "narrow", "-d", "--", "sh", "-c",
                                  "printf '中中\\n中'; exec sleep 600"], 24, 1)
        c.expect(pexpect.EOF)
        wait_until(lambda: self.px.entry("narrow"))
        time.sleep(0.3)
        self.assertDaemonFine(pid, "narrow")
        self.attach_detach("narrow", 1, 1)
        self.assertDaemonFine(pid)
        self.check_other_works(log)

    def test_huge_sizes_clamped(self):
        wlog = self.px.path("ws.log")
        c = self.px.spawn_attach(["-n", "huge", "-d", "--",
                                  *self.px.probe_cmd("winsize", wlog, ready=self.px.path("huge.ready"))],
                                 60000, 60000)
        c.expect(pexpect.EOF)
        self.px.wait_ready(self.px.path("huge.ready"))
        self.assertEqual(read_file(wlog).splitlines()[0], b"start 500 1000")
        pid = self.px.daemon_pid()
        c = self.px.spawn_attach(["-a", "huge"], 65000, 65000)
        self.px.wait_raw(c)
        wait_until(lambda: len(read_file(wlog).splitlines()) >= 2, msg=lambda: read_file(wlog))
        c.setwinsize(40000, 3)
        wait_until(lambda: b"500 3" in read_file(wlog).splitlines(), msg=lambda: read_file(wlog))
        self.detach(c, "huge")
        self.assertEqual(set(read_file(wlog).splitlines()[1:]), {b"500 1000", b"500 3"})
        self.assertTrue(pid_alive(pid))
        rss_kb = int(proc_status(pid)["VmHWM"].split()[0])
        self.assertLess(rss_kb, 200 * 1024, "daemon peak RSS %d kB" % rss_kb)


# ===========================================================================
# 4, 5: alternate screen across resizes, DECRC, RIS


class AltScreenResize(RestoreCase):
    """A 100x30 process in the alternate screen, reattached at another size:
    when the app leaves the alternate screen and prints, the pmux pane must
    look like a plain tmux pane (resized the same way) running the app."""

    HISTORY = (numbered(b"main %02d", 1, 40)
               + b"\x1b[?1049h\x1b[H\x1b[2J\x1b[2;3H\x1b[1mALT-CONTENT\x1b[0m\x1b[6;7H")
    SHORT = (b"\x1b[H\x1b[2J" + numbered(b"short %02d", 1, 12) + b"prompt$ "
             + b"\x1b[?1049h\x1b[H\x1b[2JALT-CONTENT\x1b[3;3H")
    LEAVE = b"\x1b[?1049lafter-alt 1\r\nafter-alt 2\r\n$ "

    def create_sized(self, name, script, rows, cols):
        args = self.draw_args(name, script, usr1=self.LEAVE)
        ready = self.px.path(name + ".ready")
        c = self.px.spawn_attach(["-n", name, "-d", "--",
                                  *self.px.probe_cmd("draw", *args, ready=ready)], rows, cols, cwd=self.api)
        c.expect(pexpect.EOF)
        return self.px.wait_ready(ready, name)

    def leave_alt(self, pid, name, count=1):
        os.kill(pid, signal.SIGUSR1)
        done = self.px.path(name + ".usr1.%d.done" % self._usr1_index(name))
        wait_until(lambda: read_file(done).count(b"\n") >= count, msg="probe did not handle SIGUSR1")

    def settled(self, t):
        prev = {}

        def check():
            cur = (pane_state(t), t.history())
            ok = cur == prev.get("s")
            prev["s"] = cur
            return ok
        wait_until(check, interval=0.15, msg="pane never settled")
        return prev["s"]

    def reference(self, script, rows, cols):
        """tmux pane at 100x30 running the script, resized to rows x cols,
        then the app leaves the alternate screen."""
        name = "ref%d" % (self._files + 1)
        args = self.draw_args(name, script, usr1=self.LEAVE)
        ready = self.px.path(name + ".ready")
        t = self.pane([PYTHON, PROBE, "--ready", ready, "draw", *args], 30, 100)
        pid = self.px.wait_ready(ready, "reference probe")
        t.wait_for(lambda l: any("ALT-CONTENT" in x for x in l), msg="reference alt screen")
        t.resize(rows, cols)
        wait_until(lambda: t.fmt("pane_height", "pane_width") == {"pane_height": str(rows),
                                                                    "pane_width": str(cols)})
        time.sleep(0.2)
        self.leave_alt(pid, name)
        t.wait_for(lambda l: any("after-alt 2" in x for x in l), msg="reference left alt screen")
        return self.settled(t)

    def check(self, script, rows, cols):
        (ref, ref_hist) = self.reference(script, rows, cols)
        self.assertEqual(ref["flags"]["alternate_on"], "0", "reference sanity")
        pid = self.create_sized("app", script, 30, 100)
        t = self.attach_pane("app", rows, cols)
        t.wait_for(lambda l: any("ALT-CONTENT" in x for x in l), msg="pmux alt screen")
        self.leave_alt(pid, "app")
        t.wait_for(lambda l: any("after-alt 2" in x for x in l), msg="pmux left alt screen")
        self.assertPaneMatches(t, ref, "after leaving the alternate screen at %dx%d" % (cols, rows))
        _, hist = self.settled(t)
        self.assertEqual(hist, ref_hist, "history + screen\n%s\n%s" % (dump(ref_hist, "expected"),
                                                                        dump(hist, "actual")))
        # libvterm's own primary screen now matches too: a reattach shows the same.
        self.detach_pane(t, "app")
        t = self.attach_pane("app", rows, cols)
        self.assertPaneMatches(t, ref, "reattach after leaving the alternate screen")
        _, hist = self.settled(t)
        self.assertEqual(hist, ref_hist, "history after reattach\n%s\n%s"
                         % (dump(ref_hist, "expected"), dump(hist, "actual")))
        self.detach_pane(t, "app")

    def test_history_bigger(self):
        self.check(self.HISTORY, 40, 120)

    def test_history_smaller(self):
        self.check(self.HISTORY, 20, 80)

    def test_short_bigger(self):
        self.check(self.SHORT, 40, 120)

    def test_short_smaller(self):
        self.check(self.SHORT, 20, 80)

    def test_decrc_after_shrink(self):
        # libvterm does not clamp the saved cursor on resize: restoring it after a shrink used
        # to put the cursor off the screen and the next character was written out of bounds.
        script = b"\x1b[H\x1b[2J" + numbered(b"line %02d", 1, 29) + b"\x1b7"
        restore = b"\x1b8" + b"x" * 300 + b"\r\nrestored\x1b[?1049h\x1b[?1049l" + b"y" * 50
        args = self.draw_args("rc", script, usr1=restore)
        ready = self.px.path("rc.ready")
        c = self.px.spawn_attach(["-n", "rc", "-d", "--", *self.px.probe_cmd("draw", *args, ready=ready)],
                                 30, 100, cwd=self.api)
        c.expect(pexpect.EOF)
        pid = self.px.wait_ready(ready)
        daemon = self.px.daemon_pid()
        t = self.attach_pane("rc", 10, 40)
        self.leave_alt(pid, "rc")
        t.wait_for(lambda l: any("restored" in x for x in l), msg="restored text")
        self.assertTrue(pid_alive(daemon), "daemon died")
        self.detach_pane(t, "rc")
        t = self.attach_pane("rc", 10, 40)
        t.wait_for(lambda l: any("restored" in x for x in l), msg="restored text after reattach")
        self.assertTrue(pid_alive(daemon), "daemon died")


class Ris(RestoreCase):
    SCRIPT = (numbered(b"before-ris %d", 1, 3) + b"\x1b[?1049h\x1b[?2004h\x1b[?1hALT-TEXT"
              + b"\x1bc" + numbered(b"after-ris %d", 1, 3))

    def test_ris_leaves_alt_screen(self):
        self.draw("ris", self.SCRIPT, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("ris")
        self.detach_raw(c, "ris")
        self.assertNotIn(b"\x1b[?1049h", snap, "snapshot still in the alternate screen")
        self.assertNotIn(b"ALT-TEXT", snap)
        self.assertNotIn(b"\x1b[?2004h", snap, "bracketed paste survived RIS")
        self.assertNotIn(b"\x1b[?1h", snap, "DECCKM survived RIS")
        self.assertIn(b"after-ris 3", snap)

    def test_ris_in_tmux(self):
        # (tmux 3.4 itself stays in the alternate screen on RIS, so it is no oracle here.)
        self.draw("ris", self.SCRIPT)
        t = self.attach_pane("ris")
        lines = t.wait_for(lambda l: l[:3] == ["after-ris %d" % i for i in (1, 2, 3)], msg="screen after RIS")
        self.assertFalse(any("ALT-TEXT" in l for l in lines), dump(lines))
        self.assertEqual(t.fmt("alternate_on", "keypad_cursor_flag"),
                         {"alternate_on": "0", "keypad_cursor_flag": "0"})
        self.assertEqual(t.cursor(), (0, 3))


# ===========================================================================
# 6, 7: signals and umask of new processes


class ChildState(TuiCase):
    CONFIG = None
    IGNORE = "SIGTSTP SIGTTIN SIGTTOU SIGUSR1 SIGCHLD"

    def setUp(self):
        # Like TuiCase, but the daemon is started from a parent that ignores signals.
        PmuxTestCase.setUp(self)
        self.api = self.mkdir("work/api")

    def start_daemon_ignoring(self, names):
        code = ("import os, signal, sys\n"
                "for n in sys.argv[1].split(): signal.signal(getattr(signal, n), signal.SIG_IGN)\n"
                "os.execv(sys.argv[2], sys.argv[2:])\n")
        p = subprocess.run([PYTHON, "-c", code, names, PMUX_BIN, "-l"], cwd=self.px.work,
                           env=self.px.make_env(self.px.work), stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=TIMEOUT)
        self.assertEqual(p.returncode, 0)
        pid = self.px.daemon_pid()
        ign = int(proc_status(pid)["SigIgn"], 16)
        for n in names.split():
            if n != "SIGCHLD":  # the daemon resets SIGCHLD itself
                self.assertTrue(ign & SIG_BITS[n], "sanity: daemon should inherit %s ignored" % n)
        return pid

    def test_exit_status_with_inherited_sigchld_ignore(self):
        self.start_daemon_ignoring("SIGCHLD")
        self.new("three", "sh", "-c", "exit 3")
        self.px.wait_state("three", lambda s: s == "exited:3")
        self.new("sig", "sh", "-c", "kill -TERM $$")
        self.px.wait_state("sig", lambda s: s == "signaled:15")

    def test_signal_dispositions_and_mask_reset(self):
        self.start_daemon_ignoring(self.IGNORE)
        out = self.px.path("status")
        self.new("st", "sh", "-c", 'cat /proc/self/status >"$0.tmp"; mv "$0.tmp" "$0"; exec sleep 600', out)
        wait_until(lambda: os.path.exists(out))
        st = {}
        for line in read_file(out).decode().splitlines():
            k, _, v = line.partition(":")
            st[k] = v.strip()
        self.assertEqual(int(st["SigIgn"], 16), 0, "ignored signals leaked into the process: %s" % st["SigIgn"])
        self.assertEqual(int(st["SigBlk"], 16), 0, "blocked signals leaked into the process: %s" % st["SigBlk"])

    def run_umask(self, mask, name):
        out = self.px.path(name + ".umask")
        full_env = self.px.make_env(self.api)
        p = subprocess.run([PMUX_BIN, "-n", name, "-d", "--", "sh", "-c",
                            'umask >"$0.tmp"; mv "$0.tmp" "$0"; exec sleep 600', out],
                           cwd=self.api, env=full_env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=TIMEOUT,
                           preexec_fn=lambda: os.umask(mask))
        self.assertEqual(p.returncode, 0, p.stderr)
        wait_until(lambda: os.path.exists(out))
        return read_file(out).strip()

    def test_client_umask_applied(self):
        p = self.px.run("-l")  # daemon started with the test's umask
        self.assertEqual(p.returncode, 0)
        self.assertEqual(self.run_umask(0o002, "u002"), b"0002")
        self.assertEqual(self.run_umask(0o077, "u077"), b"0077")
        self.assertEqual(self.run_umask(0o027, "u027"), b"0027")


# ===========================================================================
# 8: attach loops


class AttachLoop(TuiCase):
    def chain(self, names):
        """names[0] runs a script that waits for a go file and then runs
        `pmux -a names[-1]`; names[i] (i > 0) runs `pmux -a names[i-1]`."""
        go, err, rc = self.px.path("go"), self.px.path("loop.err"), self.px.path("loop.rc")
        self.new(names[0], "sh", "-c",
                 'while [ ! -e "$1" ]; do sleep 0.05; done; "$0" -a "$2" 2>"$3"; echo $? >"$4"; '
                 "exec sleep 600", PMUX_BIN, go, names[-1], err, rc)
        for prev, name in zip(names, names[1:]):
            self.new(name, PMUX_BIN, "-a", prev)
            pid = int(self.px.entry(name)["pid"])
            wait_until(lambda: is_raw(pid), msg="%s never attached to %s" % (name, prev))
        daemon = self.px.daemon_pid()
        with open(go, "w"):
            pass
        wait_until(lambda: read_file(rc).strip(), msg="nested pmux -a never returned")
        self.assertEqual(read_file(rc).strip(), b"1")
        self.assertEqual(read_file(err),
                         b"pmux: cannot attach %s: would create an attach loop\n" % names[-1].encode())
        before = cpu_ticks(daemon)
        time.sleep(1.0)
        used = cpu_ticks(daemon) - before
        self.assertLess(used, 20, "daemon used %d ticks in 1 s" % used)
        for n in names:
            self.assertEqual(self.px.entry(n)["state"], "running", n)

    def test_two_process_loop_refused(self):
        self.chain(["y", "x"])

    def test_three_process_loop_refused(self):
        self.chain(["z", "y", "x"])

    def test_non_loop_chain_allowed(self):
        # x attaches y; a client inside z may still attach x.
        self.marker("y")
        self.new("x", PMUX_BIN, "-a", "y")
        pid = int(self.px.entry("x")["pid"])
        wait_until(lambda: is_raw(pid), msg="x never attached to y")
        self.new("z", PMUX_BIN, "-a", "x")
        pid = int(self.px.entry("z")["pid"])
        wait_until(lambda: is_raw(pid), msg="z never attached to x")


# ===========================================================================
# 9: modifyOtherKeys


class ModifyOtherKeys(RestoreCase):
    def snapshot(self, name, script):
        self.draw(name, script, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw(name)
        out = self.detach_raw(c, name)
        return snap, out

    def test_restored(self):
        snap, _ = self.snapshot("mok", b"text\x1b[>4;2m")
        self.assertIn(b"\x1b[>4;2m", snap)
        snap, _ = self.snapshot("mok1", b"text\x1b[>4;1m")
        self.assertIn(b"\x1b[>4;1m", snap)

    def test_reset_not_restored(self):
        for i, script in enumerate((b"\x1b[>4;2m\x1b[>4m", b"\x1b[>4;2m\x1b[>4;0m", b"\x1b[>4;2m\x1b[>4n",
                                    b"\x1b[>4;2m\x1bc", b"")):
            snap, _ = self.snapshot("mok%d" % i, b"text" + script)
            self.assertNotIn(b"\x1b[>4;", snap, "script %r" % script)

    def test_restored_once_after_history(self):
        snap, _ = self.snapshot("mokh", b"\x1b[>4;2m" + numbered(b"l %d", 1, 30))
        self.assertEqual(snap.count(b"\x1b[>4;2m"), 1)


# ===========================================================================
# 10: daemon cleanups


class DaemonHousekeeping(PmuxTestCase):
    LOG_LINE = re.compile(rb"^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d pmux\[\d+\]: (.*)$")

    def log_lines(self):
        data = read_file(os.path.join(self.px.home, ".pmux", "daemon.log"))
        out = []
        for line in data.splitlines():
            m = self.LOG_LINE.match(line)
            self.assertIsNotNone(m, "malformed log line %r" % line)
            out.append(m.group(1))
        return out

    def test_process_name(self):
        self.assertEqual(self.px.run("-l").returncode, 0)
        pid = self.px.daemon_pid()
        self.assertEqual(read_file("/proc/%d/comm" % pid).strip(), b"pmux")
        if shutil.which("pgrep"):
            p = subprocess.run(["pgrep", "-x", "pmux"], capture_output=True)
            self.assertIn(str(pid).encode(), p.stdout.split())

    def test_startup_errors_logged(self):
        bad = os.path.join(self.px.root, "badrun")
        os.mkdir(bad, 0o700)
        with open(os.path.join(bad, "pmux"), "w"):
            pass  # a file where the socket directory should be
        p = self.px.run("--daemon", extra_env={"XDG_RUNTIME_DIR": bad})
        self.assertEqual(p.returncode, 1)
        lines = self.log_lines()
        self.assertEqual(len(lines), 1, lines)
        self.assertIn(b"badrun/pmux", lines[0])
        st = os.stat(os.path.join(self.px.home, ".pmux"))
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o700)
        # Second daemon: already running.
        self.assertEqual(self.px.run("-l").returncode, 0)
        p = self.px.run("--daemon")
        self.assertEqual(p.returncode, 1)
        self.assertEqual(self.log_lines()[1:], [b"daemon already running"])

    def test_input_queue_bounded(self):
        # Raw mode: the line discipline pushes back instead of discarding (canonical mode).
        self.assertEqual(self.px.run("-n", "sl", "-d", "--", "sh", "-c",
                                     "stty raw -echo; exec sleep 600").returncode, 0)
        time.sleep(0.3)
        pid = self.px.daemon_pid()
        conn = Conn(self.px)
        sid = [p for p in conn.list() if p["name"] == "sl"][0]["id"]
        typ, _ = conn.request(ATTACH, struct.pack("<IHH", sid, 24, 80) + pstr(b""))
        self.assertEqual(typ, OK)
        chunk = struct.pack("<IB", (1 << 20) + 1, INPUT) + b"k" * (1 << 20)
        try:
            for _ in range(100):
                conn.sock.sendall(chunk)
            conn.sock.settimeout(5)
            while conn.sock.recv(1 << 20):  # echoed output, then EOF once dropped
                pass
            dropped = True
        except (BrokenPipeError, ConnectionResetError):
            dropped = True
        except socket.timeout:
            dropped = False
        conn.close()
        self.assertTrue(dropped, "daemon kept a client whose input queue exceeded 64 MiB")
        self.assertTrue(pid_alive(pid))
        rss_kb = int(proc_status(pid)["VmRSS"].split()[0])
        self.assertLess(rss_kb, 150 * 1024, "daemon RSS %d kB" % rss_kb)
        self.assertEqual(self.px.entry("sl")["state"], "running")

    def test_output_queue_bounded(self):
        big = ["a" * 100000] * 4  # ~400 kB per LIST reply
        self.assertEqual(self.px.run("-n", "big", "-d", "--", "sh", "-c", "exec sleep 600", *big).returncode, 0)
        pid = self.px.daemon_pid()
        conn = Conn(self.px)
        requests = 300  # ~120 MB of replies
        try:
            conn.sock.sendall(struct.pack("<IB", 1, LIST) * requests)
        except (BrokenPipeError, ConnectionResetError):
            pass
        # Read nothing until the daemon hangs up: a reader racing the daemon's producer could keep
        # its queue under the limit on a slow machine.  POLLHUP is reported with data still unread.
        poller = select.poll()
        poller.register(conn.sock, 0)
        deadline = time.monotonic() + 6 * TIMEOUT
        hup = False
        while not hup and time.monotonic() < deadline:
            hup = any(ev & (select.POLLHUP | select.POLLERR) for _, ev in poller.poll(100))
        self.assertTrue(hup, "daemon kept a client that does not read")
        received, eof = 0, False
        conn.sock.settimeout(5)
        try:
            while True:
                data = conn.sock.recv(1 << 20)
                if not data:
                    eof = True
                    break
                received += len(data)
        except ConnectionResetError:
            eof = True
        except socket.timeout:
            pass
        conn.close()
        self.assertTrue(eof, "daemon kept a client that does not read (%d bytes received)" % received)
        self.assertLess(received, 100 << 20)
        self.assertTrue(pid_alive(pid))
        self.assertEqual(self.px.entry("big")["state"], "running")

    def test_runaway_csi_bounded(self):
        size = 48 << 20
        gen = ("import os, sys\n"
               "def w(b):\n"
               "    while b: b = b[os.write(1, b):]\n"
               "w(b'before\\r\\n\\x1b[' + b' ' * %d + b'q' + b'after-runaway\\r\\n')\n"
               "w(b'\\x1b[' + b'1' * %d + b'mafter-digits\\r\\n')\n"
               "open(sys.argv[1], 'w').close()\n"
               "os.execvp('sleep', ['sleep', '600'])\n" % (size, size))
        done = self.px.path("gen.done")
        self.assertEqual(self.px.run("-n", "csi", "-d", "--", PYTHON, "-c", gen, done).returncode, 0)
        pid = self.px.daemon_pid()
        wait_until(lambda: os.path.exists(done), timeout=60)
        time.sleep(0.5)
        c = self.px.attach("csi")
        read_until(c, b"after-digits")
        self.detach(c, "csi")
        hwm_kb = int(proc_status(pid)["VmHWM"].split()[0])
        self.assertLess(hwm_kb, 40 * 1024, "daemon peak RSS %d kB: CSI buffer not bounded" % hwm_kb)

    def test_fg_command_bounded(self):
        args = ["b" * 100000] * 3  # each argument is limited to 128 KiB
        self.assertEqual(self.px.run("-n", "longcmd", "-d", "--", PYTHON, "-c",
                                     "import time; time.sleep(600)", *args).returncode, 0)
        conn = Conn(self.px)
        fg = wait_until(lambda: [p for p in conn.list() if p["name"] == "longcmd"][0]["fg_command"])
        conn.close()
        self.assertTrue(fg.startswith(PYTHON.encode() + b" -c import time"), fg[:80])
        self.assertLessEqual(len(fg), 4096)


if __name__ == "__main__":
    import unittest
    unittest.main()
