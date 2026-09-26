# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.1.0] - 2026-09-26

First public release.

### Added

- Background daemon that owns each process in its own PTY and keeps it running
  after the TUI quits or the terminal closes. It starts automatically on first use.
- List TUI with processes grouped by start directory, showing the foreground
  command and age.
- Status glyphs: `●` running, `!` bell rung while detached, `○` exited (with
  exit code or signal).
- Type-to-filter by name, directory or command.
- New process (`Ctrl+N`) and rename (`Ctrl+R`) dialogs. Kill or remove with a
  double `Ctrl+X` and quit with a double `Ctrl+C`; both ask for confirmation
  first. Mouse selection, double-click to attach and wheel scrolling.
- Read-only view of an exited process's final screen and history.
- Byte-exact, full-screen attach. Only the detach key is intercepted (default
  `Ctrl+←`, configurable to `Ctrl+Shift+←` or `Ctrl+\`). It is recognised in
  its legacy, kitty keyboard protocol, rxvt and modifyOtherKeys encodings.
- On reattach, restore of the screen, scrollback (into the terminal's native
  scrollback), cursor, terminal modes (alternate screen, mouse, bracketed paste,
  application keys, kitty keyboard flags, modifyOtherKeys), title and OSC
  color changes, from a per-process libvterm screen. Modes and colors are
  reset on detach.
- Automatic dark/light theme from the terminal's background color, a
  16-color `ansi` theme, and four accent colors.
- Optional `~/.pmux/config` with `default_dir`, `default_cmd`, `theme`,
  `accent`, `scrollback_lines` and `detach_key`.
- CLI: `-n [name] [-d] [-- cmd …]`, `-l`, `-a <name>`, `-k <name>`,
  `--stop [-f|--force]`, `-V`/`--version`, `--daemon`.
- `scripts/release.sh` builds stripped release binaries: dynamically linked
  and fully static (x86-64 Linux), plus `SHA256SUMS`.

[0.1.0]: https://github.com/knucklehead96/pmux/releases/tag/v0.1.0
