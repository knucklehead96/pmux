"""Milestone 3: libvterm screen / scrollback / mode restore.

Two kinds of outer terminal are used:

* tmux (tests/tui.py) where the *resulting* terminal state matters: the
  screen with SGR attributes, cursor position, native scrollback history
  and tmux's mode flags.  `pmux -a NAME` runs in a fresh tmux pane per
  attach.  The oracle is a *reference pane*: the same probe script run
  directly in tmux at the same size, without pmux; after an attach the
  pmux pane must be indistinguishable from it.
* pexpect (raw bytes) where the exact bytes matter: snapshot prologue,
  modes tmux 3.4 has no format flag for (focus 1004, bracketed paste 2004,
  DECSCUSR, kitty keyboard flags, mouse encodings), OSC 4/10/11/12 color
  sets and resets, titles, the absence of RIS, and query replies.

The probe's `draw` mode (tests/probe.py) writes a script once and then
never writes anything in response to input, so after an attach every byte
on the screen comes from pmux's snapshot.  pexpect tests make the probe
print WINCH_MARK on SIGWINCH: pmux signals the app on every attach, and the
app's reaction can only reach the client after the snapshot, so the bytes
before WINCH_MARK are exactly the snapshot.
"""
import os
import re
import shutil
import signal
import sys
import time
import unittest

from helpers import (DETACH, PMUX_BIN, PROBE, PYTHON, TIMEOUT, PmuxEnv, expect_exit, hexdump,
                     pid_alive, read_file, read_until, wait_until)
from tui import FOOTER_LIST, TmuxTui, TuiCase, dump, list_rows

# DCS string: ignored by tmux and libvterm (an APC would set tmux's pane title).
WINCH_MARK = b"\x1bPpmuxprobe-winch-7d1e\x1b\\"

# tmux format flags compared between a pmux pane and its reference pane.
MODE_FLAGS = ("alternate_on", "keypad_cursor_flag", "keypad_flag", "mouse_standard_flag",
              "mouse_button_flag", "mouse_all_flag", "mouse_any_flag", "mouse_sgr_flag",
              "mouse_utf8_flag", "cursor_flag", "pane_fg", "pane_bg")

RIS = b"\x1bc"
ST_OR_BEL = rb"(?:\x1b\\|\x07)"

# ---------------------------------------------------------------------------
# scripts


def numbered(fmt, first, last):
    return b"".join(fmt % i + b"\r\n" for i in range(first, last + 1))


ATTR_SCREEN = (
    b"\x1b[H\x1b[2J"
    b"\x1b[3;5H\x1b[1;31mBOLD-RED\x1b[0m"
    b"\x1b[5;10H\x1b[3;4;38;2;10;200;30mITALIC-UL-RGB\x1b[0m"
    b"\x1b[7;1H\x1b[7;48;5;202mREVERSE-BG202\x1b[0m"
    b"\x1b[9;20H\x1b[9;93;44mSTRIKE-BRIGHT\x1b[0m plain"
    b"\x1b[11;2H\x1b[48;2;40;50;60m   \x1b[0m<-rgb bg cells"
    b"\x1b[12;40Hplain text at 12;40"
    b"\x1b[24;1Hbottom-left\x1b[24;60Hbottom-right"
    b"\x1b[15;33H"                                     # cursor parked at (32, 14)
)

MODE_SCRIPT = (b"\x1b[H\x1b[2Jmodes set"
               b"\x1b[?1h\x1b=\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[?2004h\x1b[?25l\x1b[5 q"
               b"\x1b[>1u")


def cells_text(cells):
    return ["".join(c.ch for c in row).rstrip() for row in cells]


def norm_row(row):
    """Row of Cell.key() tuples without trailing default blanks (tmux may
    or may not report them)."""
    keys = [c.key() for c in row]
    blank = (" ", None, None, False, False, False, False, False, False)
    while keys and keys[-1] == blank:
        keys.pop()
    return keys


def pane_state(t, flags=MODE_FLAGS):
    cells = t.cells()
    return {"text": cells_text(cells), "cells": [norm_row(r) for r in cells],
            "cursor": t.cursor(), "flags": t.fmt(*flags)}


def describe_state_diff(expected, actual):
    out = []
    if expected["cursor"] != actual["cursor"]:
        out.append("cursor: expected %r, got %r" % (expected["cursor"], actual["cursor"]))
    for k in expected["flags"]:
        if expected["flags"][k] != actual["flags"].get(k):
            out.append("flag %s: expected %r, got %r" % (k, expected["flags"][k], actual["flags"].get(k)))
    for i, (e, a) in enumerate(zip(expected["cells"], actual["cells"])):
        if e != a:
            col = next((j for j in range(min(len(e), len(a))) if e[j] != a[j]), min(len(e), len(a)))
            out.append("row %d differs at col %d:\n   expected %r\n   actual   %r"
                       % (i, col, e[col:col + 3], a[col:col + 3]))
    out.append(dump(expected["text"], "expected (reference pane)"))
    out.append(dump(actual["text"], "actual (pmux pane)"))
    return "\n".join(out)


def private_modes(data):
    """Final h/l state of every DEC private mode set in data."""
    state = {}
    for m in re.finditer(rb"\x1b\[\?([0-9;]+)([hl])", data):
        for mode in m.group(1).split(b";"):
            if mode:
                state[int(mode)] = m.group(2) == b"h"
    return state


def nums(lines, word):
    rx = re.compile(r"%s (\d+)" % re.escape(word))
    return [int(m.group(1)) for l in lines for m in rx.finditer(l)]


