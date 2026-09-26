"""Polish: faint restore, keys typed during the startup theme
query, self-attach refusal, too-small terminal, Release build + install."""
import os
import re
import shutil
import subprocess
import tempfile
import time
import unittest

from helpers import (FAST, PMUX_BIN, REPO_DIR, TIMEOUT, probe, read_file, read_until,
                     wait_until)
from test_restore import WINCH_MARK, RestoreCase
from tui import DARK, TuiCase, color_close, dump, row_index

# Row 0: faint, then normal.  Row 1: colors whose parameters contain a 2 that
# is not SGR 2 (semicolon and colon forms, 256-color index 2).  Row 2:
# bold + faint.  Row 3: SGR 22 clears faint.  Row 4: underline colors (58)
# must not leak faint (or anything else) either.
FAINT_SCRIPT = (
    b"\x1b[H\x1b[2J"
    b"\x1b[2mdim\x1b[0m normal"
    b"\x1b[2;1H\x1b[38;2;2;2;2mX\x1b[0m \x1b[38:2::2:2:2mY\x1b[0m \x1b[38;5;2mZ\x1b[0m"
    b" \x1b[48;2;2;2;2mW\x1b[0m \x1b[38:5:2mV\x1b[0m"
    b"\x1b[3;1H\x1b[1;2mboldfaint\x1b[0m"
    b"\x1b[4;1H\x1b[2mA\x1b[22mB\x1b[1;2mC\x1b[22mD\x1b[0m"
    b"\x1b[5;1H\x1b[58;5;2mU\x1b[58;2;2;2;2mU\x1b[0m"
    b"\x1b[6;1Hend"
)

# (row, col) of every cell drawn faint by FAINT_SCRIPT.
FAINT_CELLS = {(0, 0), (0, 1), (0, 2), (3, 0)} | {(2, c) for c in range(9)} | {(3, 2)}

SGR_RE = re.compile(rb"\x1b\[([0-9;:]*)m")


def sgr_params(data):
    """Every top-level SGR parameter in data (colon sub-parameters kept)."""
    return [p for m in SGR_RE.finditer(data) for p in m.group(1).split(b";")]


class FaintRestore(RestoreCase):
    def check_faint(self, state, what):
        rows = state["cells"]
        for r in range(6):
            for c, key in enumerate(rows[r] if r < len(rows) else []):
                if key[0] == " ":
                    continue
                self.assertEqual(key[4], (r, c) in FAINT_CELLS,
                                 "%s: cell (%d,%d) %r faint=%r\n%s"
                                 % (what, r, c, key[0], key[4], dump(state["text"])))

    def test_faint_survives_reattach(self):
        ref = self.reference(FAINT_SCRIPT)
        self.check_faint(ref, "reference pane")
        self.assertTrue(ref["cells"][2][0][3], "reference: bold+faint is bold")
        self.draw("faint", FAINT_SCRIPT)
        for i in (1, 2):
            a = self.attach_pane("faint")
            self.assertPaneMatches(a, ref, "attach #%d" % i)
            self.check_faint({"cells": [[c.key() for c in row] for row in a.cells()],
                              "text": a.screen()}, "attach #%d" % i)
            self.detach_pane(a, "faint")

    def test_faint_in_snapshot_bytes_and_history(self):
        # Faint lines scrolled into history take libvterm's pushline path.
        script = (b"\x1b[2mhist-dim\x1b[0m hist-normal\r\n" + b"".join(
            b"line %d\r\n" % i for i in range(40)) + b"\x1b[2mscreen-dim\x1b[0m\x1b[1;2m")
        self.draw("snap", script, winch=WINCH_MARK)
        c, snap, _ = self.attach_raw("snap")
        self.assertIn(b"\x1b[0;2mhist-dim\x1b[0m hist-normal", snap)
        self.assertIn(b"\x1b[0;2mscreen-dim\x1b[0m", snap)
        self.assertTrue(snap.endswith(b"\x1b[0;1;2m"),
                        "snapshot must end with the app's pen (bold+faint): %r" % snap[-40:])
        bad = [p for p in sgr_params(snap) if p.split(b":")[0] in (b"10", b"11")]
        self.assertEqual(bad, [], "snapshot must never contain SGR 10/11")
        self.detach_raw(c, "snap")

    def test_attached_output_is_byte_exact(self):
        corpus = (FAINT_SCRIPT + b"\x1b[2;22;2m\x1b[;2m\x1b[02mq\x1b[0m\x1b[>4;2m\x1b[?2m"
                  + b"\x1b]0;\x1b[2m\x07" + b"\x1b[" + b"2;" * 40 + b"2m\x1b[0m\r\n"
                  # More than 16 CSI arguments crash libvterm 0.3.3 unless pmux trims them.
                  + b"\x1b[" + b"1:" * 40 + b"1m\x1b[0m\x1b[?" + b"1;" * 40 + b"1l"
                  + b"\x1b[" + b"1;" * 3000 + b"1H\x1b[" + b";" * 5000 + b"m\r\n")
        cpath = self.file(corpus, "corpus")
        self.px.create_probe("emit", "emit", cpath)
        c = self.px.attach("emit")
        c.send(probe.START_BYTE)
        data = read_until(c, probe.END_MARKER)
        start = data.rfind(probe.START_MARKER)
        self.assertGreaterEqual(start, 0, "START marker missing")
        self.assertBytesEqual(corpus, data[start + len(probe.START_MARKER):-len(probe.END_MARKER)],
                              "attached output")
        self.detach(c, "emit")
        self.assertEqual(self.px.entry("emit")["state"], "running", "daemon survived")


