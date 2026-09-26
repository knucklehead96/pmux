"""Attach contract: byte-exact passthrough, detach keys, resize, env, exits."""
import os
import random
import time
import unittest

from helpers import (DETACH, PmuxTestCase, expect_exit, input_corpus_chunks, load_json, probe,
                     read_file, read_until, wait_until)

KITTY_DETACH = b"\x1b[92;5u"
KITTY_DETACH_PRESS = b"\x1b[92;5:1u"
KITTY_RELEASE = b"\x1b[92;5:3u"


class AttachCase(PmuxTestCase):
    def setUp(self):
        super().setUp()
        # Start the daemon from a non-tty client first so it can never
        # inherit a pexpect pty (which would keep the pty open after the
        # client exits and hide EOF).
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, "daemon start via -l failed: %r" % p.stderr)

    def inlog(self, name="app"):
        log = self.px.path(name + ".in")
        self.px.create_probe(name, "inlog", log)
        return log

    def sync(self, child, log, expected_prefix, token=b"<sync>"):
        """Send a token and wait until the log is exactly prefix+token.
        Proves nothing else reached the app before the token."""
        child.send(token)
        self.wait_log(log, expected_prefix + token)
        return expected_prefix + token


class InputTransparency(AttachCase):
    def test_all_bytes_and_escape_sequences(self):
        log = self.inlog()
        c = self.px.attach("app")
        expected = b""
        for part in input_corpus_chunks():
            c.send(part)
            expected += part
        self.wait_log(log, expected)
        self.assertTrue(c.isalive(), "client must still be attached")
        self.detach(c, "app")


class OutputTransparency(AttachCase):
    def corpus(self):
        rnd = random.Random(0x5EED)
        parts = [
            bytes(range(256)) * 8,
            b"plain\nbare-LF\rCR\r\nCRLF\ttab\x07bell\x08bs\n",
            b"\x1b[38;2;255;100;0mtruecolor fg\x1b[48;2;0;30;60m bg\x1b[0m\n",
            b"\x1b[38;5;208m256\x1b[1;3;4;9mstyles\x1b[m\n",
            b"\x1b]8;;https://example.com/path?q=1\x1b\\link\x1b]8;;\x1b\\\n",
            b"\x1b]8;id=x;file:///tmp\x07bel-terminated\x1b]8;;\x07\n",
            b"\x1b[?1049h\x1b[H\x1b[2Jalt screen\x1b[?1049l",
            b"\x1b]0;window title\x07\x1b[?25l\x1b[?25h",
            "unicode ✻ ● ◌ ○ 😀 é\n".encode(),
            rnd.randbytes(1 << 20),
            bytes(range(256)),
        ]
        data = b"".join(parts)
        for m in (probe.START_MARKER, probe.END_MARKER):
            self.assertNotIn(m, data, "corpus accidentally contains a marker")
        return data

    def test_output_is_byte_exact(self):
        corpus = self.corpus()
        cpath = self.px.path("corpus.bin")
        with open(cpath, "wb") as f:
            f.write(corpus)
        self.px.create_probe("out", "emit", cpath)
        c = self.px.attach("out", rows=30, cols=100)
        c.send(probe.START_BYTE)
        data = read_until(c, probe.END_MARKER, timeout=60)
        start = data.rfind(probe.START_MARKER)
        self.assertGreaterEqual(start, 0, "START marker missing from client output")
        got = data[start + len(probe.START_MARKER):-len(probe.END_MARKER)]
        self.assertBytesEqual(corpus, got, "client output between markers")
        self.detach(c, "out")


