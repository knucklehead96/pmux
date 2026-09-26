"""tmux-driven harness for the pmux list TUI (Milestone 2).

TmuxTui runs a command (by default `pmux` with no arguments, i.e. the list
TUI) in a detached session of a private tmux server:

    tmux -L <unique> -f /dev/null   (TMUX_TMPDIR = the PmuxEnv temp root)

with `default-terminal tmux-256color`, `remain-on-exit on` (so a quit TUI
leaves a dead pane whose exit status and final terminal modes can still be
inspected) and the PmuxEnv environment plus COLORTERM=truecolor.

Input:
  keys('Down', 'Enter', 'C-n', 'C-\\')  tmux key names (send-keys)
  type('text')                           literal text (send-keys -l)
  raw(b'...')                            exact bytes (send-keys -H); verified
                                         verbatim for all 256 byte values by
                                         HarnessSelfTest in test_tui.py
Output:
  screen()        capture-pane -p     -> list of `rows` strings (rstripped)
  screen_ansi()   capture-pane -p -e  -> list of raw lines with SGR
  cells()         screen_ansi() parsed into per-column Cell objects
  wait_for(pred)  poll the screen (text, regex or callable(lines))

Colors: tmux keeps 24-bit colors in its grid and reports them in
capture-pane -e as `38;2;r;g;b` / `48;2;r;g;b`; palette colors come back
as 30-37/90-97/38;5;n (normalized here to ('idx', n)).  A detached tmux
server does not answer OSC 11, so pmux must fall back to its assumed
background (#1E1E1E dark, #FAFAF7 light) for the selection-bar blend.
"""
import os
import re
import subprocess
import time

import itertools

from helpers import PMUX_BIN, TIMEOUT, PmuxTestCase, read_file, wait_until

ROWS, COLS = 30, 100

# ---------------------------------------------------------------------------
# theme expectations


def blend(color, alpha, bg):
    return tuple(round(b * (1 - alpha) + c * alpha) for c, b in zip(color, bg))


DARK_BG = (0x1E, 0x1E, 0x1E)
LIGHT_BG = (0xFA, 0xFA, 0xF7)

DARK = {
    "accent": (217, 119, 87),
    "secondary": (139, 139, 139),
    "running": (78, 186, 101),
    "bell": (229, 180, 80),
    "error": (224, 108, 117),
    "sel": blend((122, 162, 247), 0.18, DARK_BG),
}
LIGHT = {
    "accent": (193, 95, 60),
    "secondary": (107, 107, 107),
    "running": (46, 139, 71),
    "bell": (183, 121, 31),
    "error": (197, 48, 48),
    "sel": blend((47, 111, 219), 0.12, LIGHT_BG),
}
TEAL_ACCENT = (95, 200, 200)

# ---------------------------------------------------------------------------
# list-screen text helpers

GLYPHS = "●◌!○"
ROW_RE = re.compile(r"^   ([●◌!○]) (\S+)")
AGE_RE = r"\d+(?:s|m|h(?: \d+m)?|d)"
FOOTER_LIST = " ↑↓ select  enter attach  ^n new  ^r rename  ^x kill  ^q quit"
FOOTER_FILTER = " ↑↓ select  enter attach  esc clear filter"
FOOTER_EMPTY = " ^n new  ^q quit"
FOOTER_RENAME = " enter save · esc cancel"
DIALOG_HINT = "enter create · tab next · esc cancel"


def name_width(names):
    return min(max([12] + [len(n) for n in names]), 24)


def list_rows(lines):
    """[(line_index, glyph, name)] for every process row on the screen."""
    out = []
    for i, line in enumerate(lines):
        m = ROW_RE.match(line)
        if m:
            out.append((i, m.group(1), m.group(2)))
    return out


def row_index(lines, name):
    for i, _g, n in list_rows(lines):
        if n == name:
            return i
    return None


def dialog_complete(lines):
    """The new-process dialog is fully drawn: its title and, below it, the
    key hint (the dialog's last line)."""
    top = next((i for i, l in enumerate(lines) if "New process" in l), None)
    return top is not None and any(DIALOG_HINT in l for l in lines[top + 1:])


