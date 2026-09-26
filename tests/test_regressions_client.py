"""Regression tests for client / TUI bugs: detach under an output
flood, SIGPIPE, socket directory and peer checks, SIGWINCH in helper threads,
inherited fds, signals while attached, detach key forms, detach resets,
Home/End variants, terminal replies leaking into the app, the capture-pane
style harness and connect errors."""
import os
import shutil
import signal
import socket
import stat
import struct
import subprocess
import time
import unittest

import pexpect

from helpers import (CTRL_BACKSLASH, CTRL_LEFT, CTRL_SHIFT_LEFT, DETACH, PMUX_BIN, TIMEOUT,
                     PmuxTestCase, expect_exit, read_file, read_until, wait_until)
from tui import FOOTER_LIST, TmuxTui, TuiCase, parse_ansi_line, winch_marker

# The exact reset sequence written on detach.
MODE_RESETS = (b"\x1b[<99u"
               b"\x1b[?1049l"
               b"\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
               b"\x1b[?1l\x1b>"
               b"\x1b[?2004l"
               b"\x1b[?1004l"
               b"\x1b[?25h"
               b"\x1b[?7h"
               b"\x1b[0m"
               b"\x1b[<99u"
               b"\x1b[>4m")

# FTXUI asks for the cursor shape (DECRQSS DECSCUSR) whenever the list resumes.
CURSOR_SHAPE_REPLY = b"\x1bP1$r2 q\x1b\\"


def children_of(pid):
    out = []
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            with open("/proc/%s/stat" % d) as f:
                if int(f.read().rsplit(")", 1)[1].split()[1]) == pid:
                    out.append(int(d))
        except (OSError, IndexError, ValueError):
            pass
    return out


class ClientCase(PmuxTestCase):
    CONFIG = None   # ~/.pmux/config text, written before the daemon starts

    def setUp(self):
        super().setUp()
        if self.CONFIG is not None:
            self.px.write_config(self.CONFIG)
        p = self.px.run("-l")   # daemon started outside any pty
        self.assertEqual(p.returncode, 0, "daemon start via -l failed: %r" % p.stderr)

    def inlog(self, name="app"):
        log = self.px.path(name + ".in")
        self.px.create_probe(name, "inlog", log)
        return log

    def sync(self, child, log, expected_prefix, token=b"<sync>"):
        child.send(token)
        self.wait_log(log, expected_prefix + token)
        return expected_prefix + token


# ---------------------------------------------------------------------------
# 1. output flood + slow terminal must not starve the detach key


class FloodDetach(ClientCase):
    def test_detach_while_output_floods_a_slow_terminal(self):
        p = self.px.run("-n", "flood", "-d", "--", "yes", "flood-line")
        self.assertEqual(p.returncode, 0, p.stderr)
        c = self.px.attach("flood")
        c.delaybeforesend = None

        def slow_read():
            try:
                data = c.read_nonblocking(2048, timeout=0.02)
            except pexpect.TIMEOUT:
                data = b""
            time.sleep(0.02)
            return data

        # Let the backlog build up in every buffer on the way.
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            slow_read()
        c.send(DETACH)
        t0 = time.monotonic()
        out = b""
        while b"[detached from flood]" not in out:
            if time.monotonic() - t0 > 10:
                self.fail("Ctrl+Left ignored for 10 s while output floods; tail %r" % out[-200:])
            try:
                out += slow_read()
            except pexpect.EOF:
                break
        elapsed = time.monotonic() - t0
        self.assertIn(b"[detached from flood]", out)
        self.assertLess(elapsed, 2.0, "detach took %.2f s under an output flood" % elapsed)
        self.assertEqual(self.px.entry("flood")["state"], "running")


# ---------------------------------------------------------------------------
# 2. daemon killed under the TUI: clean exit, terminal restored