def range_summary(ns):
    if not ns:
        return "none"
    return "%d numbers, first %r, last %r, head %r, tail %r" % (len(ns), ns[0], ns[-1], ns[:5], ns[-5:])


def signal_and_wait(pid, done_path, count):
    """SIGUSR1 the probe and wait until it has written its usr1 script."""
    os.kill(pid, signal.SIGUSR1)
    wait_until(lambda: read_file(done_path).count(b"\n") >= count,
               msg="probe did not handle SIGUSR1 #%d" % count)


# ---------------------------------------------------------------------------
# base class


class RestoreCase(TuiCase):
    """TuiCase (private HOME, theme = dark config, daemon started outside
    tmux) + helpers for draw probes, reference panes and attaches."""

    CONFIG = "theme = dark\n"

    def setUp(self):
        super().setUp()
        self._files = 0

    def file(self, data, stem="f"):
        self._files += 1
        path = self.px.path("%s.%d" % (stem, self._files))
        with open(path, "wb") as f:
            f.write(data)
        return path

    def draw_args(self, name, script, winch=None, usr1=None, inlog=False, exit=None):
        args = [self.file(script, name + ".script"), "wlog=" + self.px.path(name + ".wlog")]
        if winch is not None:
            args.append("winch=" + self.file(winch, name + ".winch"))
        if usr1 is not None:
            args.append("usr1=" + self.file(usr1, name + ".usr1"))
        if inlog:
            args.append("inlog=" + self.px.path(name + ".in"))
        if exit is not None:
            args.append("exit=%d" % exit)
        return args

    def draw(self, name, script, **kw):
        """Create a detached pmux process running `probe draw`; return its
        pid.  Files: <name>.wlog (one line per SIGWINCH), <name>.in
        (input log, with inlog=True)."""
        self.px.create_probe(name, "draw", *self.draw_args(name, script, **kw), cwd=self.api)
        return int(read_file(self.px.path(name + ".ready")))

    def usr1_done(self, name):
        return self.px.path(name + ".usr1.%d.done" % self._usr1_index(name))

    def _usr1_index(self, name):
        for f in os.listdir(self.px.files):
            m = re.match(re.escape(name) + r"\.usr1\.(\d+)$", f)
            if m:
                return int(m.group(1))
        raise AssertionError("no usr1 script for %s" % name)

    def winches(self, name):
        return read_file(self.px.path(name + ".wlog")).decode().splitlines()

    # -- tmux panes ---------------------------------------------------------

    def pane(self, cmd, rows=24, cols=80):
        t = TmuxTui(self.px, cmd=cmd, rows=rows, cols=cols, cwd=self.api)
        self.addCleanup(t.close)
        return t.start()

    def reference(self, script, rows=24, cols=80, flags=MODE_FLAGS):
        """Run the script directly in tmux (no pmux); return its settled
        pane state."""
        self._files += 1
        ready = self.px.path("ref.%d.ready" % self._files)
        t = self.pane([PYTHON, PROBE, "--ready", ready, "draw", self.file(script, "ref")],
                      rows, cols)
        self.px.wait_ready(ready, "reference probe")
        prev = {}

        def settled():
            cur = pane_state(t, flags)
            ok = cur == prev.get("s")
            prev["s"] = cur
            return ok
        wait_until(settled, interval=0.1, msg="reference pane never settled")
        return prev["s"]

    def attach_pane(self, name, rows=24, cols=80):
        """Run `pmux -a NAME` in a fresh tmux pane; wait for the attach
        (the probe logs the SIGWINCH pmux sends on attach)."""
        before = len(self.winches(name))
        t = self.pane([PMUX_BIN, "-a", name], rows, cols)
        wait_until(lambda: len(self.winches(name)) > before or t.pane_dead(),
                   msg=lambda: "attach to %s: no SIGWINCH\n%s" % (name, t.describe()))
        if t.pane_dead():
            self.fail("pmux -a %s exited instead of attaching\n%s" % (name, t.describe()))
        return t

    def detach_pane(self, t, name):
        t.keys("C-\\")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 0, "pmux -a exit status after detach\n" + t.describe())
        text = "\n".join(t.history())
        self.assertIn("[detached from %s]" % name, text, t.describe())

    def assertPaneMatches(self, t, expected, what, flags=MODE_FLAGS, timeout=TIMEOUT):
        last = {}

        def check():
            last["s"] = pane_state(t, flags)
            return last["s"] == expected
        try:
            wait_until(check, timeout, interval=0.1)
        except AssertionError:
            self.fail("%s: pane never matched the reference within %.0fs\n%s"
                      % (what, timeout, describe_state_diff(expected, last["s"])))

    # -- raw (pexpect) attaches ---------------------------------------------

    def attach_raw(self, name, rows=24, cols=80):
        """pmux -a under pexpect; returns (child, snapshot bytes, seconds
        from spawn to the end of the snapshot).  The probe must have been
        created with winch=WINCH_MARK."""
        t0 = time.monotonic()
        c = self.px.spawn_attach(["-a", name], rows, cols)
        data = read_until(c, WINCH_MARK)
        elapsed = time.monotonic() - t0
        return c, data[:-len(WINCH_MARK)], elapsed

    def detach_raw(self, c, name):
        """Detach; return every byte the client wrote after the snapshot
        (its detach output), minus any further WINCH_MARKs."""
        c.send(DETACH)
        status, sig, out = expect_exit(c, b"[detached from %s]" % name.encode())
        self.assertEqual((status, sig), (0, None), "client exit after detach; output=%r" % out)
        return out.replace(WINCH_MARK, b"")

    def assertNoRis(self, data, what):
        idx = data.find(RIS)
        if idx >= 0:
            self.fail("%s contains RIS (ESC c) at offset %d:\n%s"
                      % (what, idx, hexdump(data[max(0, idx - 48):idx + 16], max(0, idx - 48))))

    def assertBytesIn(self, pattern, data, what):
        if not re.search(pattern, data):
            self.fail("%s: %r not found in %d bytes:\n%s"
                      % (what, pattern, len(data), hexdump(data[-1024:], max(0, len(data) - 1024))))


