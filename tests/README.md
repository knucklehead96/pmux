# pmux tests

Black-box tests for Milestone 1 (daemon, `-n`/`-l`/`-k`/`-a`, attach
passthrough), Milestone 2 (list TUI, config file) and Milestone 3 (libvterm
screen / scrollback / mode restore).

    tests/run.sh                          # all tests, from anywhere
    tests/run.sh -k Detach                # unittest name filter
    tests/run.sh -k test_tui              # single module
    (cd tests && python3 -m unittest -v test_cli)   # plain unittest

Requirements: python3 (stdlib `unittest`), `pexpect`, and `tmux` (3.x) for
the TUI tests.

Environment variables:

- `PMUX_BIN` — pmux binary under test (default `<repo>/build/pmux`).
- `PMUX_FAST=1` — skip slow tests (the >5 s running→idle transitions).
- `PMUX_TEST_TMPDIR` — parent for per-test temp dirs (default `/tmp`; keep it
  short, the socket paths must fit in 108 bytes).

Each test gets its own `XDG_RUNTIME_DIR`/`HOME`, so it never touches a real
pmux daemon or `~/.pmux/config`. Teardown kills the test daemon and every
process carrying that test's `XDG_RUNTIME_DIR` (including the test's tmux
server).

Files:

- `helpers.py` — `PmuxEnv` fixture, polling, hex diffs, input corpus.
- `probe.py` — the app run inside pmux; see its docstring for modes.
  `--winch-mark NAME` prints `[winch:NAME]` on every SIGWINCH, i.e. on
  every attach, which is how TUI tests see that an attach happened.
  `draw <script> [winch=F] [wlog=P] [usr1=F] [inlog=P] [exit=N]` writes a
  script once and never reacts to input (M3 tests: everything on screen
  after an attach comes from pmux's snapshot).
- `tui.py` — `TmuxTui`: runs `pmux` in a private tmux server
  (`tmux -L <unique> -f /dev/null`, `TMUX_TMPDIR` in the test's temp dir,
  100x30, `tmux-256color`, `COLORTERM=truecolor`) with `keys()` / `type()` /
  `raw()` (exact bytes via `send-keys -H`) / mouse helpers, `screen()` /
  `cells()` (capture-pane with SGR parsed per cell) and `wait_for()`; plus
  theme colors and the `TuiCase` base class.  `fmt()` / `cursor()` /
  `history()` read tmux format flags, the cursor and the full native
  scrollback; `Cell.key()` includes italic and strikethrough. TUI tests write
  `~/.pmux/config` with `theme = dark` unless they test themes.
- `test_cli.py`, `test_attach.py` — M1.
- `test_tui.py` — M2 list screen: layout, colors/themes, keys, attach and
  detach from the list, new/rename/kill dialogs, filter, mouse, refresh
  (bell/idle/exit), quit and terminal restore, passthrough through the TUI
  (pexpect). `HarnessSelfTest` validates `tui.py` without pmux.
- `test_config.py` — M2 config: warnings, `default_cmd`, `default_dir`.
- `test_restore.py` — M3: screen/attribute/cursor restore (CLI and TUI),
  output while detached, native scrollback and `scrollback_lines`
  (incl. 0 and invalid values), alt screen, modes, OSC 4/10/11/12 replay and
  reset, title, snapshot prologue / no RIS, query replies (detached:
  libvterm; attached: the real terminal only), bell only while detached,
  exited process's final screen, resize between attaches, snapshot time for
  10k history lines (printed as `[perf]`), `less`.  The oracle for screen
  state is a reference tmux pane running the same probe script without
  pmux.  Raw tests find the end of the snapshot by a DCS marker the probe
  prints on SIGWINCH (pmux signals the app on attach, after the snapshot).
  `RestoreHarnessSelfTest` validates the probe and oracle without pmux.

Notes: a detached tmux does not answer OSC 11, so pmux uses its assumed
background (#1E1E1E dark / #FAFAF7 light) and the selection-bar color is
computed from that. tmux 3.4 occasionally fails to reap a dead pane's
process, so `TmuxTui` records the exit status through a tiny `sh` wrapper
instead of `#{pane_dead_status}`.  tmux 3.4 has no format flags for focus
reporting, bracketed paste, cursor shape, kitty keyboard flags or the OSC 4
palette (those are checked on raw bytes), an APC string sets its pane
title, and a dead pane always reports `cursor_flag` 0.