class DaemonGone(TuiCase):
    def test_tui_exits_cleanly_when_daemon_dies(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        self.assertEqual(t.modes(), ("1", "1"), "TUI uses the alt screen and the mouse")
        pid = self.px.daemon_pid()
        os.kill(pid, signal.SIGKILL)
        t.wait_dead()
        self.assertEqual(t.dead_status(), 1, "exit status (141 = killed by SIGPIPE)\n" + t.describe())
        self.assertIn("pmux: lost connection to daemon", t.history(), t.describe())
        self.assertEqual(t.modes(), ("0", "0"), "alt screen / mouse reporting left on")

    def test_tui_exits_cleanly_on_stop(self):
        self.new_exited("done", "exit 0")
        t = self.start_list("done")
        self.assertEqual(t.modes(), ("1", "1"))
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 0, p.stderr)
        t.wait_dead()
        self.assertEqual(t.dead_status(), 1, t.describe())
        self.assertIn("pmux: lost connection to daemon", t.history(), t.describe())
        self.assertEqual(t.modes(), ("0", "0"), "alt screen / mouse reporting left on")

    def test_attached_client_and_tui_exit_cleanly_on_force_stop(self):
        script = self.px.path("alt.script")
        with open(script, "wb") as f:
            f.write(b"\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[HALT-SCREEN")
        self.px.create_probe("alt", "draw", script, cwd=self.api)
        lst = self.start_list("alt")
        att = self.tui(args=("-a", "alt"))
        att.wait_for("ALT-SCREEN")
        self.assertEqual(att.modes(), ("1", "1"))
        p = self.px.run("--stop", "--force", timeout=15)
        self.assertEqual(p.returncode, 0, p.stderr)
        for t in (att, lst):
            t.wait_dead()
            self.assertEqual(t.modes(), ("0", "0"), "alt screen / mouse reporting left on\n" + t.describe())
        self.assertIn("[alt killed by signal 1]", att.history(), att.describe())
        self.assertEqual(att.dead_status(), 0, att.describe())
        self.assertEqual(lst.dead_status(), 1, lst.describe())
        self.assertIn("pmux: lost connection to daemon", lst.history(), lst.describe())


def dynamically_linked(path):
    """True if the ELF64 executable has a PT_INTERP header (LD_PRELOAD applies)."""
    with open(path, "rb") as f:
        data = f.read(1 << 16)
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    return any(struct.unpack_from("<I", data, phoff + i * phentsize)[0] == 3 for i in range(phnum))


# ---------------------------------------------------------------------------
# 3. socket directory and peer credential checks


class SocketSecurity(PmuxTestCase):
    def test_refuses_socket_dir_accessible_by_others(self):
        os.makedirs(self.px.sock_dir)
        os.chmod(self.px.sock_dir, 0o777)
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(self.px.sock)
        listener.listen(4)
        listener.settimeout(0.3)
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 1, "rc; stderr=%r" % p.stderr)
        self.assertIn(b"refusing to use it", p.stderr)
        self.assertIn(b"0777", p.stderr)
        with self.assertRaises(socket.timeout, msg="client connected to the untrusted socket"):
            listener.accept()
        self.assertIsNone(self.px.daemon_pid(), "no daemon may be spawned")
        self.assertEqual(stat.S_IMODE(os.stat(self.px.sock_dir).st_mode), 0o777)

    def test_refuses_daemon_owned_by_another_uid(self):
        # A cross-uid daemon needs root or newuidmap; instead an LD_PRELOAD
        # shim makes SO_PEERCRED report a different uid.
        if not dynamically_linked(PMUX_BIN):
            self.skipTest("static PMUX_BIN: the LD_PRELOAD shim cannot apply")
        cc = shutil.which("cc") or shutil.which("gcc")
        if not cc:
            self.skipTest("no C compiler for the SO_PEERCRED shim")
        src = self.px.path("shim.c")
        lib = self.px.path("shim.so")
        with open(src, "w") as f:
            f.write("#define _GNU_SOURCE\n#include <dlfcn.h>\n#include <sys/socket.h>\n"
                    "int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {\n"
                    "  int (*real)(int, int, int, void *, socklen_t *) =\n"
                    "      (int (*)(int, int, int, void *, socklen_t *))dlsym(RTLD_NEXT, \"getsockopt\");\n"
                    "  int r = real(fd, level, opt, val, len);\n"
                    "  if (r == 0 && level == SOL_SOCKET && opt == SO_PEERCRED)\n"
                    "    ((struct ucred *)val)->uid += 4242;\n"
                    "  return r;\n}\n")
        subprocess.run([cc, "-shared", "-fPIC", "-o", lib, src, "-ldl"], check=True)
        self.assertEqual(self.px.run("-l").returncode, 0)   # the real daemon
        p = self.px.run("-l", extra_env={"LD_PRELOAD": lib})
        self.assertEqual(p.returncode, 1, "rc; stderr=%r" % p.stderr)
        self.assertIn(b"pmux: refusing to talk to daemon owned by uid %d" % (os.getuid() + 4242), p.stderr)