class TypedDuringStartup(TuiCase):
    CONFIG = None  # theme auto: the TUI queries the terminal at startup

    def test_keys_typed_at_launch_take_effect(self):
        for n in ("app-1", "app-2", "app-3"):
            self.new(n, "sleep", "600")
        # The keys are typed right after launch but before pmux reads its terminal: they wait
        # in the tty's input queue and are read by the OSC 11 / DA1 query loop.
        t = self.tui(cmd=["/bin/sh", "-c", 'sleep 0.5; exec "$0"', PMUX_BIN])
        t.keys("Down")
        t.type("app")
        lines = t.wait_for("› app▏", msg="filter typed at launch")
        self.assertIn("app-1", "\n".join(lines))
        self.wait_selected(t, "app-2")
        self.assertEqual(t.screen()[2], " › app▏", "only the typed keys reach the filter")


class SelfAttach(TuiCase):
    def test_cli_refuses_self_attach(self):
        err, rc = self.px.path("self.err"), self.px.path("self.rc")
        self.new("self", "sh", "-c", '"$0" -a self 2>"$1"; echo $? >"$2"; exec sleep 600',
                 PMUX_BIN, err, rc)
        wait_until(lambda: read_file(rc).strip(), msg="nested pmux -a never returned")
        self.assertEqual(read_file(rc).strip(), b"1")
        self.assertEqual(read_file(err), b"pmux: cannot attach self to itself\n")
        self.assertEqual(self.px.entry("self")["state"], "running", "process unaffected")

    def test_nested_attach_to_other_process_allowed(self):
        self.marker("target")
        self.new("outer", PMUX_BIN, "-a", "target")
        # The nested client attaches: the target probe draws its winch marker, which the
        # outer process's screen shows when we attach to it.
        o = self.tui(args=("-a", "outer"))
        o.wait_for("[winch:target]", msg="nested attach to target")

    def test_tui_refuses_self_attach(self):
        self.new("tuiself", PMUX_BIN)
        t = self.tui(args=("-a", "tuiself"))
        t.wait_list("tuiself")
        t.keys("Enter")
        lines = t.wait_for("cannot attach tuiself to itself", msg="footer note")
        self.assertIn("tuiself", "\n".join(lines))
        self.assertTrue(lines[0].startswith(" ✻ pmux"), dump(lines))
        foot = t.cells()[-1]
        self.assertTrue(color_close(foot[1].fg, DARK["error"]), "note must be red: %r" % foot[1])
        t.keys("C-Left")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 0, t.describe())


