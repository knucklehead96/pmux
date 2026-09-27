"""Version handshake (HELLO) between client and daemon.

- a daemon older than HELLO, or of another protocol version: every client
  mode (incl. the list TUI, before it takes over the terminal) prints a
  "restart it with: pmux --stop" message and exits 1 without sending any
  further request; -V stays offline.
- a client older than HELLO gets an ERROR saying so and the connection is
  closed, except for STOP (frozen), which it can still use.
- pmux --stop on a daemon too old for STOP: SIGTERM after checking that the
  pidfile names the socket's peer; --force semantics as for STOP.

The old / mismatched daemon is tests/fake_daemon.py.
"""
import os
import signal
import socket
import struct
import subprocess
import time

import pexpect

from helpers import (PMUX_BIN, PYTHON, TESTS_DIR, TIMEOUT, PmuxTestCase, pid_alive, read_file,
                     wait_until)

FAKE = os.path.join(TESTS_DIR, "fake_daemon.py")
LIST, ERROR, OK, STOP, HELLO = 1, 14, 15, 17, 20
PROTOCOL = 1  # kProtocolVersion (src/common/protocol.hpp)


def pstr(b):
    return struct.pack("<I", len(b)) + b


class VersionCase(PmuxTestCase):
    def setUp(self):
        super().setUp()
        self.version = self.px.run("-V").stdout.decode().split()[1]
        self.log = self.px.path("fake.log")

    def start_fake(self, *args):
        d = subprocess.Popen([PYTHON, FAKE, self.px.sock, self.px.pidfile, self.log, *args],
                             env=self.px.make_env(self.px.work), stdin=subprocess.DEVNULL)
        self.px.popens.append(d)
        wait_until(lambda: os.path.exists(self.px.pidfile) or d.poll() is not None,
                   msg="fake daemon never started")
        self.assertIsNone(d.poll(), "fake daemon exited")
        return d

    def requests(self):
        return [int(t) for t in read_file(self.log).split()]

    def old_message(self, pid):
        return ("pmux: the running daemon (pid %d, an older version) doesn't match this pmux (%s). "
                "Restart it with: pmux --stop\n" % (pid, self.version)).encode()

    def sleeper(self):
        """A process in its own session, as the daemon's processes are."""
        p = subprocess.Popen(["sleep", "600"], env=self.px.make_env(self.px.work),
                             start_new_session=True)
        self.px.popens.append(p)
        return p


class OldDaemon(VersionCase):
    def test_cli_modes_refuse_old_daemon(self):
        d = self.start_fake()
        for args in (["-l"], ["-n", "x", "-d", "--", "true"], ["-a", "x"], ["-k", "x"]):
            with self.subTest(args=args):
                open(self.log, "w").close()
                p = self.px.run(*args)
                self.assertEqual(p.returncode, 1, p.stderr)
                self.assertEqual((p.stdout, p.stderr), (b"", self.old_message(d.pid)))
                self.assertEqual(self.requests(), [HELLO], "the client went on after HELLO")
        self.assertEqual(self.px.daemon_pid(), d.pid, "no daemon may be spawned")
        self.assertIsNone(d.poll())

    def test_tui_refuses_old_daemon_before_fullscreen(self):
        d = self.start_fake()
        child = pexpect.spawn(PMUX_BIN, [], cwd=self.px.work, env=self.px.make_env(self.px.work),
                              dimensions=(24, 80), encoding=None, timeout=TIMEOUT)
        self.px.children.append(child)
        out = child.read()
        child.close()
        self.assertEqual(child.exitstatus, 1, out)
        # Only the message: no alternate screen, mouse or cursor sequences.
        self.assertEqual(out, self.old_message(d.pid).replace(b"\n", b"\r\n"))
        self.assertEqual(self.requests(), [HELLO])

    def test_version_does_not_connect(self):
        self.start_fake()
        for flag in ("-V", "--version"):
            p = self.px.run(flag)
            self.assertEqual((p.returncode, p.stdout), (0, b"pmux %s\n" % self.version.encode()))
        self.assertEqual(self.requests(), [])


class ProtocolMismatch(VersionCase):
    def message(self, pid):
        return ("pmux: the running daemon (pid %d, pmux 9.9.9, protocol %d) doesn't match this pmux "
                "(%s, protocol %d). Restart it with: pmux --stop\n"
                % (pid, PROTOCOL + 1, self.version, PROTOCOL)).encode()

    def test_cli_and_tui_refuse_other_protocol(self):
        d = self.start_fake("--hello", str(PROTOCOL + 1))
        p = self.px.run("-l")
        self.assertEqual((p.returncode, p.stdout, p.stderr), (1, b"", self.message(d.pid)))
        child = pexpect.spawn(PMUX_BIN, [], cwd=self.px.work, env=self.px.make_env(self.px.work),
                              dimensions=(24, 80), encoding=None, timeout=TIMEOUT)
        self.px.children.append(child)
        out = child.read()
        child.close()
        self.assertEqual((child.exitstatus, out), (1, self.message(d.pid).replace(b"\n", b"\r\n")))
        self.assertEqual(self.requests(), [HELLO, HELLO])

    def test_stop_uses_stop_on_other_protocol(self):
        d = self.start_fake("--hello", str(PROTOCOL + 1))
        p = self.px.run("--stop")
        self.assertEqual((p.returncode, p.stdout, p.stderr), (0, b"", b""))
        self.assertEqual(self.requests(), [HELLO, STOP])
        self.assertEqual(d.wait(TIMEOUT), 0)