def dump(lines, title="screen"):
    body = "\n".join("  %2d|%s" % (i, l) for i, l in enumerate(lines))
    return "--- %s ---\n%s\n--- end ---" % (title, body)


# ---------------------------------------------------------------------------
# SGR parsing of capture-pane -e output


class Cell:
    __slots__ = ("ch", "fg", "bg", "bold", "dim", "underline", "reverse")

    def __init__(self, ch, st):
        self.ch = ch
        self.fg, self.bg = st["fg"], st["bg"]
        self.bold, self.dim = st["bold"], st["dim"]
        self.underline, self.reverse = st["underline"], st["reverse"]

    def __repr__(self):
        return "Cell(%r fg=%r bg=%r%s%s%s)" % (
            self.ch, self.fg, self.bg, " bold" if self.bold else "",
            " dim" if self.dim else "", " ul" if self.underline else "")


def _default_state():
    return {"fg": None, "bg": None, "bold": False, "dim": False,
            "underline": False, "reverse": False}


def _apply_sgr(st, params):
    ps = [p for p in params.split(";")] if params else ["0"]
    i = 0
    while i < len(ps):
        raw = ps[i]
        sub = raw.split(":")
        p = int(sub[0]) if sub[0].isdigit() else 0
        if p == 0:
            st.update(_default_state())
        elif p == 1:
            st["bold"] = True
        elif p == 2:
            st["dim"] = True
        elif p == 22:
            st["bold"] = st["dim"] = False
        elif p == 4:
            st["underline"] = not (len(sub) > 1 and sub[1] == "0")
        elif p == 24:
            st["underline"] = False
        elif p == 7:
            st["reverse"] = True
        elif p == 27:
            st["reverse"] = False
        elif 30 <= p <= 37:
            st["fg"] = ("idx", p - 30)
        elif 90 <= p <= 97:
            st["fg"] = ("idx", p - 90 + 8)
        elif 40 <= p <= 47:
            st["bg"] = ("idx", p - 40)
        elif 100 <= p <= 107:
            st["bg"] = ("idx", p - 100 + 8)
        elif p == 39:
            st["fg"] = None
        elif p == 49:
            st["bg"] = None
        elif p in (38, 48, 58):
            key = {38: "fg", 48: "bg", 58: None}[p]
            if len(sub) > 1:  # colon form 38:2::r:g:b / 38:5:n
                vals = [int(v) if v.isdigit() else 0 for v in sub[1:]]
                if vals[0] == 5:
                    color = ("idx", vals[1])
                else:
                    rgb = vals[-3:]
                    color = tuple(rgb)
            elif i + 1 < len(ps) and ps[i + 1] == "5":
                color = ("idx", int(ps[i + 2]))
                i += 2
            elif i + 1 < len(ps) and ps[i + 1] == "2":
                color = tuple(int(v) for v in ps[i + 2:i + 5])
                i += 4
            else:
                color = None
            if key:
                st[key] = color
        i += 1


_ESC_RE = re.compile(r"\x1b(?:\[([0-9;:?]*)([A-Za-z])|\][^\x07\x1b]*(?:\x07|\x1b\\)|.)")


def parse_ansi_line(line):
    """Parse one capture-pane -e line into a list of Cells (one per column;
    all pmux glyphs are single-width)."""
    st = _default_state()
    cells = []
    pos = 0
    for m in _ESC_RE.finditer(line):
        for ch in line[pos:m.start()]:
            cells.append(Cell(ch, st))
        if m.group(2) == "m":
            _apply_sgr(st, m.group(1))
        pos = m.end()
    for ch in line[pos:]:
        cells.append(Cell(ch, st))
    return cells