# ---------------------------------------------------------------------------
# 4. resizes while attached from the TUI (helper threads block SIGWINCH)


class TuiResize(TuiCase):
    def test_resizes_during_tui_attach_reach_the_process(self):
        log = self.px.path("win.log")
        self.px.create_probe("win", "winsize", log, cwd=self.api)
        t = self.start_list("win")
        t.keys("Enter")
        wait_until(lambda: b"30 100\n" in read_file(log),
                   msg=lambda: "no attach resize; log=%r" % read_file(log))
        # One at a time: a SIGWINCH consumed by a helper thread is lost for
        # good, so every single resize must reach the process.
        for i in range(30):
            rows, cols = 20 + i % 9, 60 + i
            t.resize(rows, cols)
            want = b"%d %d" % (rows, cols)
            wait_until(lambda: read_file(log).splitlines()[-1] == want, 3.0,
                       msg=lambda: "resize %d (%r) never reached the process; log tail %r"
                       % (i, want, read_file(log).splitlines()[-5:]))


# ---------------------------------------------------------------------------
# 5. the spawned daemon must not inherit the client's fds


class InheritedFds(PmuxTestCase):
    def test_spawned_daemon_closes_inherited_fds(self):
        paths = [self.px.path("leak7"), self.px.path("leak8")]
        fds = []
        for want, path in zip((7, 8), paths):
            fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
            os.dup2(fd, want, inheritable=True)
            os.close(fd)
            fds.append(want)
        self.addCleanup(lambda: [os.close(fd) for fd in fds])
        env = self.px.make_env(self.px.work)
        p = subprocess.run([PMUX_BIN, "-n", "fdp", "-d", "--", "sleep", "1000"], cwd=self.px.work,
                           env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, pass_fds=fds, timeout=TIMEOUT)
        self.assertEqual(p.returncode, 0, p.stderr)
        daemon = self.px.daemon_pid()
        session = int(self.px.entry("fdp")["pid"])
        for pid in (daemon, session):
            links = []
            for fd in os.listdir("/proc/%d/fd" % pid):
                try:
                    links.append(os.readlink("/proc/%d/fd/%s" % (pid, fd)))
                except OSError:
                    pass
            for path in paths:
                self.assertNotIn(path, links, "pid %d inherited %s: %r" % (pid, path, links))


# ---------------------------------------------------------------------------
# 6. signals to an attached client restore the terminal


class SignalWhileAttached(ClientCase):
    def spawn_wrapped(self, name):
        """pmux -a NAME under sh, which afterwards prints pmux's exit status
        and `stty -a` of the same tty."""
        c = pexpect.spawn("/bin/sh", ["-c", '"$0" -a "$1"; echo "rc=$?"; stty -a', PMUX_BIN, name],
                          cwd=self.px.work, env=self.px.make_env(self.px.work),
                          dimensions=(24, 80), encoding=None, timeout=TIMEOUT)
        self.px.children.append(c)
        self.px.wait_raw(c)
        return c

    def check_signal(self, sig):
        self.inlog()
        c = self.spawn_wrapped("app")
        pids = wait_until(lambda: children_of(c.pid))
        os.kill(pids[0], sig)
        status, _sig, out = expect_exit(c, b"rc=%d" % (128 + sig))
        self.assertIn(MODE_RESETS, out, "detach resets missing")
        stty = out[out.index(b"rc="):].decode(errors="replace")
        self.assertRegex(stty, r"(^|\s)icanon(\s|$)", "termios not restored:\n" + stty)
        self.assertRegex(stty, r"(^|\s)echo(\s|$)", "termios not restored:\n" + stty)
        self.assertEqual(self.px.entry("app")["state"], "running", "the process must survive")

    def test_sigterm(self):
        self.check_signal(signal.SIGTERM)

    def test_sighup(self):
        self.check_signal(signal.SIGHUP)

    def test_sigint(self):
        self.check_signal(signal.SIGINT)