class DetachKeys(AttachCase):
    def _detach_with(self, key):
        log = self.inlog()
        c = self.px.attach("app")
        seen = self.sync(c, log, b"", b"before")
        self.detach(c, "app", key=key)
        self.assertEqual(self.px.entry("app")["state"], "running")
        c = self.px.attach("app")
        self.sync(c, log, seen, b"after")  # no detach bytes reached the app
        self.detach(c, "app")

    def test_ctrl_backslash_byte(self):
        self._detach_with(DETACH)

    def test_kitty_csi_u(self):
        self._detach_with(KITTY_DETACH)

    def test_kitty_csi_u_press_event(self):
        self._detach_with(KITTY_DETACH_PRESS)

    def test_split_kitty_sequence(self):
        log = self.inlog()
        c = self.px.attach("app")
        # pexpect's default 50 ms delaybeforesend would exceed the hold-back.
        c.delaybeforesend = None
        seen = self.sync(c, log, b"", b"pre")
        c.send(KITTY_DETACH[:4])      # ESC [ 9 2
        # Unavoidable tiny sleep: force two separate reads by the client,
        # well inside the contract's 20 ms hold-back window.
        time.sleep(0.002)
        c.send(KITTY_DETACH[4:])      # ; 5 u
        status, sig, out = expect_exit(c, b"[detached from app]")
        self.assertEqual((status, sig), (0, None))
        c = self.px.attach("app")
        self.sync(c, log, seen, b"post")
        self.detach(c, "app")

    def test_release_event_swallowed(self):
        log = self.inlog()
        c = self.px.attach("app")
        c.send(b"a" + KITTY_RELEASE + b"b")
        self.wait_log(log, b"ab")
        self.assertTrue(c.isalive(), "release event must not detach")
        self.sync(c, log, b"ab")
        self.detach(c, "app")

    def test_lookalikes_pass_verbatim(self):
        log = self.inlog()
        c = self.px.attach("app")
        payload = b"\x1b[93;5u" + b"\x1b[92;6u" + b"\x1b[92;3u" + b"\x1b[92u" + b"\x1b[92;5"
        # The last one is an unterminated prefix; complete it into a
        # non-matching sequence so it must be forwarded as-is.
        payload += b"~" + b"\x1b\\" + b"x"
        c.send(payload)
        self.wait_log(log, payload)
        self.assertTrue(c.isalive(), "look-alikes must not detach")
        self.detach(c, "app")

    def test_detach_mid_chunk(self):
        log = self.inlog()
        c = self.px.attach("app")
        c.send(b"abc" + DETACH + b"def")
        status, sig, out = expect_exit(c, b"[detached from app]")
        self.assertEqual((status, sig), (0, None))
        c = self.px.attach("app")
        self.sync(c, log, b"abc", b"|next")
        self.detach(c, "app")


class DetachInvisible(AttachCase):
    def test_no_sighup_across_detach_cycles(self):
        log = self.px.path("sig.log")
        self.px.create_probe("app", "signals", log)
        cycles = 3
        for i in range(1, cycles + 1):
            c = self.px.attach("app")
            wait_until(lambda: read_file(log).split().count(b"WINCH") >= i,
                       msg=lambda: "attach #%d: no SIGWINCH; log=%r" % (i, read_file(log)))
            self.detach(c, "app")
        # One more attach: its SIGWINCH is logged only after anything the
        # previous detach might have sent, so the log is now complete.
        c = self.px.attach("app")
        wait_until(lambda: read_file(log).split().count(b"WINCH") >= cycles + 1,
                   msg=lambda: "final attach: no SIGWINCH; log=%r" % read_file(log))
        self.detach(c, "app")
        sigs = read_file(log).split()
        for bad in (b"HUP", b"INT", b"QUIT", b"TERM"):
            self.assertNotIn(bad, sigs, "process received SIG%s; log=%r" % (bad.decode(), sigs))
        self.assertEqual(self.px.entry("app")["state"], "running")


