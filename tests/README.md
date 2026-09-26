# pmux tests

Black-box tests for the daemon and CLI (`-n`/`-l`/`-k`/`-a`, attach
passthrough), the list TUI and config file, libvterm screen / scrollback /
mode restore, and the finer details (faint text, self-attach, small
terminals, release build).

    tests/run.sh                          # all tests, from anywhere
    tests/run.sh -k Detach                # unittest name filter
    tests/run.sh -k test_tui              # single module
    (cd tests && python3 -m unittest -v test_cli)   # plain unittest

Requirements: python3 (stdlib `unittest`), `pexpect`, and `tmux` (3.x) for
the TUI tests.

Environment variables:

- `PMUX_BIN` — pmux binary under test (default `<repo>/build/pmux`).
- `PMUX_FAST=1` — skip slow tests (a process silent for >5 s must stay
  `running`; the Release build + install).
- `PMUX_TEST_TMPDIR` — parent for per-test temp dirs (default `/tmp`; keep it
  short, the socket paths must fit in 108 bytes).

Each test gets its own `XDG_RUNTIME_DIR`/`HOME`, so it never touches a real
pmux daemon or `~/.pmux/config`. Teardown kills the test daemon and every
process carrying that test's `XDG_RUNTIME_DIR` (including the test's tmux
server).

Files:

- `helpers.py` — `PmuxEnv` fixture, polling, hex diffs, detach keys
  (`DETACH` = the default Ctrl+Left `ESC[1;5D`, `CTRL_SHIFT_LEFT`,
  `CTRL_BACKSLASH`), the shared input corpus (all 256 bytes incl. 0x1c,
  key / mouse / paste / kitty sequences; asserted free of any Ctrl+Left
  detach form).
