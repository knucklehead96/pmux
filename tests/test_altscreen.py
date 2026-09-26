"""The session on the terminal's alternate screen: the shell screen and its
scrollback survive attach / detach / quit, the app's own alternate screen
switches are emulated, pmux's mouse (wheel scroll mode, alternate-scroll
cursor keys) and the scroll mode keys."""
import os
import re
import time

from helpers import PMUX_BIN, PYTHON, drain, read_file, read_until, wait_until
from test_restore import INDICATOR, RestoreCase
from tui import dump, list_rows

PMUX_MOUSE = b"\x1b[?1000h\x1b[?1006h"

WHEEL_UP = b"\x1b[<64;10;10M"
WHEEL_DOWN = b"\x1b[<65;10;10M"
CLICK = b"\x1b[<0;5;5M\x1b[<0;5;5m"

# A scripted app: logs every input byte to argv[1], creates argv[2] once in raw mode, and runs
# the command after each "!" in its input.
APP = r'''
import os, sys, time, tty
log, ready = sys.argv[1], sys.argv[2]
tty.setraw(0)
def out(b):
    os.write(1, b)
CMDS = {
    b"H": lambda: out(b"".join(b"session %03d\r\n" % i for i in range(1, 101))),
    b"A": lambda: out(b"\x1b[?1049h\x1b[H\x1b[2J\x1b[3;5H\x1b[1mALT-SCREEN\x1b[0m\x1b[6;2H"),
    b"L": lambda: out(b"\x1b[?1049l"),
    b"B": lambda: out(b"\x1b[?1002h"),
    b"b": lambda: out(b"\x1b[?1002l"),
    b"S": lambda: out(b"\x1b[?1000h\x1b[?1006h"),
    b"s": lambda: out(b"\x1b[?1000l\x1b[?1006l"),
    b"K": lambda: out(b"\x1b[?1h"),
    b"k": lambda: out(b"\x1b[?1l"),
    b"Q": lambda: out(b"\x1b[6n"),
    b"R": lambda: out(b"\x1bcAFTER-RIS"),
    b"P": lambda: (out(b"<split>\x1b[?10"), time.sleep(0.005), out(b"49;1004hSPLIT-DONE</split>")),
    b"p": lambda: (out(b"<plain>\x1b[?1049l"), time.sleep(0.005), out(b"\x1b[?2004;47;1004lPLAIN-DONE</plain>")),
}
open(ready, "w").write("%d\n" % os.getpid())
pending = b""
while True:
    data = os.read(0, 4096)
    if not data:
        break
    with open(log, "ab") as f:
        f.write(data)
    pending += data
    while b"!" in pending[:-1]:
        i = pending.index(b"!")
        cmd, pending = pending[i + 1:i + 2], pending[i + 2:]
        if cmd in CMDS:
            CMDS[cmd]()
    pending = pending[-1:] if pending.endswith(b"!") else b""
'''