class Resize(AttachCase):
    def lines(self, log):
        return read_file(log).decode().splitlines()

    def test_initial_size_and_resize(self):
        log = self.px.path("ws.log")
        self.px.create_probe("app", "winsize", log)
        c = self.px.attach("app", rows=30, cols=100)
        wait_until(lambda: "30 100" in self.lines(log),
                   msg=lambda: "no SIGWINCH with 30x100 on attach; log=%r" % self.lines(log))
        c.setwinsize(40, 120)
        wait_until(lambda: "40 120" in self.lines(log),
                   msg=lambda: "resize to 40x120 not propagated; log=%r" % self.lines(log))
        self.detach(c, "app")
        # Reattach at the unchanged size: still a SIGWINCH.
        c = self.px.attach("app", rows=40, cols=120)
        wait_until(lambda: self.lines(log).count("40 120") >= 2,
                   msg=lambda: "no SIGWINCH on same-size reattach; log=%r" % self.lines(log))
        self.detach(c, "app")


class ExitWhileAttached(AttachCase):
    def test_exit_code(self):
        self.px.create_probe("app", "exit", "7")
        c = self.px.attach("app")
        c.send(b"x")
        status, sig, out = expect_exit(c, b"[app exited: 7]")
        self.assertEqual((status, sig), (0, None), out)
        self.assertEqual(self.px.entry("app")["state"], "exited:7")

    def test_killed_by_signal(self):
        self.px.create_probe("app", "exit", "sig15")
        c = self.px.attach("app")
        c.send(b"x")
        status, sig, out = expect_exit(c, None)
        self.assertEqual((status, sig), (0, None), out)
        self.assertRegex(out, rb"\[app (exited: )?killed by signal 15\]")
        self.assertEqual(self.px.entry("app")["state"], "signaled:15")

    def test_attach_to_exited_fails(self):
        self.px.create_probe("app", "exit", "0", "0")
        self.px.wait_state("app", lambda s: s == "exited:0")
        p = self.px.run("-a", "app")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn(b"has exited", p.stdout + p.stderr)


class Steal(AttachCase):
    def test_second_client_steals(self):
        log = self.inlog()
        a = self.px.attach("app")
        seen = self.sync(a, log, b"", b"from-a")
        b = self.px.attach("app")
        status, sig, out = expect_exit(a, b"[detached from app: attached elsewhere]")
        self.assertEqual((status, sig), (0, None), out)
        self.assertTrue(b.isalive())
        self.sync(b, log, seen, b"from-b")
        self.detach(b, "app")


class Environment(AttachCase):
    def test_child_env_is_client_env(self):
        # The daemon was started (in setUp) with the default env; the new
        # process must get the *creating client's* env, not the daemon's.
        env = {
            "PATH": self.px.env["PATH"],
            "HOME": self.px.home,
            "XDG_RUNTIME_DIR": self.px.runtime,
            "SHELL": "/bin/sh",
            "TERM": "xterm-kitty",
            "COLORTERM": "truecolor",
            "PWD": self.px.work,
            "LANG": "C.UTF-8",  # avoids Python's PEP 538 LC_CTYPE coercion in the probe
            "PMUX_TEST_ONLY_IN_CLIENT": "va lue=with\tspecialé",
            "EMPTY_VAR": "",
        }
        out = self.px.path("env.json")
        self.px.create_probe("envp", "env", out, env=env)
        wait_until(lambda: os.path.exists(out))
        got = load_json(out)
        self.assertEqual(got, env, "child environment must equal the creating client's")


class NewAttaches(AttachCase):
    def test_new_without_d_attaches(self):
        log = self.px.path("app.in")
        ready = self.px.path("app.ready")
        c = self.px.spawn_attach(["-n", "app", "--", *self.px.probe_cmd("inlog", log, ready=ready)])
        self.px.wait_raw(c)
        self.px.wait_ready(ready, "app")
        self.sync(c, log, b"", b"hello")
        self.detach(c, "app")
        e = self.px.entry("app")
        self.assertEqual(e["state"], "running")
        self.assertEqual(e["command"], " ".join(self.px.probe_cmd("inlog", log, ready=ready)))


if __name__ == "__main__":
    unittest.main()
