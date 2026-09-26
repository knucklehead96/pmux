"""Milestone 2: the list TUI (layout, colors, keys, dialogs, filter, mouse,
refresh, attach from the list, quit / terminal restore).

Most tests drive `pmux` inside a private tmux server (tests/tui.py) and read
the screen with capture-pane; the passthrough tests use pexpect directly so
that no terminal emulator sits between pmux and the test.
"""
import os
import re
import termios
import time
import unittest

import pexpect

from helpers import (DETACH, FAST, PROBE, PYTHON, TIMEOUT, PmuxEnv, drain, input_corpus_chunks,
                     pid_alive, probe, read_file, read_until, wait_until)
from tui import (AGE_RE, DARK, DARK_BG, DIALOG_HINT, FOOTER_EMPTY, FOOTER_FILTER, FOOTER_LIST, FOOTER_RENAME,
                 LIGHT, TEAL_ACCENT, TmuxTui, TuiCase, blend, color_close, dialog_complete, dump,
                 list_rows,
                 row_index, winch_marker)

W = 100
HOME_KEY = b"\x1b[H"   # xterm Home / End (normal cursor-key mode)
END_KEY = b"\x1b[F"


def row_re(glyph, name, detail, namew=12, width=W):
    """Exact row: 3 spaces, glyph, space, name padded to namew, 2 spaces,
    detail, spaces, age ending at column width-2."""
    return re.compile(r"^   %s %s  %s +%s$" % (re.escape(glyph), re.escape(name.ljust(namew)),
                                               re.escape(detail), AGE_RE))


def names(lines):
    return [n for _i, _g, n in list_rows(lines)]


def find_col(line, text):
    col = line.find(text)
    if col < 0:
        raise AssertionError("%r not found in line %r" % (text, line))
    return col


# ===========================================================================
# harness self-test: works without a pmux TUI


class HarnessSelfTest(unittest.TestCase):
    """Validates tests/tui.py itself against stand-in programs."""

    APP = r'''
import os, signal, sys, fcntl, termios, struct, tty
tty.setraw(0)
def size():
    r, c, _, _ = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8))
    return r, c
def draw(*_):
    r, c = size()
    os.write(1, b"\x1b[H\x1b[2Jsize %dx%d\r\n\r\n" % (r, c)
             + b"   \x1b[1;38;2;217;119;87mHELLO\x1b[0m \x1b[48;2;47;54;69mbar\x1b[0m"
             + b" \x1b[35mP\x1b[95mQ\x1b[38;5;4mR\x1b[44mS\x1b[0m \x1b[4mU\x1b[24m\r\n")
os.write(1, b"\x1b[?1049h\x1b[?1000h\x1b[?1006h")
signal.signal(signal.SIGWINCH, draw)
draw()
while True:
    b = os.read(0, 1)
    if b == b"q":
        os.write(1, b"\x1b[?1006l\x1b[?1000l\x1b[?1049l")
        sys.exit(0)
    if b == b"x":
        os._exit(3)
'''

    def setUp(self):
        self.px = PmuxEnv()
        self.addCleanup(self.px.close)
        self.app = self.px.path("app.py")
        with open(self.app, "w") as f:
            f.write(self.APP)

    def tmux(self, cmd, **kw):
        t = TmuxTui(self.px, cmd=cmd, **kw)
        self.addCleanup(t.close)
        return t.start()

    def test_raw_keys_and_type_are_delivered_verbatim(self):
        log, ready = self.px.path("in.log"), self.px.path("in.ready")
        t = self.tmux([PYTHON, PROBE, "--ready", ready, "inlog", log])
        self.px.wait_ready(ready)
        expected = bytes(range(256))
        t.raw(expected)
        t.keys("C-\\", "Enter", "Escape", "Tab", "C-n", "C-r", "C-x", "C-q", "C-c", "BSpace",
               "Up", "Down", "DC")
        expected += b"\x1c\r\x1b\t\x0e\x12\x18\x11\x03\x7f\x1b[A\x1b[B\x1b[3~"
        t.type("héllo -x ;")
        expected += "héllo -x ;".encode()
        t.click(4, 5)
        expected += b"\x1b[<0;5;6M\x1b[<0;5;6m"
        t.wheel(0, 0, down=True)
        expected += b"\x1b[<65;1;1M"
        wait_until(lambda: len(read_file(log)) >= len(expected), msg=lambda: read_file(log))
        self.assertEqual(read_file(log), expected)

    def test_screen_colors_modes_resize(self):
        t = self.tmux([PYTHON, self.app])
        lines = t.wait_for("size 30x100")
        self.assertEqual(len(lines), 30)
        self.assertEqual(lines[2], "   HELLO bar PQRS U")
        cells = t.cells()[2]
        self.assertEqual(cells[3].ch, "H")
        self.assertEqual(cells[3].fg, (217, 119, 87))
        self.assertTrue(cells[3].bold)
        self.assertEqual(cells[9].bg, (47, 54, 69))
        self.assertIsNone(cells[9].fg)
        self.assertEqual([c.fg for c in cells[13:16]], [("idx", 5), ("idx", 13), ("idx", 4)])
        self.assertEqual(cells[16].bg, ("idx", 4))
        self.assertTrue(cells[18].underline)
        self.assertFalse(cells[17].underline)
        self.assertEqual(t.modes(), ("1", "1"))
        t.resize(40, 120)
        lines = t.wait_for("size 40x120")
        self.assertEqual(len(lines), 40)
        t.type("q")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 0)
        self.assertEqual(t.modes(), ("0", "0"), "restored modes must read 0 0 on the dead pane")

    def test_dead_pane_keeps_unrestored_modes(self):
        # Proves that the "0 0" check used by the quit tests is meaningful.
        t = self.tmux([PYTHON, self.app])
        t.wait_for("size 30x100")
        t.type("x")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 3)
        self.assertEqual(t.modes(), ("1", "1"))

    def test_wait_for_reports_screen_on_failure(self):
        t = self.tmux([PYTHON, self.app])
        t.wait_for("size 30x100")
        with self.assertRaises(AssertionError) as cm:
            t.wait_for("never-shown", timeout=0.3)
        self.assertIn("size 30x100", str(cm.exception))
        self.assertIn("never-shown", str(cm.exception))

    def test_detached_tmux_does_not_answer_osc11(self):
        # pmux therefore uses its assumed background (#1E1E1E / #FAFAF7) for
        # the selection-bar blend in these tests.
        out = self.px.path("osc11.out")
        script = ("import os,select,time,tty;tty.setraw(0);os.write(1,b'\\x1b]11;?\\x1b\\\\');"
                  "r=b'';e=time.time()+1\nwhile time.time()<e:\n"
                  " if select.select([0],[],[],0.1)[0]: r+=os.read(0,100)\n"
                  "open(%r,'wb').write(r)" % out)
        t = self.tmux([PYTHON, "-c", script])
        t.wait_dead()
        self.assertEqual(read_file(out), b"", "tmux answered OSC 11; expected-bg assumption broken")
        self.assertEqual(DARK["sel"], blend((122, 162, 247), 0.18, DARK_BG))