class AltCase(RestoreCase):
    def setUp(self):
        super().setUp()
        self.app_path = self.px.path("app.py")
        with open(self.app_path, "w") as f:
            f.write(APP)

    def app_cmd(self, name):
        return [PYTHON, self.app_path, self.px.path(name + ".in"), self.px.path(name + ".ready")]

    def app(self, name):
        """Create the scripted app detached (in files/: <name>.in its input log)."""
        p = self.px.run("-n", name, "-d", "--", *self.app_cmd(name), cwd=self.api)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.px.wait_ready(self.px.path(name + ".ready"), name)
        return self.px.path(name + ".in")

    def log(self, name):
        return read_file(self.px.path(name + ".in"))

    def out(self, name, data):
        """Output from the app `name`: written to its terminal directly."""
        tty = os.readlink("/proc/%d/fd/0" % int(read_file(self.px.path(name + ".ready"))))
        with open(tty, "wb", buffering=0) as f:
            f.write(data)

    def shell(self, rows=24, cols=80):
        """A tmux pane running sh with three marker lines on its screen."""
        t = self.pane(["/bin/sh"], rows, cols)
        t.wait_for("$", msg="shell prompt")
        t.type("for i in 1 2 3; do echo SHELL-MARK-$i; done\n")
        t.wait_for("SHELL-MARK-3", msg="shell markers")
        return t

    def assertShellClean(self, t, session_text="session"):
        """The main screen and its scrollback: markers intact, no session output, not on the
        alternate screen."""
        wait_until(lambda: t.fmt("alternate_on")["alternate_on"] == "0",
                   msg=lambda: "still on the alternate screen\n" + t.describe())
        hist = t.history()
        text = "\n".join(hist)
        marks = [l for l in hist if l.startswith("SHELL-MARK-")]
        self.assertEqual(marks, ["SHELL-MARK-%d" % i for i in (1, 2, 3)], dump(hist, "history"))
        self.assertNotIn(session_text, text, "session output leaked into the shell\n" + dump(hist, "history"))
        return hist

    def cli(self, t, *args):
        t.type(" ".join([PMUX_BIN, *args]) + "\n")

    def wait_alt(self, t, on=True):
        wait_until(lambda: t.fmt("alternate_on")["alternate_on"] == ("1" if on else "0"),
                   msg=lambda: "alternate_on should be %d\n%s" % (on, t.describe()))


# ===========================================================================
# the shell screen survives


class ShellScreenSurvives(AltCase):
    def check_detached_line(self, hist, name):
        self.assertIn("[detached from %s]" % name, hist, "[detached ...] on its own line\n" + dump(hist))
        at = len(hist) - 1 - hist[::-1].index("[detached from %s]" % name)
        self.assertEqual(hist[at + 1], "$", "the prompt follows on the next line\n" + dump(hist))

    def test_cli_new(self):
        t = self.shell()
        self.cli(t, "-n", "fresh", "--", *self.app_cmd("fresh"))
        self.px.wait_ready(self.px.path("fresh.ready"), "fresh")
        self.wait_alt(t)
        t.type("!H")
        t.wait_for("session 100", msg="session output")
        t.keys("C-Left")
        t.wait_for("[detached from fresh]", msg="detach")
        hist = self.assertShellClean(t)
        self.check_detached_line(hist, "fresh")

    def test_cli_attach(self):
        self.app("app")
        t = self.shell()
        for i in (1, 2):
            self.cli(t, "-a", "app")
            self.wait_alt(t)
            if i == 1:
                t.type("!H")
            t.wait_for("session 100", msg="session output")
            t.keys("C-Left")
            t.wait_for(lambda l: sum("[detached from app]" in x for x in l) == i, msg="detach #%d" % i)
            hist = self.assertShellClean(t)
            self.check_detached_line(hist, "app")

    def test_exit_while_attached(self):
        self.app("app")
        t = self.shell()
        self.cli(t, "-a", "app")
        self.wait_alt(t)
        t.type("!H")
        t.wait_for("session 100")
        os.kill(int(read_file(self.px.path("app.ready"))), 9)
        t.wait_for("[app killed by signal 9]", msg="exit message")
        self.assertShellClean(t)

    def test_tui_attach_and_quit(self):
        self.app("app")
        t = self.shell()
        self.cli(t)
        t.wait_list("app")
        t.keys("Enter")
        t.wait_for(lambda l: not any("pmux" in x for x in l), msg="attached")
        t.type("!H")
        t.wait_for("session 100", msg="attached output")
        self.detach_to_list(t, "app")
        t.keys("C-c", "C-c")
        t.wait_for("SHELL-MARK-3", msg="back at the shell")
        self.assertShellClean(t)

    def test_tui_view_exited_and_quit(self):
        self.new_exited("done", "i=1; while [ $i -le 60 ]; do echo session $i; i=$((i+1)); done")
        t = self.shell()
        self.cli(t)
        t.wait_list("done")
        t.keys("Enter")
        t.wait_for("session 60", msg="final screen")
        t.keys("Escape")
        t.wait_list("done")
        t.keys("C-c", "C-c")
        t.wait_for("SHELL-MARK-3", msg="back at the shell")
        self.assertShellClean(t)

    def test_tui_quit_without_attach(self):
        self.app("app")
        t = self.shell()
        self.cli(t)
        t.wait_list("app")
        t.keys("C-c", "C-c")
        t.wait_for("SHELL-MARK-3", msg="back at the shell")
        self.assertShellClean(t)


