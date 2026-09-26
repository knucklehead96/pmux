"""Shared fixtures for the pmux test suite.

Every test gets a fresh PmuxEnv: private XDG_RUNTIME_DIR and HOME under a
temp dir, SHELL=/bin/sh, TERM=xterm-256color, and a minimal, fully
controlled environment.  Teardown kills the daemon (pidfile: SIGTERM, then
SIGKILL) and every process that still carries this env's unique
XDG_RUNTIME_DIR in /proc/<pid>/environ, then removes the temp dir.
"""
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import termios
import time
import unittest

import pexpect

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(TESTS_DIR)
PMUX_BIN = os.environ.get("PMUX_BIN") or os.path.join(REPO_DIR, "build", "pmux")
PROBE = os.path.join(TESTS_DIR, "probe.py")
PYTHON = sys.executable
FAST = os.environ.get("PMUX_FAST") == "1"

TIMEOUT = 10.0

sys.path.insert(0, TESTS_DIR)
import probe  # noqa: E402  (re-exported markers / terminator)

DETACH = b"\x1c"


# --------------------------------------------------------------------------
# diagnostics


def hexdump(data, start=0, width=16):
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hx = " ".join("%02x" % b for b in chunk)
        asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append("  %08x  %-*s  %s" % (start + off, width * 3 - 1, hx, asc))
    return "\n".join(lines) or "  <empty>"


def bytes_diff(expected, actual, context=32):
    """Human-readable description of the first mismatch, or None if equal."""
    expected, actual = bytes(expected), bytes(actual)
    if expected == actual:
        return None
    n = min(len(expected), len(actual))
    idx = next((i for i in range(n) if expected[i] != actual[i]), n)
    lo = max(0, idx - context)
    hi = idx + context
    return (
        "byte mismatch: expected %d bytes, got %d bytes; first difference at "
        "offset %d (0x%x)\n expected[%d:%d]:\n%s\n actual[%d:%d]:\n%s"
        % (len(expected), len(actual), idx, idx,
           lo, hi, hexdump(expected[lo:hi], lo),
           lo, hi, hexdump(actual[lo:hi], lo))
    )


def wait_until(pred, timeout=TIMEOUT, interval=0.02, msg=None):
    """Poll pred() until it returns a truthy value; return that value.

    On timeout raise AssertionError with msg (a string, or a callable
    evaluated at failure time to describe the last observed state)."""
    deadline = time.monotonic() + timeout
    while True:
        val = pred()
        if val:
            return val
        if time.monotonic() >= deadline:
            text = msg() if callable(msg) else msg
            raise AssertionError("timed out after %.1fs: %s" % (timeout, text or pred))
        time.sleep(interval)


def read_file(path, default=b""):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return default


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # A zombie still "exists"; treat it as dead.
    try:
        with open("/proc/%d/stat" % pid) as f:
            return f.read().rsplit(")", 1)[1].split()[0] != "Z"
    except (FileNotFoundError, IndexError):
        return False


# --------------------------------------------------------------------------
# environment fixture