def color_close(a, b, tol=1):
    if not (isinstance(a, tuple) and isinstance(b, tuple)) or len(a) != 3 or len(b) != 3:
        return a == b  # None / ('idx', n)
    return all(abs(x - y) <= tol for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# the fixture


_server_ids = itertools.count(1)

# The pane runs `sh -c STATUS_WRAPPER <status file> <cmd...>`: tmux 3.4
# occasionally fails to reap a dead pane's process (pane_dead_status stays
# empty), so the exit status is recorded by this wrapper instead.  `trap :`
# (not `trap ''`) keeps SIGINT at its default disposition in the child.
STATUS_WRAPPER = 'trap : INT; "$@"; echo $? > "$0"'


class TmuxTui:
    def __init__(self, px, args=(), cmd=None, cwd=None, rows=ROWS, cols=COLS, extra_env=None):
        """Run `cmd` (default: [PMUX_BIN, *args]) in a private tmux server."""
        self.px = px
        self.cmd = list(cmd) if cmd else [PMUX_BIN, *args]
        self.cwd = cwd or px.home
        self.rows, self.cols = rows, cols
        sid = next(_server_ids)
        self.sock_name = "%s-%d" % (os.path.basename(px.root), sid)
        self.status_path = os.path.join(px.root, "tmux-status.%d" % sid)
        self.target = "main"
        self.tmux_env = dict(px.env)
        self.tmux_env["TMUX_TMPDIR"] = px.root
        self.pane_env = {"COLORTERM": "truecolor", "PWD": self.cwd}
        if extra_env:
            self.pane_env.update(extra_env)
        self.started = False

    # -- tmux plumbing ------------------------------------------------------

    def tmux(self, *args, check=True):
        p = subprocess.run(["tmux", "-L", self.sock_name, "-f", "/dev/null", *args],
                           env=self.tmux_env, stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=TIMEOUT)
        if check and p.returncode != 0:
            raise AssertionError("tmux %s failed rc=%d: %s"
                                 % (" ".join(args), p.returncode, p.stderr.decode(errors="replace")))
        return p

    def start(self):
        env_args = []
        for k, v in self.pane_env.items():
            env_args += ["-e", "%s=%s" % (k, v)]
        # One tmux invocation: the options are set before the pane's
        # command starts, so even an instantly-exiting command stays
        # inspectable.
        self.tmux("start-server", ";",
                  "set-option", "-g", "exit-empty", "off", ";",
                  "set-option", "-g", "remain-on-exit", "on", ";",
                  "set-option", "-g", "default-terminal", "tmux-256color", ";",
                  "set-option", "-g", "status", "off", ";",
                  "new-session", "-d", "-s", self.target, "-x", str(self.cols),
                  "-y", str(self.rows), "-c", self.cwd, *env_args, "--",
                  "/bin/sh", "-c", STATUS_WRAPPER, self.status_path, *self.cmd)
        self.started = True
        return self

    def close(self):
        if self.started:
            self.tmux("kill-server", check=False)
            self.started = False

    def display(self, fmt):
        return self.tmux("display-message", "-p", "-t", self.target, fmt).stdout.decode().rstrip("\n")

    def pane_dead(self):
        return self.display("#{pane_dead}") == "1"

    def dead_status(self):
        """Exit status of the command (128+N if killed by signal N), or
        None while it is still running."""
        s = read_file(self.status_path).strip()
        return int(s) if s.isdigit() else None

    def modes(self):
        """(alternate_on, mouse_any_flag) of the pane, e.g. ('0', '0')."""
        return tuple(self.display("#{alternate_on} #{mouse_any_flag}").split())

    def pane_pid(self):
        """pid of the pane's process (the exit-status sh wrapper)."""
        return int(self.display("#{pane_pid}"))

    # -- input --------------------------------------------------------------

    def keys(self, *keys):
        self.tmux("send-keys", "-t", self.target, *keys)

    def type(self, text):
        # tmux treats an argv element ending in ';' as a command separator.
        tail = ""
        while text.endswith(";"):
            text, tail = text[:-1], tail + ";"
        if text:
            self.tmux("send-keys", "-t", self.target, "-l", "--", text)
        if tail:
            self.raw(tail.encode())

    def raw(self, data):
        data = bytes(data)
        for i in range(0, len(data), 512):
            self.tmux("send-keys", "-t", self.target, "-H",
                      *("%02x" % b for b in data[i:i + 512]))

    def click(self, col, line, button=0):
        """SGR mouse press + release at 0-based (col, line)."""
        x, y = col + 1, line + 1
        self.raw(b"\x1b[<%d;%d;%dM\x1b[<%d;%d;%dm" % (button, x, y, button, x, y))

    def double_click(self, col, line):
        x, y = col + 1, line + 1
        one = b"\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (x, y, x, y)
        self.raw(one + one)

    def wheel(self, col, line, down=True):
        self.raw(b"\x1b[<%d;%d;%dM" % (65 if down else 64, col + 1, line + 1))

    def resize(self, rows, cols):
        self.tmux("resize-window", "-t", self.target, "-x", str(cols), "-y", str(rows))
        self.rows, self.cols = rows, cols

    # -- output -------------------------------------------------------------

    def _capture(self, *flags):
        out = self.tmux("capture-pane", "-p", *flags, "-t", self.target).stdout
        lines = out.decode("utf-8", errors="replace").split("\n")
        if lines and lines[-1] == "":
            lines.pop()
        lines += [""] * (self.rows - len(lines))
        return lines[:self.rows]

    def screen(self):
        return [l.rstrip() for l in self._capture()]

    def screen_ansi(self):
        return self._capture("-e")

    def cells(self):
        return [parse_ansi_line(l) for l in self.screen_ansi()]

    def describe(self, lines=None):
        lines = self.screen() if lines is None else lines
        extra = ""
        try:
            if self.pane_dead():
                extra = "\n(pane is dead: command exited with status %r)" % self.dead_status()
        except AssertionError:
            extra = "\n(tmux server not reachable)"
        return dump(lines) + extra

    def wait_for(self, pred, timeout=TIMEOUT, msg=None, allow_dead=False):
        """Poll the screen until pred matches; return the matching screen.

        pred: str (substring of some line), compiled regex (searched in
        the newline-joined screen) or callable(lines) -> truthy.
        Fails early if the pane's command died (unless allow_dead)."""
        if isinstance(pred, str):
            text = pred
            check = lambda lines: any(text in l for l in lines)  # noqa: E731
            what = "text %r" % text
        elif isinstance(pred, re.Pattern):
            rx = pred
            check = lambda lines: rx.search("\n".join(lines))  # noqa: E731
            what = "regex %r" % rx.pattern
        else:
            check = pred
            what = getattr(pred, "__name__", "predicate")
        deadline = time.monotonic() + timeout
        lines = []
        while True:
            lines = self.screen()
            if check(lines):
                return lines
            if not allow_dead and self.pane_dead():
                lines = self.screen()
                if check(lines):
                    return lines
                raise AssertionError("%s: pane command exited while waiting for %s\n%s"
                                     % (msg or "wait_for", what, self.describe(lines)))
            if time.monotonic() >= deadline:
                raise AssertionError("%s: timed out after %.1fs waiting for %s\n%s"
                                     % (msg or "wait_for", timeout, what, self.describe(lines)))
            time.sleep(0.05)

    def wait_dead(self, timeout=TIMEOUT):
        """Wait until the command exited and the pane is dead."""
        try:
            wait_until(lambda: self.dead_status() is not None and self.pane_dead(), timeout)
        except AssertionError:
            raise AssertionError("pane command did not exit within %.0fs\n%s"
                                 % (timeout, self.describe()))

    # -- pmux list-screen specific ------------------------------------------

    def wait_list(self, *names, timeout=TIMEOUT):
        """Wait until the list screen is up and shows rows for all names."""
        def ready(lines):
            if not lines[0].startswith(" ✻ pmux"):
                return False
            shown = {n for _i, _g, n in list_rows(lines)}
            return all(n in shown for n in names)
        return self.wait_for(ready, timeout,
                             msg="list screen with rows %r" % (names,))

    def footer(self, lines=None):
        lines = self.screen() if lines is None else lines
        return lines[-1]

    def selected_lines(self, sel_color, cells=None):
        """Indices of lines whose name column (col 5) carries the
        selection-bar background."""
        cells = self.cells() if cells is None else cells
        out = []
        for i, row in enumerate(cells):
            if len(row) > 5 and row[5].bg is not None and color_close(row[5].bg, sel_color):
                out.append(i)
        return out


def read_text(path):
    return read_file(path).decode(errors="replace")


# ---------------------------------------------------------------------------
# base test case


def winch_marker(name):
    """Screen text a --winch-mark probe prints when (re)attached."""
    return "[winch:%s]" % name


class TuiCase(PmuxTestCase):
    """PmuxTestCase + ~/.pmux/config (CONFIG; None = no file), directories
    ~/work/api and ~/work/web, and a daemon started outside tmux (so it
    never inherits a tmux pane)."""

    CONFIG = "theme = dark\n"
    THEME = DARK

    def setUp(self):
        super().setUp()
        if self.CONFIG is not None:
            self.px.write_config(self.CONFIG)
        self.api = self.mkdir("work/api")
        self.web = self.mkdir("work/web")
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, "daemon start via -l failed: %r" % p.stderr)

    def mkdir(self, rel):
        path = os.path.join(self.px.home, rel)
        os.makedirs(path, exist_ok=True)
        return path

    def new(self, name, *cmd, cwd=None):
        p = self.px.run("-n", name, "-d", "--", *cmd, cwd=cwd or self.api)
        self.assertEqual(p.returncode, 0, "pmux -n %s -d failed: %r" % (name, p.stderr))

    def new_exited(self, name, script, cwd=None):
        self.new(name, "sh", "-c", script, cwd=cwd)
        self.px.wait_state(name, lambda s: s.startswith(("exited:", "signaled:")))

    def marker(self, name, cwd=None, mode="inlog", *args):
        """Probe that prints winch_marker(name) on every SIGWINCH (i.e. on
        every attach).  inlog mode logs its input to files/<name>.in."""
        if mode == "inlog" and not args:
            args = (self.px.path(name + ".in"),)
        self.px.create_probe(name, mode, *args, cwd=cwd or self.api, winch_mark=name)
        return self.px.path(name + ".in")

    def tui(self, **kw):
        t = TmuxTui(self.px, **kw)
        self.addCleanup(t.close)
        return t.start()

    def start_list(self, *names, **kw):
        t = self.tui(**kw)
        t.wait_list(*names)
        return t

    def wait_selected(self, t, name, timeout=TIMEOUT):
        """Wait until exactly the row of `name` carries the selection bar."""
        state = {}

        def check():
            cells = t.cells()
            lines = ["".join(c.ch for c in row).rstrip() for row in cells]
            idx = row_index(lines, name)
            sel = t.selected_lines(self.THEME["sel"], cells)
            state.update(lines=lines, idx=idx, sel=sel, cells=cells)
            return idx is not None and sel == [idx]

        try:
            wait_until(check, timeout, interval=0.05)
        except AssertionError:
            bgs = ["  %2d| col5 bg=%r" % (i, row[5].bg if len(row) > 5 else None)
                   for i, row in enumerate(state.get("cells", []))
                   if ROW_RE.match("".join(c.ch for c in row))]
            self.fail("expected row %r (line %r) to be the only selected row (bar bg %r); "
                      "lines with the bar: %r\nrow backgrounds:\n%s\n%s"
                      % (name, state.get("idx"), self.THEME["sel"], state.get("sel"),
                         "\n".join(bgs), dump(state.get("lines", []))))

    def wait_attached(self, t, name):
        return t.wait_for(winch_marker(name), msg="attach to %s" % name)

    def detach_to_list(self, t, *names):
        t.keys("C-\\")
        lines = t.wait_list(*names)
        self.assertFalse(any("[detached" in l for l in lines),
                         "TUI must not show a [detached ...] message\n" + dump(lines))
        return lines

    def assertColor(self, cell, expected, what):
        self.assertTrue(color_close(cell.fg, expected),
                        "%s: fg %r, expected %r (cell %r)" % (what, cell.fg, expected, cell))