class OldClient(PmuxTestCase):
    def connect(self):
        self.assertEqual(self.px.run("-l").returncode, 0)  # start the real daemon
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(TIMEOUT)
        s.connect(self.px.sock)
        self.addCleanup(s.close)
        return s

    def read_all(self, s):
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                return data
            data += chunk

    def test_request_before_hello_is_refused(self):
        s = self.connect()
        s.sendall(struct.pack("<IB", 1, LIST))
        data = self.read_all(s)   # the daemon closes the connection after the ERROR
        msg = ("this pmux is older than the running daemon (pmux %s, pid %d); use the matching "
               "pmux binary, or restart the daemon: pmux --stop"
               % (self.px.run("-V").stdout.decode().split()[1], self.px.daemon_pid())).encode()
        payload = bytes([ERROR]) + pstr(msg)
        self.assertEqual(data, struct.pack("<I", len(payload)) + payload)
        self.assertEqual(self.px.run("-l").returncode, 0, "the daemon must keep running")

    def test_stop_before_hello_stops_the_daemon(self):
        s = self.connect()
        pid = self.px.daemon_pid()
        s.sendall(struct.pack("<IB", 2, STOP) + b"\0")
        self.assertEqual(self.read_all(s), struct.pack("<IB", 1, OK))
        wait_until(lambda: not pid_alive(pid), msg="the daemon did not exit")
        self.assertFalse(os.path.exists(self.px.sock))
        self.assertEqual(self.px.list(), [], "a fresh daemon must start")

    def test_other_protocol_may_only_stop(self):
        s = self.connect()
        s.sendall(struct.pack("<IB", 9, HELLO) + struct.pack("<I", PROTOCOL + 1) + pstr(b""))
        s.sendall(struct.pack("<IB", 1, LIST))
        data = self.read_all(s)
        n = struct.unpack_from("<I", data)[0]
        self.assertEqual(data[4], HELLO)
        self.assertEqual(struct.unpack_from("<I", data, 5)[0], PROTOCOL)
        self.assertEqual(data[4 + n + 4], ERROR, data)
        self.assertIn(b"restart it with: pmux --stop", data[4 + n:])


class StopOldDaemon(VersionCase):
    def assertStopped(self, d, p, ended=b""):
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((p.stdout, p.stderr),
                         (b"", b"pmux: stopped the older daemon (pid %d)%s\n" % (d.pid, ended)))
        # --stop returns once the daemon has exited.
        self.assertIsNotNone(d.poll(), "the old daemon is still running")
        self.assertFalse(os.path.exists(self.px.sock))
        # A fresh daemon starts on the next command.
        self.assertEqual(self.px.list(), [])
        self.assertNotEqual(self.px.daemon_pid(), d.pid)

    def test_stop_signals_idle_old_daemon(self):
        d = self.start_fake()
        p = self.px.run("--stop")
        self.assertEqual(self.requests(), [HELLO, STOP, LIST])
        self.assertStopped(d, p)

    def test_stop_refuses_running_processes_then_force_ends_them(self):
        s = self.sleeper()
        d = self.start_fake("--running", "busy:%d" % s.pid)
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stderr, b"pmux: 1 process(es) still running: busy\n"
                                   b"use pmux --stop --force to kill them and stop the daemon\n")
        self.assertIsNone(d.poll(), "the old daemon must be left alone")
        self.assertIsNone(s.poll())
        t0 = time.monotonic()
        p = self.px.run("--stop", "--force", timeout=15)
        elapsed = time.monotonic() - t0
        # The fake has no terminals to hang up: the process only goes with SIGKILL, after 3 s.
        self.assertEqual(s.wait(TIMEOUT), -signal.SIGKILL)
        self.assertGreaterEqual(elapsed, 2.5)
        self.assertStopped(d, p, b"; its 1 process(es) were ended")

    def test_stop_needs_force_without_list(self):
        d = self.start_fake("--no-list")
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stderr,
                         b"pmux: cannot tell whether the running daemon (pid %d, an older version) "
                         b"has running processes\nuse pmux --stop --force to kill them and stop the "
                         b"daemon\n" % d.pid)
        self.assertIsNone(d.poll())
        p = self.px.run("--stop", "-f")
        self.assertStopped(d, p, b"; any processes it ran were ended")

    def test_stop_refuses_pidfile_not_naming_the_peer(self):
        s = self.sleeper()
        d = self.start_fake("--pidfile-pid", str(s.pid))
        p = self.px.run("--stop", "--force")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stderr,
                         b"pmux: the running daemon is too old for --stop, and its pidfile (pid %d) "
                         b"does not name the process on its socket (pid %d); not signalling anything\n"
                         % (s.pid, d.pid))
        time.sleep(0.2)
        self.assertIsNone(d.poll(), "the daemon was signalled")
        self.assertTrue(pid_alive(s.pid), "the pidfile's process was signalled")


if __name__ == "__main__":
    import unittest
    unittest.main()