class PmuxEnv:
    def __init__(self):
        base = os.environ.get("PMUX_TEST_TMPDIR") or "/tmp"
        # Keep the path short: AF_UNIX socket paths are limited to 108 bytes.
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="pmuxt-", dir=base))
        self.runtime = os.path.join(self.root, "run")
        self.home = os.path.join(self.root, "home")
        self.files = os.path.join(self.root, "files")
        self.work = os.path.join(self.root, "work", "proj")
        os.mkdir(self.runtime, 0o700)
        for d in (self.home, self.files, self.work):
            os.makedirs(d)
        self.sock_dir = os.path.join(self.runtime, "pmux")
        self.sock = os.path.join(self.sock_dir, "pmux.sock")
        self.pidfile = os.path.join(self.sock_dir, "pmux.pid")
        self.env = {
            "PATH": os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
            "HOME": self.home,
            "XDG_RUNTIME_DIR": self.runtime,
            "SHELL": "/bin/sh",
            "TERM": "xterm-256color",
            "COLORTERM": "truecolor",
            "LANG": "C.UTF-8",
        }
        self.children = []   # pexpect clients
        self.popens = []     # foreground daemons etc.
        self._counter = 0

    # -- process helpers ---------------------------------------------------

    def make_env(self, cwd=None, env=None, extra=None):
        e = dict(self.env if env is None else env)
        if cwd is not None and env is None:
            e["PWD"] = cwd
        if extra:
            e.update(extra)
        return e

    def run(self, *args, cwd=None, env=None, extra_env=None, timeout=TIMEOUT):
        """Run pmux to completion.  stdout/stderr go to files, not pipes, so
        a daemon that (wrongly) inherits them cannot make this hang; the
        pipe case is covered separately in test_cli."""
        cwd = cwd or self.work
        full_env = self.make_env(cwd, env, extra_env)
        self._counter += 1
        out_path = os.path.join(self.root, "out.%d" % self._counter)
        err_path = os.path.join(self.root, "err.%d" % self._counter)
        with open(out_path, "wb") as out, open(err_path, "wb") as err:
            try:
                p = subprocess.run([PMUX_BIN, *args], cwd=cwd, env=full_env,
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   timeout=timeout)
            except subprocess.TimeoutExpired:
                raise AssertionError("pmux %s did not finish within %.0fs; stderr=%r"
                                     % (" ".join(args), timeout, read_file(err_path)))
        return subprocess.CompletedProcess(p.args, p.returncode,
                                           read_file(out_path), read_file(err_path))

    def spawn_attach(self, args, rows=24, cols=80, cwd=None, env=None):
        """Start an interactive pmux client under a pty (raw bytes)."""
        cwd = cwd or self.work
        child = pexpect.spawn(PMUX_BIN, list(args), cwd=cwd, env=self.make_env(cwd, env),
                              dimensions=(rows, cols), encoding=None,
                              timeout=TIMEOUT, maxread=65536)
        self.children.append(child)
        return child

    def wait_raw(self, child, timeout=TIMEOUT):
        """Wait until the client has put its tty in raw mode.  tcgetattr on
        the pty master reports the slave's termios on Linux.  Sending input
        earlier could be line-edited, echoed or flushed by TCSAFLUSH."""
        def raw():
            if not child.isalive():
                return "dead"
            try:
                lflag = termios.tcgetattr(child.child_fd)[3]
            except termios.error:
                return False
            return not (lflag & (termios.ICANON | termios.ECHO | termios.ISIG))
        state = wait_until(raw, timeout,
                           msg=lambda: "client never entered raw mode; output so far: %r"
                           % drain(child))
        if state == "dead":
            raise AssertionError("client exited before entering raw mode; output=%r status=%r"
                                 % (drain(child), child.exitstatus))

    def attach(self, name, rows=24, cols=80):
        child = self.spawn_attach(["-a", name], rows, cols)
        self.wait_raw(child)
        return child

    def list(self):
        p = self.run("-l")
        if p.returncode != 0:
            raise AssertionError("pmux -l failed rc=%d stderr=%r" % (p.returncode, p.stderr))
        rows = []
        for line in p.stdout.decode().splitlines():
            parts = line.split("\t")
            if len(parts) != 5:
                raise AssertionError("malformed -l line (want 5 TSV fields): %r" % line)
            rows.append(dict(zip(("name", "state", "pid", "dir", "command"), parts)))
        return rows

    def entry(self, name):
        for r in self.list():
            if r["name"] == name:
                return r
        return None

    def wait_state(self, name, pred, timeout=TIMEOUT):
        last = {}

        def check():
            e = self.entry(name)
            last["e"] = e
            return e if e and pred(e["state"]) else None
        return wait_until(check, timeout, interval=0.1,
                          msg=lambda: "state of %r never matched; last entry %r" % (name, last.get("e")))

    # -- probe helpers -----------------------------------------------------

    def path(self, name):
        return os.path.join(self.files, name)

    def probe_cmd(self, mode, *args, ready=None):
        cmd = [PYTHON, PROBE]
        if ready:
            cmd += ["--ready", ready]
        return cmd + [mode, *args]

    def create_probe(self, name, mode, *args, env=None):
        """pmux -n NAME -d -- probe MODE ARGS; wait for the probe's ready file."""
        ready = self.path(name + ".ready")
        p = self.run("-n", name, "-d", "--", *self.probe_cmd(mode, *args, ready=ready), env=env)
        if p.returncode != 0:
            raise AssertionError("pmux -n %s -d failed rc=%d stderr=%r"
                                 % (name, p.returncode, p.stderr))
        self.wait_ready(ready, name)
        return ready

    def wait_ready(self, ready, name="probe"):
        wait_until(lambda: os.path.exists(ready), msg=lambda: "%s never became ready; -l=%r"
                   % (name, self.run("-l").stdout))
        return int(read_file(ready))

    # -- teardown ----------------------------------------------------------

    def _env_pids(self):
        needle = ("XDG_RUNTIME_DIR=" + self.runtime).encode()
        pids = []
        for d in os.listdir("/proc"):
            if not d.isdigit() or int(d) == os.getpid():
                continue
            try:
                with open("/proc/%s/environ" % d, "rb") as f:
                    if needle in f.read().split(b"\0"):
                        pids.append(int(d))
            except OSError:
                pass
        return pids

    def daemon_pid(self):
        try:
            return int(read_file(self.pidfile).strip() or 0) or None
        except ValueError:
            return None

    def close(self):
        for c in self.children:
            try:
                c.close(force=True)
            except Exception:
                pass
        pid = self.daemon_pid()
        if pid and pid_alive(pid):
            try:
                os.kill(pid, signal.SIGTERM)
                wait_until(lambda: not pid_alive(pid), 2.0)
            except (ProcessLookupError, AssertionError):
                pass
        for p in self.popens:
            if p.poll() is None:
                p.terminate()
        for _ in range(3):
            pids = self._env_pids()
            if not pids:
                break
            for pid in pids:
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            time.sleep(0.05)  # let the kernel reap before re-scanning
        for p in self.popens:
            try:
                p.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass
        subprocess.run(["rm", "-rf", self.root])