# ===========================================================================
# harness self-test (no pmux)


class RestoreHarnessSelfTest(unittest.TestCase):
    """Validates the draw probe, the reference-pane oracle and the new tui.py
    helpers without pmux."""

    def setUp(self):
        self.px = PmuxEnv()
        self.addCleanup(self.px.close)
        self.n = 0

    def run_probe(self, script, *opts, rows=24, cols=80):
        self.n += 1
        spath = self.px.path("s%d" % self.n)
        with open(spath, "wb") as f:
            f.write(script)
        ready = self.px.path("r%d" % self.n)
        t = TmuxTui(self.px, cmd=[PYTHON, PROBE, "--ready", ready, "draw", spath, *opts],
                    rows=rows, cols=cols)
        self.addCleanup(t.close)
        t.start()
        self.px.wait_ready(ready)
        return t

    def test_attr_screen_as_seen_by_tmux(self):
        t = self.run_probe(ATTR_SCREEN)
        t.wait_for(lambda l: l[23].endswith("bottom-right"))
        wait_until(lambda: t.cursor() == (32, 14), msg=lambda: t.cursor())
        cells = t.cells()
        text = cells_text(cells)
        self.assertEqual(text[2], "    BOLD-RED", dump(text))
        c = cells[2][4]
        self.assertTrue(c.bold)
        self.assertEqual(c.fg, ("idx", 1))
        c = cells[4][9]
        self.assertEqual((c.ch, c.italic, c.underline, c.fg), ("I", True, True, (10, 200, 30)))
        c = cells[6][0]
        self.assertEqual((c.ch, c.reverse, c.bg), ("R", True, ("idx", 202)))
        c = cells[8][19]
        self.assertEqual((c.ch, c.strike, c.fg, c.bg), ("S", True, ("idx", 11), ("idx", 4)))
        self.assertEqual(cells[10][1].bg, (40, 50, 60))
        f = t.fmt(*MODE_FLAGS)
        self.assertEqual((f["alternate_on"], f["cursor_flag"], f["pane_bg"]), ("0", "1", "default"))

    def test_modes_as_seen_by_tmux(self):
        t = self.run_probe(MODE_SCRIPT + b"\x1b]11;rgb:1111/2222/3333\x1b\\\x1b]2;tt\x07")
        t.wait_for("modes set")
        exp = {"keypad_cursor_flag": "1", "keypad_flag": "1", "mouse_button_flag": "1",
               "mouse_sgr_flag": "1", "cursor_flag": "0", "pane_bg": "#112233", "pane_title": "tt"}
        wait_until(lambda: t.fmt(*exp) == exp, msg=lambda: t.fmt(*exp))

    def test_repaint_from_capture_matches_reference(self):
        # What a snapshot does: repaint a screen from scratch with different
        # SGR encodings.  The pane-state oracle must see it as identical.
        ref = self.run_probe(ATTR_SCREEN)
        ref.wait_for(lambda l: l[23].endswith("bottom-right"))
        wait_until(lambda: ref.cursor() == (32, 14), msg=lambda: ref.cursor())
        expected = pane_state(ref)
        rows = ref.screen_ansi()
        repaint = b"\x1b[H\x1b[2J" + b"".join(
            b"\x1b[%d;1H\x1b[0m" % (i + 1) + r.encode() for i, r in enumerate(rows))
        repaint += b"\x1b[0m\x1b[15;33H"
        t = self.run_probe(repaint)
        wait_until(lambda: pane_state(t) == expected,
                   msg=lambda: describe_state_diff(expected, pane_state(t)))
        other = self.run_probe(ATTR_SCREEN.replace(b"BOLD", b"BALD"))
        other.wait_for("BALD-RED")
        self.assertNotEqual(pane_state(other), expected, "oracle must see a changed cell")

    def test_usr1_inlog_wlog(self):
        # tmux answers the CPR itself; proves the query plumbing of the probe.
        log, wlog = self.px.path("in"), self.px.path("wlog")
        usr1 = self.px.path("u")
        with open(usr1, "wb") as f:
            f.write(b"\x1b[6n")
        t = self.run_probe(b"\x1b[5;10H", "usr1=" + usr1, "inlog=" + log, "wlog=" + wlog)
        pid = int(read_file(self.px.path("r%d" % self.n)))
        os.kill(pid, signal.SIGUSR1)
        wait_until(lambda: read_file(log) == b"\x1b[5;10R", msg=lambda: read_file(log))
        self.assertEqual(read_file(usr1 + ".done"), b"1\n")
        t.resize(30, 100)
        wait_until(lambda: b"30 100" in read_file(wlog), msg=lambda: read_file(wlog))

    def test_history_capture(self):
        t = self.run_probe(numbered(b"line %04d", 1, 500))
        wait_until(lambda: nums(t.history(), "line") == list(range(1, 501)),
                   msg=lambda: range_summary(nums(t.history(), "line")))
        self.assertEqual(t.screen()[22], "line 0500")

    def test_exit_option(self):
        t = self.run_probe(b"bye", "exit=3")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 3)


# ===========================================================================
# 1, 2: screen restore


