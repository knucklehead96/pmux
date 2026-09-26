"""CLI behaviour: daemon auto-spawn, -n / -l / -k / -a exit codes, list format."""
import os
import shutil
import stat
import subprocess
import time
import unittest

from helpers import (FAST, PMUX_BIN, TIMEOUT, PmuxTestCase, pid_alive, read_file,
                     wait_until)


def cmdline(pid):
    return read_file("/proc/%s/cmdline" % pid).split(b"\0")[:-1]


class DaemonTests(PmuxTestCase):
    def test_autospawn_creates_socket_dir_and_pidfile(self):
        self.assertFalse(os.path.exists(self.px.sock_dir))
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"", "empty list must print nothing")
        self.assertMode0700(self.px.sock_dir)
        self.assertTrue(stat.S_ISSOCK(os.stat(self.px.sock).st_mode), "%s is not a socket" % self.px.sock)
        pid = self.px.daemon_pid()
        self.assertIsNotNone(pid, "pidfile %s missing/empty" % self.px.pidfile)
        self.assertTrue(pid_alive(pid), "daemon pid %d from pidfile is not alive" % pid)
        # A second client reuses the running daemon.
        self.assertEqual(self.px.run("-l").returncode, 0)
        self.assertEqual(self.px.daemon_pid(), pid, "daemon was respawned")
        self.assertTrue(pid_alive(pid))

    def test_autospawned_daemon_does_not_hold_client_stdio(self):
        # With pipes, communicate() only returns once every writer is closed:
        # a daemon that keeps the client's stdout/stderr would hang this.
        env = self.px.make_env(self.px.work)
        try:
            p = subprocess.run([PMUX_BIN, "-l"], cwd=self.px.work, env=env,
                               stdin=subprocess.DEVNULL, capture_output=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.fail("pmux -l with piped stdio did not return: daemon inherited the pipes")
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_foreground_daemon(self):
        env = self.px.make_env(self.px.work)
        log = open(os.path.join(self.px.root, "daemon.log"), "wb")
        self.addCleanup(log.close)
        d = subprocess.Popen([PMUX_BIN, "--daemon"], cwd=self.px.work, env=env,
                             stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        self.px.popens.append(d)
        wait_until(lambda: os.path.exists(self.px.sock) or d.poll() is not None,
                   msg="foreground daemon never created its socket")
        self.assertIsNone(d.poll(), "--daemon exited (rc=%r)" % d.returncode)
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stdout, b"")
        self.assertEqual(self.px.daemon_pid(), d.pid,
                         "pidfile should name the foreground daemon (no second daemon spawned)")
        self.assertIsNone(d.poll(), "--daemon should stay in the foreground")

    def test_help(self):
        p = self.px.run("-h")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"usage", (p.stdout + p.stderr).lower())

    def test_version(self):
        for flag in ("--version", "-V"):
            with self.subTest(flag=flag):
                p = self.px.run(flag)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertRegex(p.stdout, rb"^pmux \d+\.\d+\.\d+\n$")
        self.assertFalse(os.path.exists(self.px.sock_dir), "--version must not spawn the daemon")

    def test_bad_args(self):
        for args in (["--bogus"], ["-Q"], ["-a"], ["-k"], ["-f"], ["-l", "--force"], ["--stop", "-l"]):
            with self.subTest(args=args):
                p = self.px.run(*args)
                self.assertEqual(p.returncode, 2, "pmux %s: stderr=%r" % (" ".join(args), p.stderr))


