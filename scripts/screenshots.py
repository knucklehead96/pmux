#!/usr/bin/env python3
"""Generate the README screenshots of the pmux list TUI.

Starts an isolated pmux daemon (temporary HOME and XDG_RUNTIME_DIR), creates
a handful of demo processes in ~/work/api and ~/work/web, runs `pmux` in a
private tmux server, drives it into a few states, converts each
`capture-pane -p -e` dump into an HTML "terminal window" and screenshots that
with headless Chromium.

    python3 scripts/screenshots.py [--pmux build/pmux] [--out docs/screenshots]
                                   [--chrome PATH] [--keep-html]

Output: list-dark.png, list-light.png, filter.png, new-dialog.png,
kill-confirm.png.  Requires tmux, perl (the demo processes use it to set a
plausible command line) and a headless Chromium.  Standard library only.
"""
import argparse
import glob
import html
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHROME_NAMES = ("chrome-headless-shell", "chromium", "chromium-browser",
                "google-chrome", "google-chrome-stable")
PLAYWRIGHT_GLOBS = ("~/.cache/ms-playwright/*/chrome-headless-shell-linux64/chrome-headless-shell",
                    "~/.cache/ms-playwright/*/chrome-linux64/chrome")

ROWS, COLS = 24, 100
FOOTER_LIST = "enter attach"
DIALOG_HINT = "enter create · tab next · esc cancel"

# Demo processes: (name, dir under ~/work, argv).  The commands are perl
# shims installed in ~/.local/bin of the temporary HOME; each sets `$0` so
# /proc/<pid>/cmdline (the list's "foreground command") reads like the real
# tool.
SHIM_HEAD = '#!/usr/bin/perl\n$0 = join " ", "%s", @ARGV; $| = 1;\n'
SHIMS = {
    "npm": 'my @r = ("GET /health 200 2ms", "GET /v1/users 200 14ms", "POST /v1/users 201 18ms",'
           ' "GET /v1/orders?page=2 200 31ms");'
           ' print "> api\\@1.0.0 dev\\n> node server.js\\n\\nlistening on :3000\\n";'
           ' for (my $i = 0;; $i++) { print $r[$i % @r], "\\n"; sleep 1 }',
    "cargo": 'for (;;) { print "[Running \'cargo check\']\\n    Finished `dev` profile in 1.84s\\n"; sleep 1 }',
    "claude": 'print "> run the test suite and fix any failures\\n"; sleep 1; print "\\a"; sleep 86400',
    "mkdocs": 'print "Serving on http://127.0.0.1:8000/\\n"; sleep 86400',
    "node": 'print "[migrate] applying 0007_add_user_roles.sql ... ok\\n";'
            ' print "error: column \\"role_id\\" of relation \\"users\\" does not exist\\n"; exit 1',
}
DEMO = [
    ("api-server", "api", ["npm", "run", "dev"]),
    ("scratch", "api", ["bash"]),
    ("migrate-db", "api", ["node", "scripts/migrate.js"]),
    ("build-watch", "web", ["cargo", "watch"]),
    ("tests", "web", ["claude"]),
    ("docs-preview", "web", ["mkdocs", "serve"]),
]
KILLED = "docs-preview"   # ends by SIGHUP: shows the signaled state
KILL_PROMPT = "api-server"  # the kill screenshot's "press ctrl+x again" target

# ---------------------------------------------------------------------------
# themes for the HTML rendering

THEMES = {
    "dark": {"bg": "#1E1E1E", "fg": "#D4D4D4", "chrome": "#2B2B2B", "title": "#8B8B8B",
             "border": "rgba(255,255,255,.10)"},
    "light": {"bg": "#FAFAF7", "fg": "#1F1F1F", "chrome": "#EAEAE6", "title": "#6B6B6B",
              "border": "rgba(0,0,0,.18)"},
}

# 16 base colors (VS Code terminal palette), then the xterm 6x6x6 cube and gray ramp.
BASE16 = ["#000000", "#CD3131", "#0DBC79", "#E5E510", "#2472C8", "#BC3FBC", "#11A8CD", "#E5E5E5",
          "#666666", "#F14C4C", "#23D18B", "#F5F543", "#3B8EEA", "#D670D6", "#29B8DB", "#E5E5E5"]