# ===========================================================================
# the app's own alternate screen


class AppAltScreen(AltCase):
    def test_smcup_rmcup_emulated(self):
        self.app("app")
        t = self.attach_pane_app("app")
        t.type("!H")
        t.wait_for(lambda l: l[22] == "session 100", msg="primary content")
        before = t.screen()
        t.type("!A")
        lines = t.wait_for(lambda l: l[2] == "    ALT-SCREEN", msg="app's alternate screen")
        self.assertFalse(any("session" in l for l in lines), dump(lines))
        self.assertEqual(t.cursor(), (1, 5))
        self.assertEqual(t.fmt("alternate_on")["alternate_on"], "1")
        t.type("!L")
        t.wait_for(lambda l: l == before, msg="primary screen repainted after rmcup")
        self.assertEqual(t.fmt("alternate_on")["alternate_on"], "1", "the terminal left pmux's screen")
        self.assertEqual(t.cursor(), (0, 23))
        # the history of the primary screen is intact: scroll mode
        t.raw(WHEEL_UP)
        t.wait_for(lambda l: l[0].startswith("session 075") and INDICATOR.search(l[0]), msg="scroll")
        t.keys("q")
        self.detach_pane(t, "app")

    def attach_pane_app(self, name):
        t = self.pane([PMUX_BIN, "-a", name])
        self.wait_alt(t)
        time.sleep(0.2)
        return t

    def raw_attach(self, name):
        c = self.px.attach(name)
        c.delaybeforesend = None
        return c

    def test_alt_params_stripped_split_across_reads(self):
        self.app("app")
        c = self.raw_attach("app")
        c.send(b"!P")
        data = read_until(c, b"</split>")
        seg = data[data.index(b"<split>"):]
        self.assertNotIn(b"1049", seg, "the app's alt screen switch reached the terminal: %r" % seg)
        self.assertIn(b"\x1b[?1004h", seg, "other parameters of the same CSI stay: %r" % seg)
        at = seg.index(b"\x1b[?1004h")
        self.assertIn(b"\x1b[1;1H\x1b[2K", seg[at:], "repaint after the switch: %r" % seg)
        self.assertLess(seg.index(b"\x1b[1;1H\x1b[2K"), seg.index(b"SPLIT-DONE"))
        c.send(b"!p")
        data = read_until(c, b"</plain>")
        seg = data[data.index(b"<plain>"):]
        self.assertNotIn(b"1049", seg, seg)
        self.assertNotIn(b";47", seg, seg)
        self.assertIn(b"\x1b[?2004;1004l", seg, "47 removed, the rest kept: %r" % seg)
        self.detach(c, "app")

    def test_ris_replaced_by_a_soft_reset(self):
        # RIS would leave the terminal's alternate screen and (VTE) erase the user's scrollback.
        self.app("app")
        c = self.raw_attach("app")
        self.out("app", b"\x1b[?2004h\x1b]11;#123456\x07<ris>\x1bcAFTER-RIS</ris>")
        data = read_until(c, b"</ris>")
        seg = data[data.index(b"<ris>"):]
        self.assertNotIn(b"\x1bc", seg, "RIS forwarded: %r" % seg)
        reset = seg.index(b"\x1b[!p")
        for seq in (b"\x1b[?2004l", b"\x1b[<99u", b"\x1b]111\x1b\\", PMUX_MOUSE, b"\x1b[1;1H\x1b[2K"):
            self.assertGreater(seg.find(seq, reset), reset, "%r missing after the soft reset: %r" % (seq, seg))
        self.assertLess(seg.index(PMUX_MOUSE), seg.index(b"AFTER-RIS"), seg)
        self.assertNotIn(b"1049", seg, seg)
        self.detach(c, "app")

    def test_ris_in_tmux(self):
        self.app("app")
        t = self.attach_pane_app("app")
        t.type("!R")
        t.wait_for(lambda l: l[0] == "AFTER-RIS", msg="output after RIS")
        f = t.fmt("alternate_on", "mouse_standard_flag", "mouse_sgr_flag")
        self.assertEqual(f, {"alternate_on": "1", "mouse_standard_flag": "1", "mouse_sgr_flag": "1"},
                         "pmux's screen and mouse after RIS")
        self.detach_pane(t, "app")

    def test_erase_scrollback_not_forwarded(self):
        self.app("app")
        c = self.raw_attach("app")
        c.send(b"!H")
        read_until(c, b"session 100")
        self.out("app", b"<ed3>\x1b[H\x1b[2J\x1b[3J\x1b[3")
        time.sleep(0.005)
        self.out("app", b"JCLEARED\x1b[1;3Jx\x1b[0J</ed3>")
        data = read_until(c, b"</ed3>")
        seg = data[data.index(b"<ed3>"):]
        self.assertEqual(seg, b"<ed3>\x1b[H\x1b[2JCLEARED\x1b[1;3Jx\x1b[0J</ed3>",
                         "only CSI 3 J removed (also split across reads)")
        # ... but the session's history is gone: the wheel has nothing to show.
        c.send(b"\x1b[<64;1;1M!Z")
        wait_until(lambda: self.log("app").endswith(b"!Z"), msg=lambda: "log %r" % self.log("app"))
        time.sleep(0.2)
        self.assertNotRegex(drain(c), rb" \d+/\d+ ", "scroll mode after clear")
        self.detach(c, "app")

    def test_combined_reset_keeps_pmux_mouse(self):
        # 1049 together with mouse modes, the screen not switching: pmux must take the mouse back.
        self.app("app")
        t = self.attach_pane_app("app")
        wait_until(lambda: t.fmt("mouse_sgr_flag")["mouse_sgr_flag"] == "1", msg="pmux's mouse")
        self.out("app", b"\x1b[?1000;1006;1049l")
        time.sleep(0.3)
        f = t.fmt("mouse_standard_flag", "mouse_sgr_flag", "alternate_on")
        self.assertEqual(f, {"mouse_standard_flag": "1", "mouse_sgr_flag": "1", "alternate_on": "1"})
        self.detach_pane(t, "app")

    def test_unmatched_alt_screen_exit_keeps_the_default_pen(self):
        # Leaving an alternate screen never entered restores a cursor never saved: the default
        # pen, not libvterm's zeroed one (black on black).
        self.app("app")
        c = self.raw_attach("app")
        self.out("app", b"<rc>\x1b[?1049l</rc>")
        data = read_until(c, b"</rc>")
        seg = data[data.index(b"<rc>"):]
        self.assertIn(b"\x1b[1;1H\x1b[2K", seg, "repaint")
        self.assertNotIn(b"38;2;0;0;0", seg, seg)
        self.detach(c, "app")

    def test_decstr_keeps_pmux_mouse(self):
        # DECSTR (tput init) turns the mouse modes off on some terminals: pmux sets them again.
        self.app("app")
        c = self.raw_attach("app")
        self.out("app", b"<decstr>\x1b[!p</decstr>")
        data = read_until(c, b"</decstr>")
        seg = data[data.index(b"<decstr>"):]
        self.assertIn(b"\x1b[!p" + PMUX_MOUSE, seg, "pmux's mouse right after the app's DECSTR")
        self.detach(c, "app")

    def test_output_resumes_at_a_sequence_boundary(self):
        # Output that ended inside a sequence while detached: after the snapshot, its rest is
        # not written as text.
        self.app("app")
        self.out("app", b"\x1b[38;5;1")
        time.sleep(0.3)
        t = self.attach_pane_app("app")
        self.out("app", b"23mTEXT\x1b[0m\r\n")
        lines = t.wait_for(lambda l: any("TEXT" in x for x in l), msg="output")
        self.assertFalse(any("23m" in x for x in lines), dump(lines))
        self.detach_pane(t, "app")