class CreateTests(PmuxTestCase):
    def new(self, *args, **kw):
        p = self.px.run("-n", *args, **kw)
        self.assertEqual(p.returncode, 0, "pmux -n %s failed: %r" % (" ".join(args), p.stderr))
        return p

    def test_default_name_and_suffix(self):
        for _ in range(3):
            self.new("-d", "--", "sleep", "600")
        rows = self.px.list()
        self.assertEqual([r["name"] for r in rows], ["proj", "proj-2", "proj-3"])

    def test_explicit_name_and_duplicate(self):
        self.new("web", "-d", "--", "sleep", "600")
        p = self.px.run("-n", "web", "-d", "--", "sleep", "601")
        self.assertEqual(p.returncode, 1, "duplicate name must exit 1; stderr=%r" % p.stderr)
        rows = self.px.list()
        self.assertEqual([(r["name"], r["command"]) for r in rows], [("web", "sleep 600")])

    def test_list_fields(self):
        other = os.path.join(self.px.root, "work", "other dir")
        os.makedirs(other)
        self.new("a", "-d", "--", "sleep", "600")
        self.new("b", "-d", "--", "sh", "-c", "sleep 600", cwd=other)
        rows = self.px.list()
        self.assertEqual([r["name"] for r in rows], ["a", "b"], "creation order")
        a, b = rows
        self.assertEqual(a["dir"], self.px.work)
        self.assertEqual(b["dir"], other)
        self.assertEqual(a["command"], "sleep 600")
        self.assertEqual(b["command"], "sh -c sleep 600")
        for r in rows:
            self.assertEqual(r["state"], "running", r)
            self.assertTrue(r["pid"].isdigit(), r)
            self.assertTrue(pid_alive(int(r["pid"])), r)
        self.assertEqual(cmdline(a["pid"]), [b"sleep", b"600"])
        self.assertEqual(os.readlink("/proc/%s/cwd" % a["pid"]), self.px.work)
        self.assertEqual(os.readlink("/proc/%s/cwd" % b["pid"]), other)

    def test_default_command_is_shell(self):
        alt = shutil.which("bash") or shutil.which("dash")
        if alt:
            self.new("s1", "-d", extra_env={"SHELL": alt})
            self.assertEqual(self.px.entry("s1")["command"], alt)
        env = self.px.make_env(self.px.work)
        del env["SHELL"]
        self.new("s2", "-d", env=env)
        self.assertEqual(self.px.entry("s2")["command"], "/bin/sh")
        for name in ("s1", "s2") if alt else ("s2",):
            self.assertEqual(self.px.entry(name)["state"], "running")

    def test_exited_and_signaled_states(self):
        self.new("ex", "-d", "--", "sh", "-c", "exit 3")
        self.new("sg", "-d", "--", "sh", "-c", "kill -TERM $$")
        self.px.wait_state("ex", lambda s: s != "running")
        self.px.wait_state("sg", lambda s: s != "running")
        self.assertEqual(self.px.entry("ex")["state"], "exited:3")
        self.assertEqual(self.px.entry("sg")["state"], "signaled:15")

    @unittest.skipIf(FAST, "PMUX_FAST=1: skipping >5 s silence test")
    def test_silent_process_stays_running(self):
        # There is no idle state: STATE is running | exited:N | signaled:N.
        self.new("quiet", "-d", "--", "sleep", "600")
        deadline = time.monotonic() + 6.5
        while time.monotonic() < deadline:
            self.assertEqual(self.px.entry("quiet")["state"], "running")
            time.sleep(0.5)
        self.assertEqual(self.px.entry("quiet")["state"], "running")


class KillTests(PmuxTestCase):
    def test_kill_running_removes(self):
        self.assertEqual(self.px.run("-n", "k", "-d", "--", "sleep", "600").returncode, 0)
        self.assertEqual(self.px.run("-n", "other", "-d", "--", "sleep", "601").returncode, 0)
        pid = int(self.px.entry("k")["pid"])
        p = self.px.run("-k", "k")
        self.assertEqual(p.returncode, 0, p.stderr)
        # -k returns only after the process exited and its entry is gone:
        # no polling here.
        self.assertFalse(pid_alive(pid), "process still alive after -k returned")
        self.assertEqual([r["name"] for r in self.px.list()], ["other"],
                         "-k on a running process must also remove it")
        p = self.px.run("-k", "k")
        self.assertEqual(p.returncode, 1, "k is gone: second -k must fail; stderr=%r" % p.stderr)

    def test_kill_escalates_to_sigkill(self):
        log = self.px.path("sig.log")
        self.px.create_probe("stubborn", "signals", log)
        pid = int(self.px.entry("stubborn")["pid"])
        t0 = time.monotonic()
        p = self.px.run("-k", "stubborn", timeout=15)
        elapsed = time.monotonic() - t0
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn(b"HUP", read_file(log).split(), "SIGHUP must reach the process group first")
        self.assertGreaterEqual(elapsed, 2.5, "SIGKILL came too early (%.2fs)" % elapsed)
        self.assertLess(elapsed, 8, "-k took %.2fs" % elapsed)
        self.assertFalse(pid_alive(pid))
        self.assertIsNone(self.px.entry("stubborn"), "SIGKILLed process must be removed")

    def test_kill_exited_removes(self):
        for name, script in (("ex", "exit 3"), ("sg", "kill -TERM $$")):
            self.assertEqual(self.px.run("-n", name, "-d", "--", "sh", "-c", script).returncode, 0)
            self.px.wait_state(name, lambda s: s != "running")
        for name in ("ex", "sg"):
            with self.subTest(name=name):
                p = self.px.run("-k", name)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertIsNone(self.px.entry(name), "-k on an exited process must remove it")
        self.assertEqual(self.px.list(), [])

    def test_unknown_names(self):
        self.assertEqual(self.px.run("-n", "real", "-d", "--", "sleep", "600").returncode, 0)
        for flag in ("-k", "-a"):
            with self.subTest(flag=flag):
                p = self.px.run(flag, "nope")
                self.assertEqual(p.returncode, 1, "pmux %s nope: stderr=%r" % (flag, p.stderr))
        self.assertEqual([r["name"] for r in self.px.list()], ["real"])