class TooSmall(TuiCase):
    def small(self, lines, rows, cols):
        y = (rows - 2) // 2
        self.assertEqual(lines[y], " " * ((cols - 18) // 2) + "Terminal too small", dump(lines))
        self.assertEqual(lines[y + 1], " " * ((cols - 11) // 2) + "ctrl+c quit", dump(lines))
        self.assertEqual([l for i, l in enumerate(lines) if l and i not in (y, y + 1)], [],
                         dump(lines))

    def test_message_keys_and_recovery(self):
        self.new("proc", "sleep", "600")
        t = self.tui(rows=6, cols=30)
        self.small(t.wait_for("Terminal too small"), 6, 30)
        t.keys("C-n")
        t.type("x")
        t.keys("C-x", "C-x")
        t.keys("C-q")
        time.sleep(0.3)
        self.assertFalse(t.pane_dead(), "Ctrl+Q must not quit\n" + t.describe())
        t.resize(30, 100)
        lines = t.wait_list("proc")
        self.assertFalse(any("New process" in l or "› x" in l or "press ctrl+x" in l for l in lines),
                         "keys must be ignored while too small\n" + dump(lines))
        self.assertEqual(self.px.entry("proc")["state"], "running", "Ctrl+X twice while too small")
        t.resize(20, 39)
        self.small(t.wait_for("Terminal too small"), 20, 39)
        t.resize(7, 80)
        self.small(t.wait_for("Terminal too small"), 7, 80)
        t.resize(8, 40)
        t.wait_list("proc")
        t.resize(6, 30)
        t.wait_for("Terminal too small")
        t.keys("C-c")
        time.sleep(0.3)
        self.assertFalse(t.pane_dead(), "a single Ctrl+C must not quit\n" + t.describe())
        t.keys("C-c")
        t.wait_dead()
        self.assertEqual(t.dead_status(), 0, t.describe())
        self.assertEqual(t.modes(), ("0", "0"), "terminal not restored")


@unittest.skipIf(FAST, "slow (PMUX_FAST=1)")
class ReleaseInstall(unittest.TestCase):
    def test_release_build_and_install(self):
        tmp = tempfile.mkdtemp(prefix="pmux-rel-")
        self.addCleanup(shutil.rmtree, tmp, True)
        build, prefix = os.path.join(tmp, "build"), os.path.join(tmp, "inst")
        configure = ["cmake", "-S", REPO_DIR, "-B", build]
        # Reuse the already fetched FTXUI sources (no network needed).
        src = os.path.join(os.path.dirname(PMUX_BIN), "_deps", "ftxui-src")
        if os.path.isdir(src):
            configure.append("-DFETCHCONTENT_SOURCE_DIR_FTXUI=" + src)
        for cmd in (configure, ["cmake", "--build", build, "-j"],
                    ["cmake", "--install", build, "--prefix", prefix]):
            p = subprocess.run(cmd, capture_output=True, timeout=900)
            self.assertEqual(p.returncode, 0, "%s failed:\n%s%s" % (
                " ".join(cmd), p.stdout.decode(errors="replace")[-3000:],
                p.stderr.decode(errors="replace")[-3000:]))
            self.assertNotIn(b"warning:", p.stdout + p.stderr, " ".join(cmd))
        cache = read_file(os.path.join(build, "CMakeCache.txt")).decode()
        self.assertIn("CMAKE_BUILD_TYPE:STRING=Release", cache)
        installed = sorted(os.path.relpath(os.path.join(d, f), prefix)
                           for d, _, fs in os.walk(prefix) for f in fs)
        self.assertEqual(installed, ["bin/pmux"])
        p = subprocess.run([os.path.join(prefix, "bin", "pmux"), "-h"], capture_output=True,
                           timeout=TIMEOUT)
        self.assertEqual(p.returncode, 0)
        self.assertIn(b"pmux -a <name>", p.stdout)


if __name__ == "__main__":
    unittest.main()