class TuiSignalWhileAttached(TuiCase):
    def test_sigterm_during_tui_attach(self):
        script = self.px.path("alt.script")
        with open(script, "wb") as f:
            f.write(b"\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[HALT-SCREEN")
        self.px.create_probe("alt", "draw", script, cwd=self.api)
        t = self.start_list("alt")
        t.keys("Enter")
        t.wait_for("ALT-SCREEN")
        self.assertEqual(t.modes(), ("1", "1"))
        pmux = wait_until(lambda: children_of(t.pane_pid()))
        os.kill(pmux[0], signal.SIGTERM)
        t.wait_dead()
        self.assertEqual(t.dead_status(), 128 + signal.SIGTERM, t.describe())
        self.assertEqual(t.modes(), ("0", "0"), "alt screen / mouse reporting left on")
        self.assertEqual(self.px.entry("alt")["state"], "running", "the process must survive")


# ---------------------------------------------------------------------------
# 7. / 8. detach key forms and the detach reset sequence


class DetachKeyForms(ClientCase):
    """Default detach_key (ctrl+left): kitty lock modifiers / events, split
    sequences, look-alikes.  Subclasses rerun the same checks for the
    other detach_key values."""
    KEY = CTRL_LEFT
    DETACHES = [b"\x1b[1;69D", b"\x1b[1;133:1D", b"\x1b[1;197D", b"\x1b[1;5:1D", b"\x1bOd"]
    SPLITS = [(b"\x1b[1;", b"5D"), (b"\x1b[1;6", b"9:1D"), (b"\x1b[1", b";13", b"3:1D")]
    SWALLOWED = b"\x1b[1;5:2D\x1b[1;69:3D\x1b[1;133:2D\x1b[1;197:3D"
    LOOKALIKES = (b"\x1b[D" b"\x1b[1;2D" b"\x1b[1;3D" b"\x1b[1;5C" b"\x1b[1;6D" b"\x1b[1;70D"
                  b"\x1b[1;7D" b"\x1b[1;13D" b"\x1b[1;69C" b"\x1b[1;5:1C" b"\x1b[5D" b"\x1bOD"
                  b"\x1bOc" b"\x1c" b"\x1b[92;5u" b"\x1b[92;69u" b"\x1b[27;5;92~" b"x")

    def detach_with(self, *parts, name="app"):
        log = self.inlog(name)
        c = self.px.attach(name)
        c.delaybeforesend = None
        seen = self.sync(c, log, b"", b"before")
        for i, part in enumerate(parts):
            if i:
                time.sleep(0.002)   # separate reads, inside the 20 ms hold-back
            c.send(part)
        status, sig, out = expect_exit(c, b"[detached from %s]" % name.encode())
        self.assertEqual((status, sig), (0, None))
        self.assertIn(MODE_RESETS, out)
        c = self.px.attach(name)
        self.sync(c, log, seen, b"after")   # no detach bytes reached the app
        self.detach(c, name, key=self.KEY)

    def test_plain_key(self):
        self.detach_with(self.KEY)

    def test_other_forms(self):
        for i, key in enumerate(self.DETACHES):
            with self.subTest(key=key):
                self.detach_with(key, name="form%d" % i)

    def test_split_sequences(self):
        for i, parts in enumerate(self.SPLITS):
            with self.subTest(parts=parts):
                self.detach_with(*parts, name="split%d" % i)

    def test_repeat_and_release_swallowed(self):
        log = self.inlog()
        c = self.px.attach("app")
        c.send(b"a" + self.SWALLOWED + b"b")
        self.wait_log(log, b"ab")
        self.assertTrue(c.isalive(), "repeat / release must not detach")
        self.sync(c, log, b"ab")
        self.detach(c, "app", key=self.KEY)

    def test_lookalikes_pass_verbatim(self):
        log = self.inlog()
        c = self.px.attach("app")
        c.send(self.LOOKALIKES)
        self.wait_log(log, self.LOOKALIKES)
        self.assertTrue(c.isalive(), "look-alikes must not detach")
        self.detach(c, "app", key=self.KEY)

    def test_detach_mid_chunk(self):
        log = self.inlog()
        c = self.px.attach("app")
        c.send(b"abc" + self.KEY + b"def")
        status, sig, out = expect_exit(c, b"[detached from app]")
        self.assertEqual((status, sig), (0, None))
        c = self.px.attach("app")
        self.sync(c, log, b"abc", b"|next")
        self.detach(c, "app", key=self.KEY)