class ScreenRestore(RestoreCase):
    def test_screen_attrs_cursor_cli(self):
        ref = self.reference(ATTR_SCREEN)
        self.assertEqual(ref["cursor"], (32, 14), "reference sanity")
        self.draw("scr", ATTR_SCREEN)
        a = self.attach_pane("scr")
        self.assertPaneMatches(a, ref, "first attach")
        self.detach_pane(a, "scr")
        b = self.attach_pane("scr")
        self.assertPaneMatches(b, ref, "reattach")
        self.detach_pane(b, "scr")

    def test_screen_attrs_cursor_tui(self):
        ref = self.reference(ATTR_SCREEN)
        self.draw("scr", ATTR_SCREEN)
        t = self.start_list("scr", rows=24, cols=80)
        for i in (1, 2):
            n = len(self.winches("scr"))
            t.keys("Enter")
            wait_until(lambda: len(self.winches("scr")) > n, msg="TUI attach #%d" % i)
            self.assertPaneMatches(t, ref, "TUI attach #%d" % i)
            self.detach_to_list(t, "scr")

    def test_output_while_detached(self):
        self.draw("od", b"\x1b[H\x1b[2Jbefore-detach\r\n",
                  usr1=b"while-detached-1\r\n\x1b[32mwhile-detached-2\x1b[0m\r\n")
        pid = int(read_file(self.px.path("od.ready")))
        a = self.attach_pane("od")
        a.wait_for("before-detach")
        self.detach_pane(a, "od")
        signal_and_wait(pid, self.usr1_done("od"), 1)
        # The daemon must consume the output while detached; give it time so
        # that late processing cannot masquerade as live output.
        time.sleep(0.5)
        b = self.attach_pane("od")
        lines = b.wait_for(lambda l: l[:3] == ["before-detach", "while-detached-1", "while-detached-2"],
                           msg="output written while detached")
        self.assertEqual(b.cells()[2][0].fg, ("idx", 2), dump(lines))
        wait_until(lambda: b.cursor() == (0, 3), msg=lambda: "cursor %r" % (b.cursor(),))
        self.detach_pane(b, "od")


# ===========================================================================
# 3, 14: scrollback


class Scrollback(RestoreCase):
    def check_history(self, t, expected):
        wait_until(lambda: nums(t.history(), "line") == expected,
                   msg=lambda: "native scrollback + screen: expected %s; got %s\n%s"
                   % (range_summary(expected), range_summary(nums(t.history(), "line")),
                      t.describe()))

    def test_history_lands_in_native_scrollback(self):
        self.draw("sb", numbered(b"line %04d", 1, 500))
        t = self.attach_pane("sb")
        self.check_history(t, list(range(1, 501)))
        lines = t.screen()
        self.assertEqual(lines[:23], ["line %04d" % i for i in range(478, 501)], dump(lines))
        self.assertEqual(lines[23], "", dump(lines))
        self.assertEqual(t.cursor(), (0, 23))
        self.assertGreaterEqual(int(t.fmt("history_size")["history_size"]), 477)
        self.detach_pane(t, "sb")
        # a second, fresh pane gets the same history again
        t = self.attach_pane("sb")
        self.check_history(t, list(range(1, 501)))
        self.detach_pane(t, "sb")


class ScrollbackLimit(RestoreCase):
    CONFIG = "theme = dark\nscrollback_lines = 100\n"

    def test_only_last_100_history_lines(self):
        self.draw("sb", numbered(b"line %04d", 1, 500))
        t = self.attach_pane("sb")
        t.wait_for(lambda l: l[22] == "line 0500", msg="screen restored")
        state = {}

        def ok():
            ns = state["ns"] = nums(t.history(), "line")
            # 23 screen rows (478..500) + 100 history lines -> 378..500
            return ns and 373 <= ns[0] <= 383 and ns == list(range(ns[0], 501))
        wait_until(ok, msg=lambda: "expected ~378..500 (100 history + 23 screen lines), got %s"
                   % range_summary(state.get("ns")))
        self.detach_pane(t, "sb")


class ScrollbackZero(RestoreCase):
    CONFIG = "theme = dark\nscrollback_lines = 0\n"

    def test_no_history_replayed(self):
        self.draw("sb", numbered(b"line %04d", 1, 100))
        t = self.attach_pane("sb")
        lines = t.wait_for(lambda l: l[22] == "line 0100", msg="screen restored")
        self.assertEqual(lines[:23], ["line %04d" % i for i in range(78, 101)], dump(lines))
        ns = nums(t.history(), "line")
        self.assertEqual(ns, list(range(78, 101)), "no history expected: %s" % range_summary(ns))
        self.detach_pane(t, "sb")


class ScrollbackConfig(RestoreCase):
    CONFIG = None

    def warnings(self, value):
        self.px.write_config("theme = dark\nscrollback_lines = %s\n" % value)
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stderr.decode()

    def test_invalid_values_warn(self):
        for v in ("abc", "-1", "100001", "10k", "1.5"):
            with self.subTest(value=v):
                self.assertIn("pmux: ~/.pmux/config:2: invalid value for scrollback_lines: '%s'" % v,
                              self.warnings(v))

    def test_valid_values_silent(self):
        for v in ("0", "1", "10000", "100000"):
            with self.subTest(value=v):
                self.assertEqual(self.warnings(v), "")


# ===========================================================================
# 4: alt screen


