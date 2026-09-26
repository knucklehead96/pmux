# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- `install.sh` one-line installer:
  `curl -fsSL https://raw.githubusercontent.com/knucklehead96/pmux/main/install.sh | sh`.
- aarch64 release binaries (`pmux-<version>-linux-aarch64` and `-static`).
- Scroll mode: while the attached app doesn't track the mouse, the wheel scrolls
  through the process's history (`↑` `↓` `PgUp` `PgDn` `Home` `End` too), with
  the position shown in the top-right corner; the bottom, `Esc` or `q` leave it
  and any other key leaves it and goes to the app. On the app's alternate
  screen the wheel sends `↑` / `↓`. Other mouse events are ignored then.
- The read-only view of an exited process scrolls through its history the same way.

### Changed

- Attach and the exited-process view use the terminal's alternate screen, like
  tmux: after detaching or quitting, the shell's screen and native scrollback
  are as before (they used to be replaced by the process's output). The
  process's history is no longer copied into the native scrollback.
- An app's full reset (RIS) is replaced by a soft reset of the modes and colors
  it changed, and its erase-scrollback (`CSI 3 J`, e.g. from `clear`) clears only
  pmux's history of the process: neither reaches the terminal, where they would
  leave the alternate screen or erase the shell's scrollback.
- The app's own alternate screen switches are emulated: quitting `less` or `vim`
  inside pmux repaints the app's normal screen instead of leaving pmux's.
- `[detached from NAME]` and the exit messages of `pmux -n` / `pmux -a` are
  printed on the shell's screen, on their own line.

### Fixed

- The view of an exited process is painted at the terminal's size without
  resizing the process's stored screen: a terminal taller than that screen no
  longer shows lines twice.

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

[Unreleased]: https://github.com/knucklehead96/pmux/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/knucklehead96/pmux/releases/tag/v0.1.0