# ===========================================================================
# layout


class Layout(TuiCase):
    def populate(self):
        self.zzz = os.path.join(self.px.root, "zzz")   # outside $HOME, sorts last by path
        os.makedirs(self.zzz)
        self.new_exited("ex3", "exit 3", cwd=self.web)
        self.new_exited("sig", "kill -TERM $$", cwd=self.web)
        self.new_exited("ok0", "exit 0", cwd=self.web)
        # Running ones last and quickly: they must still be "running" (<5 s).
        self.new("zed", "sleep", "1000", cwd=self.api)
        self.new("alpha", "cat", cwd=self.api)
        self.new("outside", "sleep", "1001", cwd=self.zzz)
        t = self.start_list("ex3", "sig", "ok0", "zed", "alpha", "outside")
        # The footer is drawn last: once it is there the frame is complete.
        t.wait_for(lambda l: re.match(r"^ ✻ pmux +3 running · 3 exited$", l[0])
                   and l[29] == FOOTER_LIST, msg="complete list frame")
        return t

    def test_groups_rows_header_footer(self):
        t = self.populate()
        lines = t.screen()
        d = dump(lines)
        self.assertRegex(lines[0], r"^ ✻ pmux +3 running · 3 exited$", d)
        self.assertEqual(len(lines[0]), W - 1, "counts must end at column W-2\n" + d)
        self.assertEqual(lines[1], "", d)
        self.assertEqual(lines[2], " ~/work/api · 2", d)
        self.assertRegex(lines[3], row_re("●", "zed", "sleep 1000"), d)
        self.assertRegex(lines[4], row_re("●", "alpha", "cat"), d)
        self.assertEqual(lines[5], "", d)
        self.assertEqual(lines[6], " ~/work/web · 3", d)
        self.assertRegex(lines[7], row_re("○", "ex3", "exited (3)"), d)
        self.assertRegex(lines[8], row_re("○", "sig", "killed (SIGTERM)"), d)
        self.assertRegex(lines[9], row_re("○", "ok0", "exited (0)"), d)
        self.assertEqual(lines[10], "", d)
        self.assertEqual(lines[11], " %s · 1" % self.zzz, d)
        self.assertRegex(lines[12], row_re("●", "outside", "sleep 1001"), d)
        for i in list(range(3, 5)) + list(range(7, 10)) + [12]:
            self.assertEqual(len(lines[i]), W - 1, "age must end at column W-2 (line %d)\n%s" % (i, d))
        self.assertEqual(lines[13:29], [""] * 16, d)
        self.assertEqual(lines[29], FOOTER_LIST, d)

    def test_dark_colors(self):
        t = self.populate()
        th = self.THEME
        cells = t.cells()
        lines = t.screen()
        d = dump(lines)
        self.assertEqual(cells[0][1].ch, "✻", d)
        self.assertColor(cells[0][1], th["accent"], "✻")
        for c in cells[0][3:7]:
            self.assertColor(c, th["accent"], "'pmux'")
            self.assertTrue(c.bold, "'pmux' must be bold: %r" % c)
        col = find_col(lines[0], "3 running")
        self.assertColor(cells[0][col], th["secondary"], "header counts")
        self.assertColor(cells[0][lines[0].index("·")], th["secondary"], "header counts separator")
        # group header: path bold, " · N" secondary
        self.assertTrue(cells[2][1].bold, "group path must be bold: %r" % cells[2][1])
        self.assertColor(cells[2][lines[2].index("·")], th["secondary"], "group count")
        # glyphs / details
        self.assertColor(cells[3][3], th["running"], "running ●")
        ex3, sig, ok0 = 7, 8, 9
        self.assertColor(cells[ex3][find_col(lines[ex3], "exited (3)")], th["error"], "exited (3)")
        self.assertColor(cells[sig][find_col(lines[sig], "killed")], th["error"], "killed (SIGTERM)")
        self.assertColor(cells[ok0][find_col(lines[ok0], "exited (0)")], th["secondary"], "exited (0)")
        for i in (ex3, sig, ok0):
            g = cells[i][3]
            self.assertTrue(color_close(g.fg, th["secondary"]) or g.dim,
                            "exited ○ must be dim/secondary: %r" % g)
        # age: secondary
        self.assertColor(cells[4][W - 2], th["secondary"], "age")
        # footer hints: key in default fg, description in secondary
        f = lines[29]
        self.assertColor(cells[29][find_col(f, "select")], th["secondary"], "footer hint text")
        # selection: first row, full-width bar, name bold
        self.wait_selected(t, "zed")
        cells = t.cells()
        self.assertTrue(cells[3][5].bold, "selected name must be bold: %r" % cells[3][5])
        for col in (5, 20, W - 3):
            self.assertTrue(color_close(cells[3][col].bg, th["sel"]),
                            "selection bar must span the row; col %d bg=%r" % (col, cells[3][col].bg))
        self.assertIsNone(cells[4][5].bg, "unselected row must have no background")

    def test_name_column_width(self):
        self.new("short", "sleep", "1000")
        self.new("a-rather-long-name20", "sleep", "1001")
        t = self.start_list("short", "a-rather-long-name20")
        lines = t.wait_for(lambda l: row_re("●", "short", "sleep 1000", namew=20).match(l[3])
                           and row_re("●", "a-rather-long-name20", "sleep 1001", namew=20).match(l[4]),
                           msg="complete rows")
        d = dump(lines)
        self.assertRegex(lines[3], row_re("●", "short", "sleep 1000", namew=20), d)
        self.assertRegex(lines[4], row_re("●", "a-rather-long-name20", "sleep 1001", namew=20), d)

    def test_empty_state(self):
        t = self.tui()
        lines = t.wait_for(lambda l: l[0] == " ✻ pmux" and l[3] == "   No processes yet."
                           and l[4] == "   Press ^n to start one." and l[29] == FOOTER_EMPTY,
                           msg="complete empty-state frame")
        d = dump(lines)
        self.assertEqual(lines[0], " ✻ pmux", d)
        self.assertEqual(lines[1:3], ["", ""], d)
        self.assertEqual(lines[3], "   No processes yet.", d)
        self.assertEqual(lines[4], "   Press ^n to start one.", d)
        self.assertEqual(lines[5:29], [""] * 24, d)
        self.assertEqual(lines[29], FOOTER_EMPTY, d)
        cells = t.cells()
        self.assertColor(cells[4][3], self.THEME["secondary"], "'Press' hint")

    def test_type_to_filter_hint_above_eight(self):
        many = self.mkdir("many")
        for i in range(8):
            self.new("p%d" % i, "sleep", "10%02d" % i, cwd=many)
        t = self.start_list(*("p%d" % i for i in range(8)))
        lines = t.wait_for(lambda l: l[-1].startswith(FOOTER_LIST), msg="footer with 8 processes")
        self.assertEqual(lines[-1], FOOTER_LIST, dump(lines))
        self.new("p8", "sleep", "1008", cwd=many)
        t.wait_list("p8")
        t.wait_for(lambda l: l[-1] == FOOTER_LIST + "  type to filter",
                   msg="footer with >8 processes")

    def test_resize(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        t.resize(40, 120)
        lines = t.wait_for(lambda l: l[39] == FOOTER_LIST and len(l[0]) == 119
                           and row_re("●", "a1", "sleep 1000", width=120).match(l[3]),
                           msg="complete frame after resize")
        d = dump(lines)
        self.assertEqual(len(lines[0]), 119, "counts end at W-2 after resize\n" + d)
        self.assertRegex(lines[3], row_re("●", "a1", "sleep 1000", width=120), d)
        self.assertEqual(len(lines[3]), 119, d)


class LightTheme(TuiCase):
    CONFIG = "theme = light\n"
    THEME = LIGHT

    def test_light_colors(self):
        self.new("a1", "sleep", "1000")
        self.new("a2", "sleep", "1001")
        t = self.start_list("a1", "a2")
        cells = t.cells()
        self.assertColor(cells[0][1], LIGHT["accent"], "✻ (light)")
        self.assertColor(cells[0][find_col(t.screen()[0], "2 running")], LIGHT["secondary"],
                         "counts (light)")
        self.assertColor(cells[3][3], LIGHT["running"], "● (light)")
        self.wait_selected(t, "a1")


class TealAccent(TuiCase):
    CONFIG = "theme = dark\naccent = teal\n"

    def test_teal_accent(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        cells = t.cells()
        self.assertColor(cells[0][1], TEAL_ACCENT, "✻ (accent = teal)")
        self.assertColor(cells[0][3], TEAL_ACCENT, "pmux (accent = teal)")


class AnsiTheme(TuiCase):
    CONFIG = "theme = ansi\n"

    def test_ansi_palette_only(self):
        self.new("a1", "sleep", "1000")
        self.new("a2", "sleep", "1001")
        t = self.start_list("a1", "a2")
        raw = "\n".join(t.screen_ansi())
        self.assertNotRegex(raw, r"\x1b\[[0-9;:]*[34]8[;:]2[;:]",
                            "theme = ansi must not use 24-bit colors:\n%r" % raw)
        cells = t.cells()
        self.assertIn(cells[0][1].fg, (("idx", 5), ("idx", 13)), "accent is palette 5 (magenta)")
        self.assertIn(cells[3][5].bg, (("idx", 4), ("idx", 12)),
                      "selection bar is palette 4 (blue): %r" % cells[3][5])
        self.assertIsNone(cells[4][5].bg)


class AutoTheme(TuiCase):
    CONFIG = None   # theme = auto (default)

    def test_auto_without_osc11_reply_falls_back_to_dark(self):
        self.new("a1", "sleep", "1000")
        t = self.tui()
        t.wait_list("a1", timeout=5)
        self.assertColor(t.cells()[0][1], DARK["accent"], "auto theme without OSC 11 reply")
        self.wait_selected(t, "a1")


# ===========================================================================
# navigation / attach


class Navigation(TuiCase):
    def test_keys_move_over_rows_only(self):
        for n in ("a1", "a2"):
            self.marker(n, cwd=self.api)
        self.marker("w1", cwd=self.web)
        t = self.start_list("a1", "a2", "w1")
        self.wait_selected(t, "a1")
        t.keys("Down")
        self.wait_selected(t, "a2")
        t.keys("Down")                      # skips blank line + group header
        self.wait_selected(t, "w1")
        t.keys("Up")
        self.wait_selected(t, "a2")
        t.raw(HOME_KEY)
        self.wait_selected(t, "a1")
        t.raw(END_KEY)
        self.wait_selected(t, "w1")
        t.keys("Up", "Up")
        self.wait_selected(t, "a1")
        t.keys("Down")
        self.wait_selected(t, "a2")
        t.keys("Enter")
        self.wait_attached(t, "a2")
        self.detach_to_list(t, "a1", "a2", "w1")
        self.wait_selected(t, "a2")

    def test_attach_passes_input_and_returns(self):
        log = self.marker("app")
        self.marker("other")
        t = self.start_list("app", "other")
        t.keys("Enter")
        lines = self.wait_attached(t, "app")
        self.assertFalse(lines[0].startswith(" ✻ pmux"), "no list chrome while attached\n" + dump(lines))
        t.type("hello")
        wait_until(lambda: read_file(log) == b"hello", msg=lambda: read_file(log))
        self.detach_to_list(t, "app", "other")
        self.wait_selected(t, "app")
        self.assertEqual(read_file(log), b"hello", "detach key must not reach the app")
        self.assertEqual(self.px.entry("app")["state"], "running")
        # and again
        t.keys("Enter")
        self.wait_attached(t, "app")
        self.detach_to_list(t, "app", "other")

    def test_enter_on_exited(self):
        self.new_exited("first", "echo first-output; exit 0")
        self.new_exited("done", "echo done-final-output; exit 4")
        t = self.start_list("first", "done")
        self.wait_selected(t, "first")
        t.keys("Down")
        self.wait_selected(t, "done")
        t.keys("Enter")
        lines = t.wait_for(lambda l: any("done-final-output" in x for x in l),
                           msg="final screen of the selected exited process")
        self.assertFalse(lines[0].startswith(" ✻ pmux"), "final screen, not the list\n" + dump(lines))
        self.assertFalse(any("first-output" in x for x in lines), dump(lines))
        t.keys("Up")                        # closes the view; must not move the selection
        t.wait_list("first", "done")
        self.wait_selected(t, "done")
        self.assertNotIn("has exited", t.footer(), dump(t.screen()))

    def test_exit_while_attached(self):
        self.marker("app", self.api, "exit", "7")
        t = self.start_list("app")
        t.keys("Enter")
        self.wait_attached(t, "app")
        t.type("x")
        lines = t.wait_for(lambda l: l[0].startswith(" ✻ pmux") and "app exited: 7" in l[-1],
                           msg="back to list after exit")
        self.assertFalse(any("[app" in l for l in lines), dump(lines))
        self.assertEqual(self.px.entry("app")["state"], "exited:7")
        self.assertRegex(lines[3], row_re("○", "app", "exited (7)"), dump(lines))


# ===========================================================================
# dialogs: new / rename / kill


def field_line(lines, label):
    for l in lines:
        if re.search(r"(^|[│ ])%s\b" % label, l):
            return l
    raise AssertionError("no %r field in dialog\n%s" % (label, dump(lines)))


class NewDialog(TuiCase):
    def open_dialog(self, t):
        t.keys("C-n")
        return t.wait_for(dialog_complete, msg="new-process dialog")

    def test_prefill_create_and_attach(self):
        self.new("a1", "sleep", "1000", cwd=self.api)
        self.new("w1", "sleep", "1001", cwd=self.web)
        t = self.start_list("a1", "w1")
        t.keys("Down")
        self.wait_selected(t, "w1")
        lines = self.open_dialog(t)
        d = dump(lines)
        self.assertIn("~/work/web", field_line(lines, "Dir"), d)
        self.assertIn("/bin/sh", field_line(lines, "Command"), d)
        field_line(lines, "Name")
        self.assertTrue(any(DIALOG_HINT in l for l in lines), d)
        self.assertTrue(any("╭" in l for l in lines) and any("╯" in l for l in lines),
                        "rounded border\n" + d)
        row = next(i for i, l in enumerate(lines) if "New process" in l)
        self.assertColor(t.cells()[row][lines[row].index("New process")], self.THEME["accent"],
                         "dialog title")
        t.type("fresh")
        t.keys("Enter")
        e = self.px.wait_state("fresh", lambda s: True)
        self.assertEqual((e["dir"], e["command"]), (self.web, "/bin/sh"))
        t.wait_for(lambda l: not l[0].startswith(" ✻ pmux"), msg="attached after create")
        t.type("echo PMUX$((40+2))")
        t.keys("Enter")
        t.wait_for("PMUX42", msg="typing into the new shell")
        lines = self.detach_to_list(t, "a1", "w1", "fresh")
        web = lines.index(" ~/work/web · 2")
        self.assertGreater(row_index(lines, "fresh"), web, dump(lines))

    def test_duplicate_name_keeps_dialog(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        self.open_dialog(t)
        t.type("a1")
        t.keys("Enter")
        lines = t.wait_for(re.compile(r"already in use|already exists|taken|exists"),
                           msg="duplicate-name error in dialog")
        self.assertTrue(any("New process" in l for l in lines), "dialog must stay open\n" + dump(lines))
        self.assertEqual([r["name"] for r in self.px.list()], ["a1"])
        t.keys("Escape")
        t.wait_for(lambda l: not any("New process" in x for x in l), msg="Esc closes the dialog")
        self.assertEqual([r["name"] for r in self.px.list()], ["a1"])

    def test_escape_cancels(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        self.open_dialog(t)
        t.type("nope")
        t.keys("Escape")
        t.wait_list("a1")
        lines = t.wait_for(lambda l: not any("New process" in x for x in l) and l[-1] == FOOTER_LIST,
                           msg="Esc closes the dialog")
        self.assertEqual([r["name"] for r in self.px.list()], ["a1"])
        self.assertEqual(lines[-1], FOOTER_LIST, dump(lines))

    def test_empty_list_uses_launch_dir(self):
        launch = self.mkdir("launch")
        t = self.tui(cwd=launch)
        t.wait_for("No processes yet.")
        lines = self.open_dialog(t)
        self.assertIn("~/launch", field_line(lines, "Dir"), dump(lines))
        t.type("first")
        t.keys("Enter")
        e = self.px.wait_state("first", lambda s: True)
        self.assertEqual(e["dir"], launch)


class Rename(TuiCase):
    def clear_field(self, t, n=12):
        # Works whether the cursor starts at the end or at the start.
        t.keys(*(["BSpace"] * n + ["DC"] * n))

    def start(self):
        self.new("a1", "sleep", "1000")
        self.new("a2", "sleep", "1001")
        t = self.start_list("a1", "a2")
        self.wait_selected(t, "a1")
        t.keys("C-r")
        t.wait_for(lambda l: l[-1] == FOOTER_RENAME, msg="rename footer")
        return t

    def test_rename(self):
        t = self.start()
        self.clear_field(t)
        t.type("renamed")
        t.keys("Enter")
        wait_until(lambda: [r["name"] for r in self.px.list()] == ["renamed", "a2"],
                   msg=lambda: self.px.list())
        lines = t.wait_list("renamed", "a2")
        t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="footer after rename")
        self.assertNotIn("a1", names(lines))

    def test_duplicate_shows_red_error(self):
        t = self.start()
        self.clear_field(t)
        t.type("a2")
        t.keys("Enter")
        th = self.THEME

        def red_footer(lines):
            f = t.cells()[-1]
            return lines[-1] not in (FOOTER_RENAME, FOOTER_LIST) and any(
                c.ch.strip() and color_close(c.fg, th["error"]) for c in f)
        t.wait_for(red_footer, msg="red error in footer")
        self.assertEqual([r["name"] for r in self.px.list()], ["a1", "a2"])
        t.keys("Escape")
        t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="Esc leaves rename")
        self.assertEqual([r["name"] for r in self.px.list()], ["a1", "a2"])

    def test_escape_cancels(self):
        t = self.start()
        t.type("zzz")
        t.keys("Escape")
        lines = t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="Esc leaves rename")
        self.assertEqual(names(lines), ["a1", "a2"])
        self.assertEqual([r["name"] for r in self.px.list()], ["a1", "a2"])


class Kill(TuiCase):
    CONFIRM = " Kill victim (sleep 1000)?  y yes · n no"

    def test_confirm_no_then_yes(self):
        self.new("victim", "sleep", "1000")
        t = self.start_list("victim")
        t.keys("C-x")
        lines = t.wait_for(lambda l: l[-1] == self.CONFIRM, msg="kill confirmation footer")
        f = t.cells()[-1]
        for c in f[1:5]:
            self.assertColor(c, self.THEME["error"], "'Kill'")
            self.assertTrue(c.bold, "'Kill' must be bold: %r" % c)
        t.type("n")
        t.wait_for(lambda l: l[-1] == FOOTER_LIST, msg="n cancels")
        self.assertIn(self.px.entry("victim")["state"], ("running", "idle"))
        t.keys("C-x")
        t.wait_for(lambda l: l[-1] == self.CONFIRM, msg="kill confirmation footer (2)")
        t.type("y")
        self.px.wait_state("victim", lambda s: s == "signaled:1")
        lines = t.wait_for(lambda l: any(row_re("○", "victim", "killed (SIGHUP)").match(x) for x in l),
                           msg="killed row")
        self.assertColor(t.cells()[3][find_col(lines[3], "killed")], self.THEME["error"], "killed")

    def test_kill_exited_removes(self):
        self.new_exited("gone", "exit 0")
        self.new("stay", "sleep", "1000")
        t = self.start_list("gone", "stay")
        self.wait_selected(t, "gone")
        t.keys("C-x")
        wait_until(lambda: [r["name"] for r in self.px.list()] == ["stay"],
                   msg=lambda: "gone not removed; -l=%r\n%s" % (self.px.list(), t.describe()))
        lines = t.wait_for(lambda l: names(l) == ["stay"], msg="row removed")
        self.assertIn(" ~/work/api · 1", lines, dump(lines))


# ===========================================================================
# filter


class Filter(TuiCase):
    def populate(self):
        self.marker("alpha", cwd=self.api)
        self.marker("build-watch", cwd=self.web)
        self.new("gamma", "sleep", "4242", cwd=self.api)
        self.new("delta", "sleep", "1000", cwd=self.mkdir("srv/datadir"))
        return self.start_list("alpha", "build-watch", "gamma", "delta")

    def test_filter_name_dir_command(self):
        t = self.populate()
        t.type("buil")
        lines = t.wait_for(lambda l: l[2] == " › buil▏" and names(l) == ["build-watch"]
                           and l[3] == "" and " ~/work/web · 1" in l and l[-1] == FOOTER_FILTER,
                           msg="filter 'buil'")
        d = dump(lines)
        self.assertEqual(lines[3], "", d)
        self.assertIn(" ~/work/web · 1", lines, d)
        self.assertEqual(lines[-1], FOOTER_FILTER, d)
        self.assertColor(t.cells()[2][1], self.THEME["accent"], "filter prompt ›")
        t.keys("BSpace")
        t.wait_for(lambda l: l[2] == " › bui▏", msg="backspace edits the filter")
        t.keys("Escape")
        lines = t.wait_for(lambda l: len(names(l)) == 4 and l[-1] == FOOTER_LIST,
                           msg="Esc clears the filter")
        self.assertFalse(any("›" in l for l in lines), dump(lines))
        t.type("datadir")
        t.wait_for(lambda l: names(l) == ["delta"], msg="filter by dir")
        t.keys("Escape")
        t.wait_for(lambda l: len(names(l)) == 4, msg="Esc clears the filter")
        t.type("4242")
        t.wait_for(lambda l: names(l) == ["gamma"], msg="filter by command")

    def test_enter_attaches_filtered_row(self):
        t = self.populate()
        t.type("buil")
        t.wait_for(lambda l: names(l) == ["build-watch"], msg="filter 'buil'")
        t.keys("Enter")
        self.wait_attached(t, "build-watch")
        t.keys("C-\\")
        t.wait_for(lambda l: "build-watch" in names(l), msg="back to list")


# ===========================================================================
# quit / terminal restore


class Quit(TuiCase):
    def _quit_with(self, key):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        self.assertEqual(t.modes()[1], "1", "list screen must enable mouse reporting")
        t.keys(key)
        t.wait_dead()
        self.assertEqual(t.dead_status(), 0, t.describe())
        self.assertEqual(t.modes(), ("0", "0"),
                         "alt screen / mouse reporting left enabled after quit")
        self.assertIn(self.px.entry("a1")["state"], ("running", "idle"))
        self.assertTrue(pid_alive(self.px.daemon_pid()), "daemon must survive the TUI")

    def test_ctrl_q(self):
        self._quit_with("C-q")

    def test_ctrl_c(self):
        self._quit_with("C-c")


# ===========================================================================
# refresh / status


BEL_CMD = "printf '\\a'; exec sleep 1000"
OSC_CMD = "printf '\\033]0;title\\a'; exec sleep 1000"


class Refresh(TuiCase):
    def test_new_process_appears(self):
        self.new("a1", "sleep", "1000")
        t = self.start_list("a1")
        self.new("late", "sleep", "1001", cwd=self.web)
        t.wait_list("late", timeout=3)

    def test_exit_updates_row(self):
        self.new("soon", "sh", "-c", "sleep 1; exit 5")
        t = self.start_list("soon")
        t.wait_for(lambda l: any(row_re("○", "soon", "exited (5)").match(x) for x in l),
                   timeout=5, msg="row shows exit")
        t.wait_for(lambda l: l[0].endswith(" 1 exited"), msg="counts updated")

    def test_bell(self):
        self.new("ringer", "sh", "-c", BEL_CMD)
        t = self.tui()
        lines = t.wait_for(lambda l: any(row_re("!", "ringer", "sleep 1000 · bell").match(x) for x in l),
                           msg="bell row")
        self.assertRegex(lines[0], r" 1 running$", "bell counts as running\n" + dump(lines))
        i = row_index(lines, "ringer")
        self.assertColor(t.cells()[i][3], self.THEME["bell"], "bell !")

    def test_osc_bel_is_not_a_bell(self):
        self.new("titled", "sh", "-c", OSC_CMD)
        self.new("ringer", "sh", "-c", BEL_CMD)
        t = self.tui()
        # ringer is created later, so once its bell shows, titled's output
        # has been processed too.
        lines = t.wait_for(lambda l: any(row_re("!", "ringer", "sleep 1000 · bell").match(x) for x in l),
                           msg="bell row")
        i = row_index(lines, "titled")
        self.assertRegex(lines[i], row_re("●", "titled", "sleep 1000"), dump(lines))

    def test_bell_clears_after_attach(self):
        self.new("ringer", "sh", "-c", BEL_CMD)
        t = self.tui()
        t.wait_for(lambda l: any(row_re("!", "ringer", "sleep 1000 · bell").match(x) for x in l),
                   msg="bell row")
        t.keys("Enter")
        t.wait_for(lambda l: not l[0].startswith(" ✻ pmux"), msg="attached")
        t.keys("C-\\")
        lines = t.wait_list("ringer")
        lines = t.wait_for(lambda l: row_index(l, "ringer") is not None
                           and "bell" not in l[row_index(l, "ringer")], msg="bell cleared")
        self.assertNotEqual(list_rows(lines)[0][1], "!", dump(lines))

    @unittest.skipIf(FAST, "PMUX_FAST=1: skipping >5 s idle test")
    def test_idle(self):
        self.new("quiet", "sleep", "1000")
        t = self.start_list("quiet")
        lines = t.wait_for(lambda l: re.match(r"^   ◌ quiet {7}  sleep 1000 · idle \d+s +%s$" % AGE_RE,
                                              l[3] or ""), timeout=12, msg="idle row")
        self.assertRegex(lines[0], r" 1 idle$", dump(lines))
        self.assertTrue(t.cells()[3][3].dim or color_close(t.cells()[3][3].fg, self.THEME["secondary"]),
                        "◌ must be dim")


# ===========================================================================
# mouse


class Mouse(TuiCase):
    def populate(self):
        for n in ("m1", "m2", "m3"):
            self.marker(n)
        t = self.start_list("m1", "m2", "m3")
        self.wait_selected(t, "m1")
        return t

    def test_click_selects(self):
        t = self.populate()
        lines = t.screen()
        t.click(10, row_index(lines, "m3"))
        self.wait_selected(t, "m3")
        t.click(10, row_index(lines, "m2"))
        self.wait_selected(t, "m2")
        self.assertTrue(t.screen()[0].startswith(" ✻ pmux"), "single click must not attach")

    def test_double_click_attaches(self):
        t = self.populate()
        t.double_click(10, row_index(t.screen(), "m2"))
        self.wait_attached(t, "m2")
        self.detach_to_list(t, "m1", "m2", "m3")
        self.wait_selected(t, "m2")

    def test_wheel_moves_selection(self):
        t = self.populate()
        t.wheel(10, 5, down=True)
        self.wait_selected(t, "m2")
        t.wheel(10, 5, down=True)
        self.wait_selected(t, "m3")
        t.wheel(10, 5, down=False)
        self.wait_selected(t, "m2")


# ===========================================================================
# passthrough through the TUI (pexpect, raw bytes, no terminal emulator)


class TuiPassthrough(TuiCase):
    def open_list(self, name):
        c = self.px.spawn_attach([], rows=30, cols=W)
        out = read_until(c, name.encode())

        def cooked_off():
            lflag = termios.tcgetattr(c.child_fd)[3]
            return not (lflag & (termios.ICANON | termios.ECHO))
        wait_until(cooked_off, msg="TUI tty still in canonical/echo mode")
        return c, out

    def quit(self, c):
        c.send(b"\x11")
        try:
            c.expect(pexpect.EOF, timeout=TIMEOUT)
        except pexpect.TIMEOUT:
            self.fail("TUI did not exit on Ctrl+Q; output tail %r" % c.before[-300:])
        return c.before

    def test_input_is_byte_exact(self):
        log = self.px.path("tapp.in")
        self.px.create_probe("tapp", "inlog", log, winch_mark="tapp")
        c, _ = self.open_list("tapp")
        c.send(b"\r")
        read_until(c, winch_marker("tapp").encode())
        expected = b""
        for part in input_corpus_chunks():
            c.send(part)
            expected += part
        self.wait_log(log, expected)
        c.send(DETACH)
        out = read_until(c, b"tapp")
        self.assertNotIn(b"[detached", out)
        self.assertTrue(c.isalive())
        tail = self.quit(c)
        self.assertNotIn(b"[detached", tail)
        self.assertEqual(read_file(log), expected, "nothing after the detach key reaches the app")

    def test_output_is_byte_exact_and_nothing_else(self):
        corpus = (bytes(range(256)) * 4
                  + b"\x1b[38;2;255;100;0mtruecolor\x1b[48;5;17m256\x1b[0m\r\n"
                  + b"\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\\r\n"
                  + b"\x1b[?1049h\x1b[H\x1b[2Jalt\x1b[?1049l\x1b]0;title\x07"
                  + "unicode ✻ ● ◌ ○ 😀 é\r\n".encode()
                  + bytes((i * 7 + 3) % 256 for i in range(65536)))
        for m in (probe.START_MARKER, probe.END_MARKER):
            self.assertNotIn(m, corpus)
        cpath = self.px.path("corpus.bin")
        with open(cpath, "wb") as f:
            f.write(corpus)
        self.px.create_probe("oapp", "emit", cpath, "winch")
        c, _ = self.open_list("oapp")
        c.send(b"\r")
        data = read_until(c, probe.END_MARKER, timeout=30)
        start = data.rfind(probe.START_MARKER)
        self.assertGreaterEqual(start, 0, "START marker missing")
        self.assertBytesEqual(corpus, data[start + len(probe.START_MARKER):-len(probe.END_MARKER)],
                              "attached output (SIGWINCH emission)")
        # While attached pmux writes nothing of its own: give a list refresh
        # timer time to (wrongly) fire, then check the gap before the next
        # emission.  Extra SIGWINCH emissions are tolerated.
        time.sleep(1.5)
        full = probe.START_MARKER + corpus + probe.END_MARKER
        gap = drain(c)
        while gap.startswith(full):
            gap = gap[len(full):]
        self.assertBytesEqual(b"", gap, "client output while attached and idle")
        c.send(probe.START_BYTE)
        data = read_until(c, probe.END_MARKER, timeout=30)
        self.assertBytesEqual(full, data, "client output for one emission")
        c.send(DETACH)
        out = read_until(c, b"oapp")
        self.assertNotIn(b"[detached", out)
        self.quit(c)

    def test_terminal_modes_restored_on_quit(self):
        self.new("a1", "sleep", "1000")
        c, out = self.open_list("a1")
        out += self.quit(c)
        wait_until(lambda: not c.isalive())
        self.assertEqual((c.exitstatus, c.signalstatus), (0, None))
        state = {}
        for m in re.finditer(rb"\x1b\[\?([0-9;]+)([hl])", out):
            for mode in m.group(1).split(b";"):
                state[int(mode)] = m.group(2)
        mouse = [m for m in (1000, 1002, 1003) if m in state]
        self.assertTrue(mouse, "TUI never enabled mouse reporting; output=%r" % out[:400])
        for mode in (1000, 1002, 1003, 1004, 1005, 1006, 1015, 1049, 2004):
            if mode in state:
                self.assertEqual(state[mode], b"l", "mode ?%d left enabled after quit" % mode)
        if 25 in state:
            self.assertEqual(state[25], b"h", "cursor left hidden after quit")


if __name__ == "__main__":
    unittest.main()