# ===========================================================================
# the mouse


class Mouse(AltCase):
    def attach(self, name):
        t = self.pane([PMUX_BIN, "-a", name])
        self.wait_alt(t)
        wait_until(lambda: t.fmt("mouse_sgr_flag")["mouse_sgr_flag"] == "1", msg="pmux's mouse")
        return t

    def flags(self, t):
        f = t.fmt("mouse_standard_flag", "mouse_button_flag", "mouse_sgr_flag")
        return (f["mouse_standard_flag"], f["mouse_button_flag"], f["mouse_sgr_flag"])

    def test_outer_mouse_modes_follow_the_app(self):
        self.app("app")
        t = self.attach("app")
        self.assertEqual(self.flags(t), ("1", "0", "1"), "pmux's: press/release, SGR")
        t.type("!B")   # the app: button-event tracking, no SGR
        wait_until(lambda: self.flags(t) == ("0", "1", "0"), msg=lambda: "app's modes: %r" % (self.flags(t),))
        t.type("!b")
        wait_until(lambda: self.flags(t) == ("1", "0", "1"), msg=lambda: "pmux's again: %r" % (self.flags(t),))
        t.type("!S")
        wait_until(lambda: self.flags(t) == ("1", "0", "1"), msg="app: 1000 + SGR")
        t.type("!s")
        wait_until(lambda: self.flags(t) == ("1", "0", "1"), msg="pmux's again")
        self.detach_pane(t, "app")

    def test_tracking_app_gets_mouse_bytes_unchanged(self):
        self.app("app")
        t = self.attach("app")
        t.type("!S")
        time.sleep(0.3)
        t.raw(WHEEL_UP + WHEEL_DOWN + CLICK)
        wait_until(lambda: self.log("app").endswith(WHEEL_UP + WHEEL_DOWN + CLICK),
                   msg=lambda: "log %r" % self.log("app"))
        self.detach_pane(t, "app")

    def test_clicks_swallowed(self):
        self.app("app")
        t = self.attach("app")
        t.raw(CLICK + b"x")
        wait_until(lambda: self.log("app").endswith(b"x"), msg=lambda: "log %r" % self.log("app"))
        self.assertEqual(self.log("app"), b"x")
        self.detach_pane(t, "app")

    def test_wheel_on_alt_screen_sends_cursor_keys(self):
        self.app("app")
        t = self.attach("app")
        t.type("!A")
        t.wait_for("ALT-SCREEN")
        time.sleep(0.2)
        t.raw(WHEEL_UP)
        t.raw(WHEEL_DOWN)
        want = b"!A" + b"\x1b[A" * 3 + b"\x1b[B" * 3
        wait_until(lambda: self.log("app") == want, msg=lambda: "log %r" % self.log("app"))
        t.type("!K")   # DECCKM
        time.sleep(0.3)
        t.raw(WHEEL_UP)
        t.raw(WHEEL_DOWN)
        want += b"!K" + b"\x1bOA" * 3 + b"\x1bOB" * 3
        wait_until(lambda: self.log("app") == want, msg=lambda: "log %r" % self.log("app"))
        self.detach_pane(t, "app")

    def test_split_mouse_report(self):
        self.app("app")
        c = self.px.attach("app")
        c.delaybeforesend = None
        c.send(b"!H")
        read_until(c, b"session 100")
        c.send(WHEEL_UP[:5])
        time.sleep(0.002)
        c.send(WHEEL_UP[5:])
        read_until(c, b" 3/77 ")
        c.send(b"q")
        time.sleep(0.3)
        self.assertEqual(self.log("app"), b"!H", "nothing of the report or q reached the app")
        self.detach(c, "app")