def drain(child):
    """Non-blocking read of whatever the client has written so far."""
    out = b""
    try:
        while True:
            out += child.read_nonblocking(65536, timeout=0)
    except (pexpect.TIMEOUT, pexpect.EOF, OSError, ValueError):
        pass
    return out


def read_until(child, marker, timeout=TIMEOUT):
    """Read raw client output until marker; return everything up to and
    including it.  Bytes read past the marker are pushed back into
    child.buffer so later expect() calls still see them."""
    buf = bytearray(child.buffer)
    child.buffer = b""
    deadline = time.monotonic() + timeout
    while True:
        idx = buf.find(marker)
        if idx >= 0:
            end = idx + len(marker)
            child.buffer = bytes(buf[end:])
            return bytes(buf[:end])
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise AssertionError("marker %r not seen within %.0fs; got %d bytes, tail:\n%s"
                                 % (marker, timeout, len(buf), hexdump(bytes(buf[-128:]))))
        try:
            buf += child.read_nonblocking(65536, timeout=min(remaining, 0.5))
        except pexpect.TIMEOUT:
            pass
        except pexpect.EOF:
            raise AssertionError("client hit EOF before marker %r; output tail:\n%s"
                                 % (marker, hexdump(bytes(buf[-256:]))))


def expect_exit(child, message, timeout=TIMEOUT):
    """Read the client's output to EOF, assert `message` (bytes) is in it and
    return (exit_status, signal_status, output)."""
    try:
        child.expect(pexpect.EOF, timeout=timeout)
    except pexpect.TIMEOUT:
        raise AssertionError("client did not exit within %.0fs (waiting for %r); output=%r"
                             % (timeout, message, child.before))
    out = child.before
    wait_until(lambda: not child.isalive(), msg="client closed its pty but did not exit")
    if message is not None and message not in out:
        raise AssertionError("expected %r in client output, got %r" % (message, out))
    return child.exitstatus, child.signalstatus, out


class PmuxTestCase(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        if not os.access(PMUX_BIN, os.X_OK):
            self.fail("pmux binary not found/executable at %s (set PMUX_BIN)" % PMUX_BIN)
        self.px = PmuxEnv()
        self.addCleanup(self.px.close)

    # assertion helpers

    def assertBytesEqual(self, expected, actual, what="data"):
        diff = bytes_diff(expected, actual)
        if diff:
            self.fail("%s differs\n%s" % (what, diff))

    def wait_log(self, path, expected, timeout=TIMEOUT, what="probe input log"):
        """Wait until the log reaches len(expected) bytes, then compare exactly."""
        try:
            wait_until(lambda: len(read_file(path)) >= len(expected), timeout)
        except AssertionError:
            pass  # fall through to the exact comparison for a useful diff
        self.assertBytesEqual(expected, read_file(path), what)

    def detach(self, child, name, key=DETACH):
        child.send(key)
        status, sig, out = expect_exit(child, b"[detached from %s]" % name.encode())
        self.assertEqual((status, sig), (0, None), "client exit after detach; output=%r" % out)
        return out

    def assertMode0700(self, path):
        st = os.stat(path)
        self.assertTrue(stat.S_ISDIR(st.st_mode), path)
        self.assertEqual(stat.S_IMODE(st.st_mode), 0o700, "%s mode is %o" % (path, stat.S_IMODE(st.st_mode)))


def load_json(path):
    with open(path) as f:
        return json.load(f)