class DetachKeyCtrlBackslash(DetachKeyForms):
    CONFIG = "detach_key = ctrl+backslash\n"
    KEY = CTRL_BACKSLASH
    DETACHES = [b"\x1b[92;5u", b"\x1b[92;5:1u", b"\x1b[92;69u", b"\x1b[92;133:1u",
                b"\x1b[92;197u", b"\x1b[27;5;92~"]
    SPLITS = [(b"\x1b[27;5", b";92~"), (b"\x1b[92;6", b"9:1u"), (b"\x1b[9", b"2;5u")]
    SWALLOWED = b"\x1b[92;5:2u\x1b[92;69:3u\x1b[92;133:2u"
    LOOKALIKES = (b"\x1b[92;6u" b"\x1b[27;6;92~" b"\x1b[92;7u" b"\x1b[92;13u" b"\x1b[92;70u"
                  b"\x1b[27;5;93~" b"\x1b[27;5;92u" b"\x1b[93;69u" b"\x1b[92;69~"
                  b"\x1b[93;5u" b"\x1b[92;3u" b"\x1b[92u"
                  b"\x1b[1;5D" b"\x1b[1;5:1D" b"\x1b[1;69D" b"\x1bOd" b"x")


class DetachKeyCtrlShiftLeft(DetachKeyForms):
    CONFIG = "detach_key = ctrl+shift+left\n"
    KEY = CTRL_SHIFT_LEFT
    DETACHES = [b"\x1b[1;6:1D", b"\x1b[1;70D", b"\x1b[1;134:1D", b"\x1b[1;198D"]
    SPLITS = [(b"\x1b[1;", b"6D"), (b"\x1b[1;7", b"0:1D")]
    SWALLOWED = b"\x1b[1;6:2D\x1b[1;70:3D\x1b[1;134:2D"
    LOOKALIKES = (b"\x1b[D" b"\x1b[1;2D" b"\x1b[1;5D" b"\x1b[1;69D" b"\x1b[1;5:1D" b"\x1bOd"
                  b"\x1b[1;6C" b"\x1b[1;7D" b"\x1b[1;14D" b"\x1c" b"\x1b[92;5u" b"x")


class DetachKeyInvalid(DetachKeyForms):
    """An invalid detach_key falls back to the default (ctrl+left)."""
    CONFIG = "detach_key = ctrl+q\n"

    def test_warning(self):
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0)
        self.assertIn(b"pmux: ~/.pmux/config:1: invalid value for detach_key: 'ctrl+q'", p.stderr)


class TuiDetachKeyConfig(TuiCase):
    """The list TUI honours detach_key too; the other keys reach the app."""

    def check(self, detach, passes, passes_bytes):
        log = self.marker("app")
        t = self.start_list("app")
        t.keys("Enter")
        self.wait_attached(t, "app")
        t.keys(passes)
        t.type("z")
        wait_until(lambda: read_file(log) == passes_bytes + b"z", msg=lambda: read_file(log))
        t.keys(detach)
        t.wait_list("app")
        t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="back in the list")
        self.assertEqual(read_file(log), passes_bytes + b"z", "detach key reached the app")

    def test_ctrl_backslash(self):
        self.px.write_config("theme = dark\ndetach_key = ctrl+backslash\n")
        self.check("C-\\", "C-Left", CTRL_LEFT)

    def test_ctrl_shift_left(self):
        self.px.write_config("theme = dark\ndetach_key = ctrl+shift+left\n")
        self.check("C-S-Left", "C-Left", CTRL_LEFT)

    def test_default_ctrl_left(self):
        self.check("C-Left", "C-\\", CTRL_BACKSLASH)


# ---------------------------------------------------------------------------
# 9. Home / End variants in the list