# ===========================================================================
# scroll mode


class ScrollMode(AltCase):
    def setUp(self):
        super().setUp()
        self.app("app")

    def attached(self):
        t = self.pane([PMUX_BIN, "-a", "app"])
        self.wait_alt(t)
        t.type("!H")
        t.wait_for(lambda l: l[22] == "session 100", msg="history")
        return t

    def top(self, t, first, offset, total=77):
        return t.wait_for(lambda l: l[0].startswith("session %03d" % first)
                          and l[0].endswith(" %d/%d" % (offset, total)),
                          msg="scrolled to session %03d (%d/%d)" % (first, offset, total))

    def live(self, t):
        return t.wait_for(lambda l: l[0] == "session 078" and l[22] == "session 100", msg="live screen")

    def test_navigation(self):
        t = self.attached()
        t.raw(WHEEL_UP)
        lines = self.top(t, 75, 3)
        self.assertEqual(lines[1:23], ["session %03d" % i for i in range(76, 98)], dump(lines))
        t.keys("Up")
        self.top(t, 74, 4)
        t.raw(WHEEL_UP)
        self.top(t, 71, 7)
        t.keys("Down")
        self.top(t, 72, 6)
        t.keys("PageUp")
        self.top(t, 49, 29)
        t.keys("PageDown")
        self.top(t, 72, 6)
        t.keys("Home")
        self.top(t, 1, 77)
        t.raw(WHEEL_DOWN)
        self.top(t, 4, 74)
        t.keys("End")
        self.live(t)
        self.assertEqual(self.log("app"), b"!H", "navigation keys reached the app")
        self.detach_pane(t, "app")

    def test_exit_keys_swallowed(self):
        t = self.attached()
        for key in ("q", "Escape", "End"):
            with self.subTest(key=key):
                t.raw(WHEEL_UP)
                self.top(t, 75, 3)
                t.keys(key)
                self.live(t)
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        t.raw(WHEEL_DOWN)   # to the bottom
        self.live(t)
        time.sleep(0.2)
        self.assertEqual(self.log("app"), b"!H", "exit keys reached the app")
        self.detach_pane(t, "app")

    def test_other_key_exits_and_is_forwarded(self):
        t = self.attached()
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        t.type("x")
        self.live(t)
        wait_until(lambda: self.log("app") == b"!Hx", msg=lambda: "log %r" % self.log("app"))
        self.detach_pane(t, "app")

    def test_output_withheld_and_anchored(self):
        t = self.attached()
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        # Output while scrolled: written to the app's terminal directly.
        tty = os.readlink("/proc/%d/fd/0" % int(read_file(self.px.path("app.ready"))))
        with open(tty, "wb", buffering=0) as f:
            f.write(b"\r\nWHILE-SCROLLED\r\n")
        time.sleep(0.5)
        lines = self.top(t, 75, 3)   # frozen
        self.assertFalse(any("WHILE-SCROLLED" in l for l in lines), dump(lines))
        t.keys("Up")
        lines = self.top(t, 74, 6, 79)   # anchored: two more lines above the live screen
        self.assertFalse(any("WHILE-SCROLLED" in l for l in lines), dump(lines))
        t.keys("q")
        t.wait_for(lambda l: "WHILE-SCROLLED" in l[22], msg="live screen with the new output")
        self.detach_pane(t, "app")

    def test_queries_answered_while_scrolled(self):
        t = self.attached()
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        tty = os.readlink("/proc/%d/fd/0" % int(read_file(self.px.path("app.ready"))))
        with open(tty, "wb", buffering=0) as f:   # as if the app asked: the daemon answers
            f.write(b"\x1b[6n")
        wait_until(lambda: re.search(rb"\x1b\[\d+;\d+R", self.log("app")),
                   msg=lambda: "no reply while scrolled; log %r" % self.log("app"))
        self.top(t, 75, 3)
        t.keys("q")
        self.live(t)
        self.detach_pane(t, "app")

    def test_scroll_paint_keeps_input_modes(self):
        self.out("app", b"\x1b[?2004h")
        c = self.px.attach("app")
        c.delaybeforesend = None
        c.send(b"!H")
        read_until(c, b"session 100")
        c.send(WHEEL_UP)
        paint = read_until(c, b" 3/77 ")
        self.assertNotIn(b"\x1b[!p", paint, "DECSTR resets bracketed paste / the mouse on VTE")
        self.assertNotIn(b"\x1b[?2004l", paint)
        c.send(b"\x1b[200~pasted\x1b[201~")   # a paste while scrolled: leaves scroll mode, reaches the app
        wait_until(lambda: self.log("app").endswith(b"\x1b[200~pasted\x1b[201~"),
                   msg=lambda: "log %r" % self.log("app"))
        self.detach(c, "app")

    def test_modes_turned_off_while_scrolled(self):
        t = self.attached()
        self.out("app", b"\x1b[?2004h\x1b[?1004h")
        time.sleep(0.2)
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        self.out("app", b"\x1b[?2004l\x1b[?1004l")
        time.sleep(0.2)
        t.keys("q")
        self.live(t)
        time.sleep(0.2)
        t.tmux("set-buffer", "PASTE")
        t.tmux("paste-buffer", "-p", "-t", t.target)
        wait_until(lambda: b"PASTE" in self.log("app"), msg=lambda: "log %r" % self.log("app"))
        self.assertNotIn(b"\x1b[200~", self.log("app"), "bracketed paste still on")

    def test_colors_reset_while_scrolled(self):
        base = self.reference(b"x")["flags"]["pane_bg"]
        for how in ("q", "detach"):
            with self.subTest(how=how):
                t = self.attached() if how == "q" else self.pane([PMUX_BIN, "-a", "app"])
                self.wait_alt(t)
                self.out("app", b"\x1b]11;#123456\x07" + (b"" if how == "q" else b"\r\n" * 30))
                wait_until(lambda: t.fmt("pane_bg")["pane_bg"] == "#123456", msg="OSC 11")
                t.raw(WHEEL_UP)
                t.wait_for(lambda l: INDICATOR.search(l[0]), msg="scroll mode")
                self.out("app", b"\x1b]111\x07")
                time.sleep(0.2)
                if how == "q":
                    t.keys("q")
                    t.wait_for(lambda l: not INDICATOR.search(l[0]), msg="live")
                    wait_until(lambda: t.fmt("pane_bg")["pane_bg"] == base,
                               msg=lambda: "pane_bg %r after leaving scroll mode" % t.fmt("pane_bg"))
                    self.detach_pane(t, "app")
                else:
                    self.detach_pane(t, "app")
                    self.assertEqual(t.fmt("pane_bg")["pane_bg"], base, "color left set after detach")

    def test_output_resumes_at_a_sequence_boundary(self):
        t = self.attached()
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        self.out("app", b"\x1b[38;5;1")   # while scrolled, a chunk ends inside an SGR
        time.sleep(0.3)
        t.keys("q")
        self.live(t)
        self.out("app", b"23mTEXT\x1b[0m\r\n")
        lines = t.wait_for(lambda l: any("TEXT" in x for x in l), msg="output")
        self.assertFalse(any("23m" in x for x in lines), dump(lines))
        self.detach_pane(t, "app")

    def test_bell_while_scrolled(self):
        t = self.start_list("app", rows=24, cols=80)
        t.keys("Enter")
        t.wait_for(lambda l: not any("pmux" in x for x in l), msg="attached")
        t.type("!H")
        t.wait_for(lambda l: l[22] == "session 100")
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        self.out("app", b"\x07")
        time.sleep(0.3)
        self.detach_to_list(t, "app")
        t.wait_for(lambda l: any(g == "!" and n == "app" for _i, g, n in list_rows(l)),
                   msg="BEL while scrolled sets the bell flag")

    def test_detach_while_scrolled(self):
        t = self.shell()
        self.cli(t, "-a", "app")
        self.wait_alt(t)
        t.type("!H")
        t.wait_for("session 100")
        t.raw(WHEEL_UP)
        self.top(t, 75, 3)
        t.keys("C-Left")
        t.wait_for("[detached from app]", msg="detach")
        self.assertShellClean(t)
        u = self.pane([PMUX_BIN, "-a", "app"])
        self.live(u)
        self.detach_pane(u, "app")