class StopTests(PmuxTestCase):
    def assertDaemonGone(self, pid):
        # --stop returns only once the daemon has exited: no polling here.
        self.assertFalse(pid_alive(pid), "daemon still alive after --stop returned")
        self.assertFalse(os.path.exists(self.px.sock), "socket left behind")
        self.assertFalse(os.path.exists(self.px.pidfile), "pidfile left behind")

    def test_stop_without_daemon(self):
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, b"pmux: no daemon running\n")
        self.assertFalse(os.path.exists(self.px.sock_dir), "--stop must not spawn the daemon")

    def test_stop_idle_daemon(self):
        self.assertEqual(self.px.run("-n", "ex", "-d", "--", "sh", "-c", "exit 3").returncode, 0)
        self.px.wait_state("ex", lambda s: s != "running")
        pid = self.px.daemon_pid()
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((p.stdout, p.stderr), (b"", b""))
        self.assertDaemonGone(pid)

    def test_stop_refuses_running_processes(self):
        for name in ("one", "two"):
            self.assertEqual(self.px.run("-n", name, "-d", "--", "sleep", "600").returncode, 0)
        self.assertEqual(self.px.run("-n", "ex", "-d", "--", "sh", "-c", "exit 0").returncode, 0)
        self.px.wait_state("ex", lambda s: s != "running")
        pid = self.px.daemon_pid()
        p = self.px.run("--stop")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertEqual(p.stderr, b"pmux: 2 process(es) still running: one, two\n"
                                   b"use pmux --stop --force to kill them and stop the daemon\n")
        self.assertTrue(pid_alive(pid), "daemon must be left alone")
        self.assertEqual(self.px.daemon_pid(), pid)
        self.assertEqual([(r["name"], r["state"]) for r in self.px.list()],
                         [("one", "running"), ("two", "running"), ("ex", "exited:0")])

    def test_stop_force_kills_and_stops(self):
        self.assertEqual(self.px.run("-n", "plain", "-d", "--", "sleep", "600").returncode, 0)
        log = self.px.path("sig.log")
        self.px.create_probe("stubborn", "signals", log)
        pids = [int(self.px.entry(n)["pid"]) for n in ("plain", "stubborn")]
        daemon = self.px.daemon_pid()
        t0 = time.monotonic()
        p = self.px.run("--stop", "-f", timeout=15)
        elapsed = time.monotonic() - t0
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual((p.stdout, p.stderr), (b"", b""))
        self.assertIn(b"HUP", read_file(log).split(), "SIGHUP must reach the process group first")
        self.assertGreaterEqual(elapsed, 2.5, "SIGKILL came too early (%.2fs)" % elapsed)
        self.assertLess(elapsed, 8, "--stop --force took %.2fs" % elapsed)
        for pid in pids:
            self.assertFalse(pid_alive(pid), "process %d survived --stop --force" % pid)
        self.assertDaemonGone(daemon)
        # The next command starts a fresh, empty daemon.
        self.assertEqual(self.px.list(), [])


if __name__ == "__main__":
    unittest.main()