- `probe.py` — the app run inside pmux; see its docstring for modes.
  `--winch-mark NAME` prints `[winch:NAME]` on every SIGWINCH, i.e. on
  every attach, which is how TUI tests see that an attach happened.
  `draw <script> [winch=F] [wlog=P] [usr1=F] [inlog=P] [exit=N]` writes a
  script once and never reacts to input (restore tests: everything on
  screen after an attach comes from pmux's snapshot).
- `tui.py` — `TmuxTui`: runs `pmux` in a private tmux server
  (`tmux -L <unique> -f /dev/null`, `TMUX_TMPDIR` in the test's temp dir,
  100x30, `tmux-256color`, `COLORTERM=truecolor`) with `keys()` (`C-Left`
  sends `ESC[1;5D`, the default detach key) / `type()` /
  `raw()` (exact bytes via `send-keys -H`) / mouse helpers, `screen()` /
  `cells()` (capture-pane with SGR parsed per cell) and `wait_for()`; plus
  theme colors and the `TuiCase` base class.  `fmt()` / `cursor()` /
  `history()` read tmux format flags, the cursor and the full native
  scrollback; `Cell.key()` includes italic and strikethrough.  Footer
  constants (`FOOTER_LIST`, `FOOTER_FILTER`, `FOOTER_EMPTY`, `FOOTER_QUIT`,
  `footer_kill()`) and the hint lists they are built from.  TUI tests write
  `~/.pmux/config` with `theme = dark` unless they test themes.
- `test_cli.py`, `test_attach.py` — CLI and attach (`-l` states running | exited:N |
  signaled:N; `-k` kills and removes, SIGKILL after 3 s; default detach key
  Ctrl+Left, Ctrl+\\ reaches the app).
- `test_tui.py` — list screen: layout, colors/themes, spelled-out key
  hints (whole hints dropped from the right when narrow, `type to filter`
  above 8 processes), keys, attach and detach (Ctrl+Left) from the list,
  new/rename dialogs, kill (Ctrl+X twice: kill + remove, or remove an exited
  entry; other key / selection change / 2 s timeout cancels), filter, mouse,
  refresh (bell/exit; no idle state), quit (Ctrl+C twice; other key / 2 s
  timeout cancels; Ctrl+Q does nothing) and terminal restore, passthrough
  through the TUI (pexpect). `HarnessSelfTest` validates `tui.py` without pmux.
- `test_config.py` — config file: warnings (incl. `detach_key`),
  `default_cmd`, `default_dir`.
- `test_restore.py` — restore: screen/attribute/cursor restore (CLI and TUI),
  output while detached, history (never in the native scrollback; scroll
  mode) and `scrollback_lines`
  (incl. 0 and invalid values), alt screen, modes, OSC 4/10/11/12 replay and
  reset, title, snapshot prologue / no RIS, query replies (detached:
  libvterm; attached: the real terminal only), bell only while detached,
  exited process's final screen, resize between attaches, snapshot time for
  10k history lines (printed as `[perf]`), `less`.  The oracle for screen
  state is a reference tmux pane running the same probe script without
  pmux.  Raw tests find the end of the snapshot by a DCS marker the probe
  prints on SIGWINCH (pmux signals the app on attach, after the snapshot).
  `RestoreHarnessSelfTest` validates the probe and oracle without pmux.
  While attached, a pmux pane is on tmux's alternate screen and has pmux's
  mouse modes when the app tracks none (`attached_state` adjusts the
  reference); `scroll_history` reads a session's history through scroll mode.
- `test_altscreen.py` — the shell's screen and scrollback survive attach /
  detach / quit (`-n`, `-a`, TUI attach and exited view, quit without
  attaching), `[detached ...]` on its own line, the app's alternate screen
  switches emulated (incl. other parameters in the same CSI, split across
  reads, RIS), the outer mouse modes following the app, the wheel (scroll
  mode, cursor keys on the app's alternate screen, split reports), scroll
  mode keys, output withheld and the view anchored while scrolled, queries
  answered while scrolled, detach while scrolled, scrolling the exited view.
  A scripted app (`APP`) logs its input and runs `!X` commands from it.

- `test_polish.py` — faint (SGR 2) restore incl. history and the app's
  pen, colors whose parameters contain a 2, attached output still
  byte-exact (plus >16-argument CSIs that crash libvterm 0.3.3), keys typed
  during the startup theme query, self-attach refusal (CLI, TUI) and nested
  attach to another process, the too-small terminal screen, and (slow)
  Release build + install into a temp dir.
- `test_regressions_client.py` — client / TUI regressions: detach while
  output floods a slow terminal, TUI exit when the daemon dies, socket
  directory / owner checks, resizes during a TUI attach, inherited fds,
  signals while attached (CLI, TUI), detach-key forms for every
  `detach_key` value (Ctrl+Left: kitty lock modifiers / events, rxvt,
  split sequences, look-alikes; the same for `ctrl+shift+left` and
  `ctrl+backslash` incl. modifyOtherKeys; an invalid value falls back to
  Ctrl+Left; the TUI honours the setting), Home/End
  variants, the list screen's late cursor-shape reply, harness style
  carry-over, specific connect errors.
- `test_regressions_daemon.py` — daemon / screen regressions: libvterm
  crashes (reflow, wide characters on one column, huge sizes), the alternate
  screen across resizes, RIS in the alternate screen, child signal state and
  umask, attach loops, modifyOtherKeys restore, process name, `daemon.log`,
  bounded queues and CSI buffer, foreground command.

Notes: a detached tmux does not answer OSC 11, so pmux uses its assumed
background (#1E1E1E dark / #FAFAF7 light) and the selection-bar color is
computed from that. tmux 3.4 occasionally fails to reap a dead pane's
process, so `TmuxTui` records the exit status through a tiny `sh` wrapper
instead of `#{pane_dead_status}`.  tmux 3.4 has no format flags for focus
reporting, bracketed paste, cursor shape, kitty keyboard flags or the OSC 4
palette (those are checked on raw bytes), an APC string sets its pane
title, and a dead pane always reports `cursor_flag` 0.