class ViewScroll(AltCase):
    def test_view_does_not_resize_the_stored_screen(self):
        # libvterm (no reflow) cuts lines for good when resized: viewing never resizes.
        wide = "W" * 110
        self.new_exited("wide", "printf '%s\\n'" % wide)   # created at 80 columns: wraps
        go = self.px.path("go")
        p = self.px.run("-n", "w120", "-d", "--", "sh", "-c",
                        "while [ ! -e %s ]; do sleep 0.05; done; printf '%s\\n'" % (go, wide), cwd=self.api)
        self.assertEqual(p.returncode, 0, p.stderr)
        a = self.pane([PMUX_BIN, "-a", "w120"], rows=24, cols=120)   # its screen at 120 columns
        self.wait_alt(a)
        time.sleep(0.3)
        open(go, "w").close()
        self.px.wait_state("w120", lambda s: s.startswith("exited"))
        seen = []
        for cols in (120, 50, 120):
            t = self.start_list("w120", rows=24, cols=cols)
            t.keys("End")   # the last row: w120
            time.sleep(0.2)
            t.keys("Enter")
            lines = t.wait_for(lambda l: any(x.startswith("WWWW") for x in l), msg="view at %d" % cols)
            seen.append(max(len(x) for x in lines if x.startswith("W")))
            t.keys("Escape")
            t.wait_list("w120")
            t.close()
        self.assertEqual(seen, [110, 50, 110], "line length shown at 120, 50, 120 columns")

    def test_view_keeps_pmux_mouse(self):
        self.new_exited("done", "echo hello")
        c = self.px.spawn_attach([], rows=24, cols=80)
        read_until(c, b"done")
        time.sleep(0.3)
        c.send(b"\r")
        data = read_until(c, b"hello")
        tail = read_until(c, PMUX_MOUSE)
        self.assertIn(b"\x1b[!p", data + tail)
        self.assertGreater((data + tail).rfind(PMUX_MOUSE), (data + tail).rfind(b"\x1b[!p"),
                           "pmux's mouse after the snapshot's DECSTR")
        c.send(b"\x1b")
        read_until(c, b"done")

    def test_view_exited_scrolls(self):
        self.new_exited("done", "i=1; while [ $i -le 60 ]; do echo line $i; i=$((i+1)); done")
        t = self.start_list("done", rows=30, cols=80)
        t.keys("Enter")
        # stored at 24 rows, shown at 30: the last 30 lines, none twice
        lines = t.wait_for(lambda l: l[28] == "line 60", msg="final screen at the terminal size")
        self.assertEqual(lines[:29], ["line %d" % i for i in range(32, 61)], dump(lines))
        t.raw(WHEEL_UP)
        t.wait_for(lambda l: l[0].startswith("line 29") and INDICATOR.search(l[0]), msg="scrolled")
        t.keys("Home")
        t.wait_for(lambda l: l[0].startswith("line 1 ") and l[0].endswith(" 31/31"), msg="top")
        t.keys("End")
        t.wait_for(lambda l: l[0] == "line 32", msg="bottom")
        t.keys("Escape")
        t.wait_list("done")