def palette(n):
    if n < 16:
        return BASE16[n]
    if n < 232:
        n -= 16
        lv = [0, 95, 135, 175, 215, 255]
        return "#%02X%02X%02X" % (lv[n // 36], lv[n // 6 % 6], lv[n % 6])
    v = 8 + 10 * (n - 232)
    return "#%02X%02X%02X" % (v, v, v)


FONT_PX = 14
CHAR_EM = 0.6021   # DejaVu Sans Mono advance width (1233 / 2048 em)
LINE_EM = 1.45
PAD = 14           # terminal padding
MARGIN = 24        # transparent margin around the window (room for the shadow)
TITLEBAR = 28
BOX_SCALE = 1.28   # stretch box-drawing glyphs (1.164 em tall) to the line height


# ---------------------------------------------------------------------------
# ANSI -> styled cells


def default_style():
    return {"fg": None, "bg": None, "bold": False, "dim": False, "italic": False,
            "underline": False, "reverse": False, "strike": False}


def apply_sgr(st, params):
    ps = params.split(";") if params else ["0"]
    i = 0
    while i < len(ps):
        sub = ps[i].split(":")
        p = int(sub[0]) if sub[0].isdigit() else 0
        if p == 0:
            st.update(default_style())
        elif p == 1:
            st["bold"] = True
        elif p == 2:
            st["dim"] = True
        elif p == 3:
            st["italic"] = True
        elif p == 4:
            st["underline"] = not (len(sub) > 1 and sub[1] == "0")
        elif p == 7:
            st["reverse"] = True
        elif p == 9:
            st["strike"] = True
        elif p == 22:
            st["bold"] = st["dim"] = False
        elif p == 23:
            st["italic"] = False
        elif p == 24:
            st["underline"] = False
        elif p == 27:
            st["reverse"] = False
        elif p == 29:
            st["strike"] = False
        elif 30 <= p <= 37:
            st["fg"] = palette(p - 30)
        elif 90 <= p <= 97:
            st["fg"] = palette(p - 90 + 8)
        elif 40 <= p <= 47:
            st["bg"] = palette(p - 40)
        elif 100 <= p <= 107:
            st["bg"] = palette(p - 100 + 8)
        elif p == 39:
            st["fg"] = None
        elif p == 49:
            st["bg"] = None
        elif p in (38, 48, 58):
            color = None
            if len(sub) > 1:   # colon form 38:2::r:g:b / 38:5:n
                vals = [int(v) if v.isdigit() else 0 for v in sub[1:]]
                if vals and vals[0] == 5 and len(vals) > 1:
                    color = palette(vals[1])
                elif vals and vals[0] == 2 and len(vals) >= 4:
                    color = "#%02X%02X%02X" % tuple(vals[-3:])
            elif i + 2 < len(ps) and ps[i + 1] == "5":
                color = palette(int(ps[i + 2]))
                i += 2
            elif i + 4 < len(ps) and ps[i + 1] == "2":
                color = "#%02X%02X%02X" % tuple(int(v) for v in ps[i + 2:i + 5])
                i += 4
            if p != 58:
                st["fg" if p == 38 else "bg"] = color
        i += 1


ESC_RE = re.compile(r"\x1b(?:\[([0-9;:?]*)([A-Za-z])|\][^\x07\x1b]*(?:\x07|\x1b\\)|.)")


def parse_line(line, st):
    """[(char, style)] for one capture-pane -e line.  tmux only emits
    attribute changes, so the SGR state `st` carries over between lines."""
    cells, pos = [], 0
    for m in ESC_RE.finditer(line):
        cells += [(ch, dict(st)) for ch in line[pos:m.start()]]
        if m.group(2) == "m":
            apply_sgr(st, m.group(1))
        pos = m.end()
    cells += [(ch, dict(st)) for ch in line[pos:]]
    return cells


def wide(ch):
    import unicodedata
    return unicodedata.east_asian_width(ch) in ("W", "F")


def mix(a, b, t):
    a, b = a.lstrip("#"), b.lstrip("#")
    ca = [int(a[i:i + 2], 16) for i in (0, 2, 4)]
    cb = [int(b[i:i + 2], 16) for i in (0, 2, 4)]
    return "#%02X%02X%02X" % tuple(round(x * (1 - t) + y * t) for x, y in zip(ca, cb))


def css(st, theme):
    fg, bg = st["fg"] or theme["fg"], st["bg"]
    if st["reverse"]:
        fg, bg = bg or theme["bg"], fg
    if st["dim"]:
        fg = mix(fg, bg or theme["bg"], 0.5)
    out = []
    if fg != theme["fg"]:
        out.append("color:" + fg)
    if bg:
        out.append("background:" + bg)
    if st["bold"]:
        out.append("font-weight:700")
    if st["italic"]:
        out.append("font-style:italic")
    deco = [d for d, on in (("underline", st["underline"]), ("line-through", st["strike"])) if on]
    if deco:
        out.append("text-decoration:" + " ".join(deco))
    return ";".join(out)


def run_html(text):
    """Escape a style run; non-ASCII glyphs get a fixed-width box so a
    fallback font cannot break the column grid."""
    out = []
    for ch in text:
        if ord(ch) < 128:
            out.append(html.escape(ch))
        else:
            cls = "w2" if wide(ch) else "w bx" if 0x2500 <= ord(ch) <= 0x257F else "w"
            out.append('<i class="%s">%s</i>' % (cls, html.escape(ch)))
    return "".join(out)


def row_html(line, st, theme):
    cells = parse_line(line, st)
    width = sum(2 if wide(ch) else 1 for ch, _ in cells)
    cells += [(" ", default_style())] * max(0, COLS - width)
    runs = []   # [css, text]
    for ch, st in cells:
        c = css(st, theme)
        if runs and runs[-1][0] == c:
            runs[-1][1] += ch
        else:
            runs.append([c, ch])
    spans = "".join('<span style="%s">%s</span>' % (c, run_html(t)) if c else
                    "<span>%s</span>" % run_html(t) for c, t in runs)
    return '<div class="r">%s</div>' % spans


def page_size():
    w = COLS * CHAR_EM * FONT_PX + 2 * PAD + 2 + 2 * MARGIN
    h = ROWS * LINE_EM * FONT_PX + 2 * PAD + TITLEBAR + 2 + 2 * MARGIN
    return int(w + 0.999), int(h + 0.999)


def to_html(lines, theme_name, title):
    t = THEMES[theme_name]
    lh = LINE_EM * FONT_PX
    st = default_style()
    rows = "".join(row_html(l, st, t) for l in (lines + [""] * ROWS)[:ROWS])
    return f"""<!doctype html><meta charset="utf-8">
<style>
html, body {{ margin: 0; background: transparent; }}
.win {{ margin: {MARGIN}px; border-radius: 10px; overflow: hidden; background: {t['bg']};
        border: 1px solid {t['border']}; box-shadow: 0 6px 18px rgba(0,0,0,.28); width: max-content; }}
.bar {{ height: {TITLEBAR}px; background: {t['chrome']}; position: relative; display: flex; align-items: center; }}
.dots {{ display: flex; gap: 7px; padding-left: 12px; }}
.dots b {{ width: 11px; height: 11px; border-radius: 50%; display: block; }}
.title {{ position: absolute; left: 0; right: 0; text-align: center; color: {t['title']};
          font: 12px system-ui, "DejaVu Sans", sans-serif; }}
.term {{ padding: {PAD}px; color: {t['fg']}; font-family: "DejaVu Sans Mono", monospace;
         font-size: {FONT_PX}px; line-height: {lh}px; font-variant-ligatures: none; }}
.r {{ height: {lh}px; white-space: pre; width: {COLS}ch; }}
.r span {{ display: inline-block; height: {lh}px; vertical-align: top; text-underline-offset: 3px; }}
.r i {{ font-style: inherit; display: inline-block; width: 1ch; text-align: center; overflow: visible; }}
.r i.w2 {{ width: 2ch; }}
.r i.bx {{ transform: scaleY({BOX_SCALE}); }}
</style>
<div class="win"><div class="bar"><div class="dots"><b style="background:#FF5F57"></b><b style="background:#FEBC2E"></b><b style="background:#28C840"></b></div><div class="title">{html.escape(title)}</div></div>
<div class="term">{rows}</div></div>
"""


# ---------------------------------------------------------------------------
# isolated pmux + tmux


class Env:
    def __init__(self, pmux, keep=False):
        self.pmux = pmux
        # Short path: AF_UNIX socket paths are limited to 108 bytes.
        self.root = os.path.realpath(tempfile.mkdtemp(prefix="pmuxshot-", dir="/tmp"))
        self.runtime = os.path.join(self.root, "run")
        self.home = os.path.join(self.root, "home")
        os.mkdir(self.runtime, 0o700)
        os.makedirs(os.path.join(self.home, ".pmux"))
        bindir = os.path.join(self.home, ".local", "bin")
        os.makedirs(bindir)
        for tool, body in SHIMS.items():
            path = os.path.join(bindir, tool)
            with open(path, "w") as f:
                f.write(SHIM_HEAD % tool + body + "\n")
            os.chmod(path, 0o755)
        self.env = {
            "PATH": bindir + ":" + os.environ.get("PATH", "/usr/local/bin:/usr/bin:/bin"),
            "HOME": self.home,
            "XDG_RUNTIME_DIR": self.runtime,
            "SHELL": "/bin/bash",
            "TERM": "xterm-256color",
            "COLORTERM": "truecolor",
            "LANG": "C.UTF-8",
            "PS1": "$ ",
        }
        self.tmux_sock = "pmuxshot-%d" % os.getpid()
        self.tmux_started = False
        self.log = open(os.path.join(self.root, "pmux.log"), "wb")

    def config(self, theme):
        with open(os.path.join(self.home, ".pmux", "config"), "w") as f:
            f.write("default_dir = ~/work\ntheme = %s\n" % theme)

    def run(self, *args, cwd=None, check=True):
        env = dict(self.env, PWD=cwd or self.home)
        # Output goes to a file, not a pipe: the daemon spawned by the first
        # command must not hold our pipe open.
        p = subprocess.run([self.pmux, *args], cwd=cwd or self.home, env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if args == ("-l",) else self.log,
                           stderr=self.log, timeout=30)
        if check and p.returncode != 0:
            raise RuntimeError("pmux %s failed (%d), see %s" % (" ".join(args), p.returncode, self.log.name))
        return p.stdout.decode() if p.stdout else ""

    def states(self):
        out = {}
        for line in self.run("-l").splitlines():
            f = line.split("\t")
            if len(f) >= 2:
                out[f[0]] = f[1]
        return out

    def tmux(self, *args):
        p = subprocess.run(["tmux", "-L", self.tmux_sock, "-f", "/dev/null", *args],
                           env=dict(self.env, TMUX_TMPDIR=self.root), stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=30)
        if p.returncode != 0:
            raise RuntimeError("tmux %s: %s" % (" ".join(args), p.stderr.decode(errors="replace")))
        return p.stdout.decode()

    def tui(self, session):
        """Start `pmux` (the list) in a new tmux session."""
        opts = []
        if not self.tmux_started:
            opts = ["start-server", ";", "set-option", "-g", "exit-empty", "off", ";",
                    "set-option", "-g", "default-terminal", "tmux-256color", ";",
                    "set-option", "-g", "status", "off", ";"]
            self.tmux_started = True
        self.tmux(*opts, "new-session", "-d", "-s", session, "-x", str(COLS), "-y", str(ROWS),
                  "-c", os.path.join(self.home, "work"),
                  "-e", "COLORTERM=truecolor", "-e", "PWD=" + os.path.join(self.home, "work"),
                  "--", self.pmux)

    def keys(self, session, *keys, literal=False):
        self.tmux("send-keys", "-t", session, *(["-l"] if literal else []), *keys)

    def capture(self, session):
        return self.tmux("capture-pane", "-p", "-e", "-N", "-t", session).split("\n")[:ROWS]

    def text(self, session):
        return self.tmux("capture-pane", "-p", "-t", session).split("\n")[:ROWS]

    def wait(self, session, pred, what, timeout=10):
        end = time.time() + timeout
        while True:
            lines = self.text(session)
            if pred(lines):
                time.sleep(0.3)   # let FTXUI finish the frame
                return
            if time.time() > end:
                raise RuntimeError("timed out waiting for %s; screen:\n%s" % (what, "\n".join(lines)))
            time.sleep(0.1)

    def wait_gone(self, session, what, timeout=10):
        """Waits for the session to end (its pmux exited)."""
        end = time.time() + timeout
        while subprocess.run(["tmux", "-L", self.tmux_sock, "-f", "/dev/null", "has-session", "-t", session],
                             env=dict(self.env, TMUX_TMPDIR=self.root), capture_output=True).returncode == 0:
            if time.time() > end:
                raise RuntimeError("timed out waiting for %s" % what)
            time.sleep(0.1)

    def cleanup(self):
        if self.tmux_started:
            subprocess.run(["tmux", "-L", self.tmux_sock, "-f", "/dev/null", "kill-server"],
                           env=dict(self.env, TMUX_TMPDIR=self.root), capture_output=True)
        pidfile = os.path.join(self.runtime, "pmux", "pmux.pid")
        try:
            os.kill(int(open(pidfile).read().strip()), signal.SIGTERM)
        except (OSError, ValueError):
            pass
        # Anything still carrying our XDG_RUNTIME_DIR (demo processes, the
        # daemon) belongs to this run.
        mark = ("XDG_RUNTIME_DIR=%s" % self.runtime).encode()
        for _ in range(20):
            left = []
            for pid in filter(str.isdigit, os.listdir("/proc")):
                if int(pid) == os.getpid():
                    continue
                try:
                    with open("/proc/%s/environ" % pid, "rb") as f:
                        if mark in f.read().split(b"\0"):
                            left.append(int(pid))
                except OSError:
                    pass
            if not left:
                break
            for pid in left:
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            time.sleep(0.1)
        self.log.close()
        shutil.rmtree(self.root, ignore_errors=True)


def contains(s):
    return lambda lines: any(s in l for l in lines)


def wait_states(env, want, timeout):
    end = time.time() + timeout
    while True:
        st = env.states()
        if all(st.get(n, "").startswith(v) for n, v in want.items()):
            return st
        if time.time() > end:
            raise RuntimeError("timed out waiting for process states: %r" % st)
        time.sleep(0.2)


def shoot(chrome, html_text, png, scale, workdir):
    path = os.path.join(workdir, os.path.basename(png) + ".html")
    with open(path, "w", encoding="utf-8") as f:
        f.write(html_text)
    w, h = page_size()
    p = subprocess.run([chrome, "--headless", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                        "--default-background-color=00000000", "--force-device-scale-factor=%g" % scale,
                        "--window-size=%d,%d" % (w, h), "--screenshot=" + os.path.abspath(png),
                        "file://" + path], capture_output=True, timeout=60)
    if p.returncode != 0 or not os.path.exists(png):
        raise RuntimeError("chrome failed: %s" % p.stderr.decode(errors="replace"))


def find_chrome(explicit):
    """--chrome, then $CHROME, then well-known names on PATH, then a
    Playwright-installed Chromium; None if nothing is found."""
    for cand in (explicit, os.environ.get("CHROME")):
        if cand:
            return shutil.which(cand) or cand
    for name in CHROME_NAMES:
        path = shutil.which(name)
        if path:
            return path
    for pattern in PLAYWRIGHT_GLOBS:
        hits = sorted(p for p in glob.glob(os.path.expanduser(pattern)) if os.access(p, os.X_OK))
        if hits:
            return hits[-1]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pmux", default=os.path.join(REPO, "build", "pmux"))
    ap.add_argument("--out", default=os.path.join(REPO, "docs", "screenshots"))
    ap.add_argument("--chrome", help="headless Chromium binary (default: $CHROME, PATH, Playwright cache)")
    ap.add_argument("--scale", type=float, default=2, help="device scale factor (default 2)")
    ap.add_argument("--keep-html", action="store_true", help="also write the .html pages to --out")
    a = ap.parse_args()

    pmux = os.path.abspath(a.pmux)
    a.chrome = find_chrome(a.chrome)
    if not a.chrome:
        sys.exit("screenshots.py: no headless Chromium found; pass --chrome PATH or set $CHROME "
                 "(looked for %s on PATH and in ~/.cache/ms-playwright)" % ", ".join(CHROME_NAMES))
    for exe, what in ((pmux, "--pmux"), (a.chrome, "--chrome / $CHROME")):
        if not os.access(exe, os.X_OK):
            sys.exit("screenshots.py: %s is not executable (%s)" % (exe, what))
    for tool in ("tmux", "perl"):
        if not shutil.which(tool):
            sys.exit("screenshots.py: %s not found" % tool)
    os.makedirs(a.out, exist_ok=True)

    env = Env(pmux)
    try:
        env.config("dark")
        for name, sub, argv in DEMO:
            d = os.path.join(env.home, "work", sub)
            os.makedirs(d, exist_ok=True)
            env.run("-n", name, "-d", "--", *argv, cwd=d)
            time.sleep(0.05)   # distinct creation order
        wait_states(env, {"migrate-db": "exited", "tests": "running"}, 10)
        # `pmux -k` removes the process; signal it directly to keep a signaled row.
        pid = int(next(l.split("\t")[2] for l in env.run("-l").splitlines()
                       if l.split("\t")[0] == KILLED))
        os.killpg(pid, signal.SIGHUP)
        wait_states(env, {"migrate-db": "exited:1", KILLED: "signaled"}, 15)
        time.sleep(0.5)

        shots = []   # (file, theme, lines)
        s = "dark"
        env.tui(s)
        env.wait(s, contains(FOOTER_LIST), "the list")
        env.wait(s, contains("· bell"), "the bell")
        shots.append(("list-dark.png", "dark", env.capture(s)))

        env.keys(s, "build", literal=True)
        env.wait(s, contains("› build"), "the filter")
        shots.append(("filter.png", "dark", env.capture(s)))
        env.keys(s, "Escape")
        env.wait(s, contains(FOOTER_LIST + "  ctrl+n new"), "the filter to clear")
        env.keys(s, *["Up"] * len(DEMO))   # back to the first row (FTXUI ignores tmux's Home, ESC [ 1 ~)
        time.sleep(0.5)

        env.keys(s, "C-n")
        env.wait(s, contains(DIALOG_HINT), "the new-process dialog")
        env.keys(s, "fix-flaky-tests", literal=True)
        env.wait(s, contains("fix-flaky-tests"), "the typed name")
        shots.append(("new-dialog.png", "dark", env.capture(s)))
        env.keys(s, "Escape")
        env.wait(s, lambda ls: not contains(DIALOG_HINT)(ls), "the dialog to close")

        env.keys(s, "C-x")
        env.wait(s, contains("press ctrl+x again to kill " + KILL_PROMPT), "the kill prompt")
        shots.append(("kill-confirm.png", "dark", env.capture(s)))
        env.keys(s, "Escape")   # any other key cancels
        env.wait(s, contains(FOOTER_LIST), "the kill prompt to go away")

        env.keys(s, "C-c")
        env.wait(s, contains("press ctrl+c again to quit"), "the quit prompt")
        env.keys(s, "C-c")
        env.wait_gone(s, "the list to quit")

        env.config("light")
        s = "light"
        env.tui(s)
        env.wait(s, contains(FOOTER_LIST), "the light list")
        shots.append(("list-light.png", "light", env.capture(s)))

        for fname, theme, lines in shots:
            page = to_html(lines, theme, "pmux — ~/work")
            png = os.path.join(a.out, fname)
            shoot(a.chrome, page, png, a.scale, env.root)
            if a.keep_html:
                with open(png[:-4] + ".html", "w", encoding="utf-8") as f:
                    f.write(page)
            print("wrote %s (%d KB)" % (png, os.path.getsize(png) // 1024))
    finally:
        env.cleanup()


if __name__ == "__main__":
    main()