class AltScreen(RestoreCase):
    SCRIPT = (numbered(b"main %02d", 1, 40)
              + b"\x1b[?1049h\x1b[H\x1b[2J\x1b[2;3H\x1b[1;35mALT-SCREEN-CONTENT\x1b[0m"
              + b"\x1b[10;10H\x1b[7m rev \x1b[0m\x1b[20;5H")

    def test_alt_screen_restored_in_tmux(self):
        ref = self.reference(self.SCRIPT)
        self.assertEqual(ref["flags"]["alternate_on"], "1", "reference sanity")
        self.draw("alt", self.SCRIPT)
        t = self.attach_pane("alt")
        self.assertPaneMatches(t, ref, "alt-screen attach")
        self.detach_pane(t, "alt")
        self.assertEqual(t.fmt("alternate_on")["alternate_on"], "0",
                         "alt screen must be left on detach\n" + t.describe())
        t = self.attach_pane("alt")
        self.assertPaneMatches(t, ref, "alt-screen reattach")
        self.detach_pane(t, "alt")

    def test_history_flows_before_alt_screen(self):
        self.draw("alt", self.SCRIPT, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("alt")
        self.detach_raw(c, "alt")
        on = snap.rfind(b"\x1b[?1049h")
        self.assertGreater(on, 0, "snapshot never enters the alt screen:\n%s" % hexdump(snap[-512:]))
        before, after = snap[:on], snap[on:]
        # 40 lines on 24 rows: main 01..17 are history, 18..40 the main screen
        seen = nums([before.decode(errors="replace")], "main")
        for i in range(1, 18):
            self.assertIn(i, seen, "history line 'main %02d' must flow before ?1049h; saw %r" % (i, seen))
        self.assertIn(b"ALT-SCREEN-CONTENT", after)
        self.assertNotIn(b"ALT-SCREEN-CONTENT", before)
        self.assertNotIn(b"main ", after, "main-screen text painted into the alt screen")


# ===========================================================================
# 5: modes


class Modes(RestoreCase):
    def test_modes_restored_in_tmux(self):
        ref = self.reference(MODE_SCRIPT)
        expected = {"keypad_cursor_flag": "1", "keypad_flag": "1", "mouse_button_flag": "1",
                    "mouse_sgr_flag": "1", "cursor_flag": "0", "alternate_on": "0"}
        self.assertEqual({k: ref["flags"][k] for k in expected}, expected, "reference sanity")
        self.draw("md", MODE_SCRIPT)
        t = self.attach_pane("md")
        self.assertPaneMatches(t, ref, "modes after attach")
        self.detach_pane(t, "md")
        f = t.fmt(*MODE_FLAGS)
        for k in ("mouse_standard_flag", "mouse_button_flag", "mouse_all_flag", "mouse_any_flag",
                  "mouse_sgr_flag", "mouse_utf8_flag", "alternate_on"):
            self.assertEqual(f[k], "0", "%s still set after detach: %r" % (k, f))
        # (cursor visibility is checked on the raw bytes: tmux hides the
        # cursor of a dead pane, so cursor_flag is always 0 here)

    def test_detach_resets_cursor_key_and_keypad_modes(self):
        # The task contract says "after detach all flags are off"; SPEC's
        # detach list does not name DECCKM / DECKPAM explicitly.
        self.draw("md", MODE_SCRIPT)
        t = self.attach_pane("md")
        wait_until(lambda: t.fmt("keypad_flag")["keypad_flag"] == "1", msg="keypad restored")
        self.detach_pane(t, "md")
        f = t.fmt("keypad_cursor_flag", "keypad_flag")
        self.assertEqual(f, {"keypad_cursor_flag": "0", "keypad_flag": "0"},
                         "application cursor keys / keypad left on after detach")

    def check_raw(self, name, script, want_modes, extra=()):
        label = b"MODES-" + name.encode()
        self.draw(name, b"\x1b[H\x1b[2J" + script + label, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw(name)
        out = self.detach_raw(c, name)
        self.assertNoRis(snap, "snapshot")
        self.assertNoRis(out, "detach output")
        text_at = snap.find(label)
        self.assertGreater(text_at, 0, "screen text missing from snapshot")
        restore = snap[text_at:]
        state = private_modes(snap)
        for mode, on in want_modes.items():
            self.assertEqual(state.get(mode), on, "?%d: final state %r in snapshot, expected %r\n%s"
                             % (mode, state.get(mode), on, hexdump(snap[-512:])))
        for pattern in extra:
            self.assertBytesIn(pattern, restore, "snapshot restore section")
        after = private_modes(out)
        for mode, on in want_modes.items():
            if on and mode in (1000, 1002, 1003, 1004, 1005, 1006, 1015, 2004, 1049):
                self.assertIs(after.get(mode), False, "?%d not reset on detach:\n%s" % (mode, hexdump(out)))
        return snap, out

    def test_raw_mode_restore_set1(self):
        snap, out = self.check_raw(
            "m1", MODE_SCRIPT, {1: True, 1002: True, 1006: True, 1004: True, 2004: True, 25: False},
            extra=(rb"\x1b\[5 q", rb"\x1b=", rb"\x1b\[(?:>1|=1(?:;1)?)u"))
        # keypad: the last keypad sequence must be DECKPAM
        self.assertGreater(snap.rfind(b"\x1b="), snap.rfind(b"\x1b>"), "keypad left in normal mode")
        self.assertEqual(re.findall(rb"\x1b\[(\d*) q", snap)[-1], b"5", "last DECSCUSR")
        self.assertBytesIn(rb"\x1b\[<\d*u", out, "detach pops kitty keyboard flags")
        self.assertBytesIn(rb"\x1b\[\?25h", out, "detach shows the cursor")

    def test_raw_mode_restore_set2(self):
        self.check_raw("m2", b"\x1b[?1000h\x1b[?1005h\x1b[2 q",
                       {1000: True, 1005: True}, extra=(rb"\x1b\[2 q",))

    def test_raw_mode_restore_set3(self):
        self.check_raw("m3", b"\x1b[?1003h\x1b[?1015h\x1b[?1049h\x1b[Halt",
                       {1003: True, 1015: True, 1049: True})

    def test_raw_defaults_not_enabled(self):
        # An app that set nothing: the snapshot enables no mouse/paste/focus mode.
        snap, _ = self.check_raw("m4", b"", {})
        state = private_modes(snap)
        for mode in (1000, 1002, 1003, 1004, 1005, 1006, 1015, 1049, 2004):
            self.assertNotEqual(state.get(mode), True, "?%d enabled for an app that never set it" % mode)
        self.assertNotEqual(state.get(25), False, "cursor hidden for an app that never hid it")


# ===========================================================================
# 6, 7, 8: colors, title, prologue / no RIS


class ColorsTitle(RestoreCase):
    COLORS = (b"\x1b[H\x1b[2JCOLORS-SCREEN"
              b"\x1b]11;rgb:1111/2222/3333\x1b\\"
              b"\x1b]4;1;#ff0000\x1b\\")

    def test_color_sets_replayed_and_reset(self):
        self.draw("col", self.COLORS, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("col")
        out = self.detach_raw(c, "col")
        paint = snap.find(b"COLORS-SCREEN")
        self.assertGreaterEqual(paint, 0, "screen text missing:\n" + hexdump(snap[-512:]))
        restore = snap[paint:]
        self.assertBytesIn(rb"\x1b\]11;rgb:1111/2222/3333" + ST_OR_BEL, restore,
                           "OSC 11 replayed after the screen paint")
        self.assertBytesIn(rb"\x1b\]4;1;#ff0000" + ST_OR_BEL, restore,
                           "OSC 4 replayed after the screen paint")
        self.assertNoRis(snap, "snapshot")
        self.assertNoRis(out, "detach output")
        self.assertBytesIn(rb"\x1b\]111" + ST_OR_BEL, out, "detach resets OSC 11")
        self.assertBytesIn(rb"\x1b\]104;1" + ST_OR_BEL, out, "detach resets palette index 1")
        for bad in (rb"\x1b\]110", rb"\x1b\]112", rb"\x1b\]104" + ST_OR_BEL, rb"\x1b\]104;(?!1\b)\d"):
            self.assertIsNone(re.search(bad, out), "unexpected color reset %r in detach output:\n%s"
                              % (bad, hexdump(out)))
        # the resets come after the mode resets
        first_osc = min(m.start() for m in re.finditer(rb"\x1b\](?:111|104)", out))
        last_mode = max([m.start() for m in re.finditer(rb"\x1b\[\?[0-9;]+[hl]", out)] or [-1])
        self.assertGreater(first_osc, last_mode, "OSC resets must follow the mode resets:\n%s" % hexdump(out))

    def test_all_color_kinds_reset(self):
        script = (b"x\x1b]10;#aabbcc\x07\x1b]11;#112233\x07\x1b]12;#00ff00\x07"
                  b"\x1b]4;3;#010203\x07\x1b]4;200;rgb:aa/bb/cc\x07")
        self.draw("col", script, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("col")
        out = self.detach_raw(c, "col")
        for p in (rb"\x1b\]10;#aabbcc", rb"\x1b\]11;#112233", rb"\x1b\]12;#00ff00",
                  rb"\x1b\]4;3;#010203", rb"\x1b\]4;200;rgb:aa/bb/cc"):
            self.assertBytesIn(p + ST_OR_BEL, snap, "color set replayed")
        for p in (rb"\x1b\]110", rb"\x1b\]111", rb"\x1b\]112", rb"\x1b\]104;3\b", rb"\x1b\]104;200\b"):
            self.assertBytesIn(p, out, "color reset on detach")

    def test_reset_color_is_not_reset_again(self):
        script = (b"RESET-COLORS"
                  b"\x1b]11;rgb:1111/2222/3333\x1b\\\x1b]111\x1b\\"
                  b"\x1b]4;1;#ff0000\x07\x1b]104;1\x07")
        self.draw("col", script, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("col")
        out = self.detach_raw(c, "col")
        for set_rx, reset_rx in ((rb"\x1b\]11;rgb:1111", rb"\x1b\]111"),
                                 (rb"\x1b\]4;1;#ff0000", rb"\x1b\]104;1")):
            s = [m.start() for m in re.finditer(set_rx, snap)]
            if s:  # a replayed set must be followed by its reset
                r = [m.start() for m in re.finditer(reset_rx, snap)]
                self.assertTrue(r and max(r) > max(s), "snapshot leaves %r applied:\n%s"
                                % (set_rx, hexdump(snap[-512:])))
            self.assertIsNone(re.search(reset_rx, out), "detach resets %r although the app reset it:\n%s"
                              % (reset_rx, hexdump(out)))

    def test_colors_in_tmux(self):
        base = self.reference(b"x")["flags"]
        self.draw("col", self.COLORS + b"\x1b]10;rgb:aaaa/bbbb/cccc\x1b\\")
        t = self.attach_pane("col")
        wait_until(lambda: t.fmt("pane_bg", "pane_fg") == {"pane_bg": "#112233", "pane_fg": "#aabbcc"},
                   msg=lambda: "colors after attach: %r" % t.fmt("pane_bg", "pane_fg"))
        self.detach_pane(t, "col")
        self.assertEqual(t.fmt("pane_bg", "pane_fg"),
                         {"pane_bg": base["pane_bg"], "pane_fg": base["pane_fg"]},
                         "colors not reset on detach")

    def test_title_restored_not_reset(self):
        self.draw("ttl", b"TITLE-SCREEN\x1b]2;pmux-title-test\x1b\\", winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("ttl")
        out = self.detach_raw(c, "ttl")
        self.assertBytesIn(rb"\x1b\][02];pmux-title-test" + ST_OR_BEL, snap, "title in snapshot")
        self.assertIsNone(re.search(rb"\x1b\][012];", out), "detach touched the title:\n%s" % hexdump(out))
        # and in tmux: set after a fresh attach, kept after detach
        t = self.attach_pane("ttl")
        wait_until(lambda: t.fmt("pane_title")["pane_title"] == "pmux-title-test",
                   msg=lambda: "pane_title %r" % t.fmt("pane_title"))
        self.detach_pane(t, "ttl")
        self.assertEqual(t.fmt("pane_title")["pane_title"], "pmux-title-test")

    def test_snapshot_prologue_and_no_ris(self):
        self.draw("pro", numbered(b"line %02d", 1, 30) + b"\x1b[?1049h\x1b[?1000hALT", winch=WINCH_MARK)
        for i in (1, 2):
            c, snap, _ = self.attach_raw("pro")
            out = self.detach_raw(c, "pro")
            self.assertTrue(snap.startswith(b"\x1b[!p"),
                            "attach #%d: snapshot must start with DECSTR:\n%s" % (i, hexdump(snap[:64])))
            pos = 0
            for seq in (b"\x1b[!p", b"\x1b[0m", b"\x1b[H", b"\x1b[2J", b"\x1b[3J"):
                idx = snap.find(seq, pos)
                self.assertGreaterEqual(idx, 0, "attach #%d: %r missing from the prologue:\n%s"
                                        % (i, seq, hexdump(snap[:64])))
                pos = idx + len(seq)
            content = snap.find(b"line 01")
            self.assertGreater(content, pos, "attach #%d: history must follow the prologue" % i)
            self.assertNoRis(snap, "snapshot #%d" % i)
            self.assertNoRis(out, "detach output #%d" % i)


# ===========================================================================
# 9: query replies


class QueryReplies(RestoreCase):
    QUERY = b"\x1b[6n\x1b[c"
    CPR_RX = rb"\x1b\[(\d+);(\d+)R"
    DA_RX = rb"\x1b\[\?[0-9;]*c"

    def check_detached_reply(self, pid, log, count):
        signal_and_wait(pid, self.usr1_done("q"), count)
        want = lambda: (len(re.findall(self.CPR_RX, read_file(log))) >= count  # noqa: E731
                        and len(re.findall(self.DA_RX, read_file(log))) >= count)
        wait_until(want, timeout=1.0, msg=lambda: "no CPR+DA reply within 1 s while detached; log=%r"
                   % read_file(log))
        time.sleep(0.5)
        data = read_file(log)
        self.assertEqual(re.findall(self.CPR_RX, data), [(b"5", b"10")] * count,
                         "exactly one CPR per query, at the cursor (5;10); log=%r" % data)
        self.assertEqual(len(re.findall(self.DA_RX, data)), count, "one DA reply per query; log=%r" % data)
        rest = re.sub(self.DA_RX, b"", re.sub(self.CPR_RX, b"", data))
        self.assertEqual(rest, b"", "unexpected input to the app: %r" % data)

    def test_libvterm_answers_while_detached(self):
        pid = self.draw("q", b"\x1b[H\x1b[2Jquery\x1b[5;10H", usr1=self.QUERY, inlog=True, winch=WINCH_MARK)
        log = self.px.path("q.in")
        self.check_detached_reply(pid, log, 1)
        c, _snap, _ = self.attach_raw("q")
        self.detach_raw(c, "q")
        self.check_detached_reply(pid, log, 2)

    def test_only_the_terminal_answers_while_attached(self):
        pid = self.draw("q", b"\x1b[H\x1b[2Jquery\x1b[5;10H", usr1=self.QUERY, inlog=True, winch=WINCH_MARK)
        log = self.px.path("q.in")
        c, _snap, _ = self.attach_raw("q")
        os.kill(pid, signal.SIGUSR1)
        read_until(c, self.QUERY)
        reply = b"\x1b[7;9R\x1b[?62;22c"   # deliberately not the libvterm answer
        c.send(reply)
        wait_until(lambda: len(read_file(log)) >= len(reply), timeout=1.0,
                   msg=lambda: "terminal's reply not forwarded; log=%r" % read_file(log))
        time.sleep(0.5)
        self.assertEqual(read_file(log), reply,
                         "the app must get exactly the real terminal's reply (libvterm's discarded)")
        self.detach_raw(c, "q")


# ===========================================================================
# 10, 11: TUI bell and exited final screen


class TuiRestore(RestoreCase):
    def glyph(self, lines, name):
        for _i, g, n in list_rows(lines):
            if n == name:
                return g
        return None

    def attach_from_list(self, t, name):
        n = len(self.winches(name))
        t.keys("Enter")
        wait_until(lambda: len(self.winches(name)) > n,
                   msg=lambda: "TUI attach to %s\n%s" % (name, t.describe()))

    def test_bell_only_counts_while_detached(self):
        pid = self.draw("ring", b"\x1b[H\x1b[2Jringer", usr1=b"\x07[rang]\r\n")
        done = self.usr1_done("ring")
        t = self.start_list("ring")
        self.attach_from_list(t, "ring")
        signal_and_wait(pid, done, 1)
        t.wait_for("[rang]", msg="BEL output while attached")
        self.detach_to_list(t, "ring")
        # the list refreshes periodically: the row must stay bell-free
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            lines = t.wait_list("ring")
            self.assertNotEqual(self.glyph(lines, "ring"), "!",
                                "BEL received while attached set the bell flag\n" + dump(lines))
            time.sleep(0.1)
        signal_and_wait(pid, done, 2)
        t.wait_for(lambda l: self.glyph(l, "ring") == "!", msg="BEL while detached sets the flag")
        self.attach_from_list(t, "ring")
        self.detach_to_list(t, "ring")
        t.wait_for(lambda l: self.glyph(l, "ring") not in (None, "!"), msg="attach clears the bell")

    def test_exited_final_screen(self):
        script = (b"\x1b[H\x1b[2J\x1b[2;3H\x1b[1;32mFINAL-SCREEN-OK\x1b[0m"
                  b"\x1b[4;10Hlast words\x1b[6;1H")
        self.draw("fin", script, exit=3)
        self.px.wait_state("fin", lambda s: s == "exited:3")
        t = self.start_list("fin", rows=24, cols=80)
        t.keys("Enter")
        lines = t.wait_for(lambda l: l[1] == "  FINAL-SCREEN-OK", msg="final screen shown")
        self.assertFalse(lines[0].startswith(" ✻ pmux"), dump(lines))
        self.assertEqual(lines[3], " " * 9 + "last words", dump(lines))
        cell = t.cells()[1][2]
        self.assertTrue(cell.bold and cell.fg == ("idx", 2), "final screen colors: %r" % cell)
        time.sleep(0.3)   # read-only: nothing changes by itself
        self.assertEqual(t.screen(), lines)
        t.type("z")
        lines = t.wait_list("fin")
        self.assertFalse(any(l.lstrip().startswith("›") for l in lines),
                         "the key that closes the final screen must not start a filter\n" + dump(lines))
        t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="normal list footer")
        self.assertEqual(self.px.entry("fin")["state"], "exited:3")
        p = self.px.run("-a", "fin")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn(b"has exited", p.stdout + p.stderr)


# ===========================================================================
# 12: resize between attaches


class ResizeRestore(RestoreCase):
    def test_reattach_bigger(self):
        long_token = "".join(chr(ord("a") + i % 26) for i in range(150))
        script = (b"\x1b[H\x1b[2J" + b"".join(b"resize-line-%d\r\n" % i for i in range(1, 6))
                  + long_token.encode() + b"\r\nend")
        self.draw("rz", script)
        a = self.attach_pane("rz", rows=30, cols=100)
        a.wait_for(lambda l: l[:5] == ["resize-line-%d" % i for i in range(1, 6)], msg="30x100 attach")
        wait_until(lambda: "30 100" in self.winches("rz"), msg=lambda: self.winches("rz"))
        self.detach_pane(a, "rz")
        b = self.attach_pane("rz", rows=40, cols=120)
        wait_until(lambda: "40 120" in self.winches("rz"), msg=lambda: "probe winsize log %r" % self.winches("rz"))

        def content(lines):
            idx = [i for i, l in enumerate(lines) if l.startswith("resize-line-")]
            return (len(idx) == 5 and [lines[i] for i in idx] == ["resize-line-%d" % i for i in range(1, 6)]
                    and long_token in "".join(lines) and any(l.startswith("end") for l in lines))
        b.wait_for(content, msg="content after reattach at 40x120")
        self.assertFalse(b.pane_dead())
        self.assertTrue(pid_alive(self.px.daemon_pid()), "daemon died")
        self.assertEqual(self.px.entry("rz")["state"], "running")
        self.detach_pane(b, "rz")


# ===========================================================================
# 13: performance


class Performance(RestoreCase):
    def test_10k_history_snapshot_time(self):
        total = 10050
        self.draw("big", numbered(b"hist %05d", 1, total), winch=WINCH_MARK)
        times = []
        for i in (1, 2):
            c, snap, elapsed = self.attach_raw("big")
            self.detach_raw(c, "big")
            times.append(elapsed)
            ns = sorted(set(nums([snap.decode(errors="replace")], "hist")))
            # 23 screen lines + 10000 history lines -> 28..10050
            self.assertTrue(ns and 25 <= ns[0] <= 31 and ns == list(range(ns[0], total + 1)),
                            "attach #%d: expected hist ~28..%d, got %s" % (i, total, range_summary(ns)))
            sys.stderr.write("\n  [perf] attach #%d: %d-byte snapshot (10000 history lines) "
                             "complete in %.1f ms\n" % (i, len(snap), elapsed * 1000))
        self.assertLess(max(times), 1.0, "snapshot too slow: %r s" % times)


# ===========================================================================
# 15: a real application


@unittest.skipUnless(shutil.which("less"), "less not installed")
class RealApp(RestoreCase):
    def test_less(self):
        text = self.file(b"".join(b"less-line %03d\n" % i for i in range(1, 301)), "less.txt")
        p = self.px.run("-n", "lv", "-d", "--", "less", text, cwd=self.api)
        self.assertEqual(p.returncode, 0, p.stderr)
        a = self.pane([PMUX_BIN, "-a", "lv"])
        a.wait_for(lambda l: l[0] == "less-line 001", msg="less started")
        a.type("10j")
        lines = a.wait_for(lambda l: l[0] == "less-line 011", msg="less scrolled")
        time.sleep(0.3)
        lines = a.screen()
        alt = a.fmt("alternate_on")
        self.detach_pane(a, "lv")
        b = self.pane([PMUX_BIN, "-a", "lv"])
        b.wait_for(lambda l: l == lines, msg="same visible text after reattach")
        self.assertEqual(b.fmt("alternate_on"), alt)
        b.type("j")
        b.wait_for(lambda l: l[0] == "less-line 012", msg="less still responds after reattach")
        b.type("q")
        b.wait_dead()


if __name__ == "__main__":
    unittest.main()