class HomeEnd(TuiCase):
    def test_home_end_variants(self):
        for n in ("h1", "h2", "h3"):
            self.new(n, "sleep", "1000")
        t = self.start_list("h1", "h2", "h3")
        self.wait_selected(t, "h1")
        t.keys("End")                  # tmux sends ESC [ 4 ~
        self.wait_selected(t, "h3")
        t.keys("Home")                 # ESC [ 1 ~
        self.wait_selected(t, "h1")
        for home, end in ((b"\x1b[7~", b"\x1b[8~"), (b"\x1b[1~", b"\x1b[4~"), (b"\x1b[H", b"\x1b[F")):
            t.raw(end)
            self.wait_selected(t, "h3")
            t.raw(home)
            self.wait_selected(t, "h1")


# ---------------------------------------------------------------------------
# 10. a late reply to the list's cursor-shape query never reaches the app


class ListQueryReply(TuiCase):
    def test_cursor_shape_reply_at_attach_is_dropped(self):
        log = self.px.path("qreply.in")
        self.px.create_probe("qreply", "inlog", log, winch_mark="qreply")
        c = self.px.spawn_attach([], rows=30, cols=100)
        c.delaybeforesend = None
        read_until(c, b"\x1bP$q q\x1b\\")   # the list screen's query, left unanswered
        read_until(c, b"qreply")             # the row is drawn
        c.send(b"\r")
        read_until(c, winch_marker("qreply").encode())
        # The terminal answers the query only now, split over two reads.
        c.send(CURSOR_SHAPE_REPLY[:5])
        time.sleep(0.005)
        c.send(CURSOR_SHAPE_REPLY[5:] + b"abc")
        self.wait_log(log, b"abc")
        # Outside the short window after attach, the same bytes are input.
        time.sleep(0.6)
        c.send(CURSOR_SHAPE_REPLY + b"d")
        self.wait_log(log, b"abc" + CURSOR_SHAPE_REPLY + b"d")
        c.send(DETACH)
        read_until(c, b"qreply")             # back in the list
        c.send(b"\x03")
        time.sleep(0.1)
        c.send(b"\x03")
        c.expect(pexpect.EOF)


# ---------------------------------------------------------------------------
# 11. capture-pane style carries across lines (harness)


class HarnessStyleCarry(TuiCase):
    def test_style_carries_across_capture_lines(self):
        t = TmuxTui(self.px, cmd=["sh", "-c", r"printf '\033[41mA\033[K\r\nB\033[0m\r\nC'; sleep 30"])
        self.addCleanup(t.close)
        t.start()
        t.wait_for("C")
        lines = t.screen_ansi()
        cells = t.cells()
        self.assertEqual(cells[1][0].ch, "B")
        self.assertEqual(cells[1][0].bg, ("idx", 1),
                         "line 1 inherits line 0's background: %r" % lines[:3])
        self.assertIsNone(cells[2][0].bg)
        self.assertIsNone(parse_ansi_line(lines[1])[0].bg, "a lone line starts from defaults")

    def test_selection_bar_does_not_bleed(self):
        self.new("s1", "sleep", "1000")
        self.new("s2", "sleep", "1001")
        t = self.start_list("s1", "s2")
        self.wait_selected(t, "s1")
        cells = t.cells()
        for col in (0, 5, t.cols - 3):
            self.assertIsNone(cells[4][col].bg, "row below the bar, col %d: %r" % (col, cells[4][col]))


# ---------------------------------------------------------------------------
# 12. connect errors


class ConnectErrors(PmuxTestCase):
    def test_socket_path_too_long(self):
        deep = os.path.join(self.px.root, "d" * 60, "e" * 60)
        os.makedirs(deep, 0o700)
        t0 = time.monotonic()
        p = self.px.run("-l", extra_env={"XDG_RUNTIME_DIR": deep})
        self.assertEqual(p.returncode, 1)
        self.assertIn(b"pmux: socket path too long", p.stderr)
        self.assertLess(time.monotonic() - t0, 1.5, "must not wait for a daemon")

    def test_daemon_that_cannot_start(self):
        ro = os.path.join(self.px.root, "ro")
        os.mkdir(ro, 0o500)
        self.addCleanup(os.chmod, ro, 0o700)
        p = self.px.run("-l", extra_env={"XDG_RUNTIME_DIR": ro})
        self.assertEqual(p.returncode, 1)
        self.assertIn(b"pmux: cannot connect to daemon (see ~/.pmux/daemon.log)", p.stderr)


if __name__ == "__main__":
    unittest.main()
