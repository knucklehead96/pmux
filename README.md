# pmux

A terminal process multiplexer: keep long-running processes alive in the background, see them all in one list, and attach to any of them full-screen.

![pmux process list](docs/screenshots/list-dark.png)

## Features

- **One list, grouped by directory.** A list in the style of Claude Code shows every process under the directory it was started in.
- **Status at a glance.** Each process shows whether it is running, has rung the bell while you were away, or has exited (with the exit code or signal). Each row also shows the command currently in the foreground and the process's age.
- **Byte-exact attach.** Attaching gives the process the whole terminal, with no status bar and no border. Everything you type goes straight to it except one key: the detach key (`Ctrl+←` by default, configurable), which takes you back to the list.
- **Screen and scrollback restore.** The daemon keeps a libvterm screen for every process. When you reattach, it restores the screen, the scrollback (into your terminal's native scrollback), the cursor and the terminal modes, even for full-screen apps on the alternate screen.
- **Automatic dark/light theme.** pmux asks the terminal for its background color and picks a matching palette. Four accent colors are available.
- **Plain config file.** Settings live in `~/.pmux/config`, and the file is optional.

Processes keep running after you quit the TUI or close the terminal.

## Install

Requirements: Linux, a C++20 compiler, CMake ≥ 3.20, pkg-config and libvterm (`libvterm-dev` on Debian/Ubuntu). [FTXUI](https://github.com/ArthurSonzogni/FTXUI) is downloaded and built automatically.

```sh
cmake -S . -B build && cmake --build build -j
cmake --install build --prefix ~/.local      # installs ~/.local/bin/pmux
```

## Usage

| Command | |
|---|---|
| `pmux` | Open the process list. Starts the daemon if it isn't running. |
| `pmux -n [name] [-d] [-- cmd …]` | Create a process in the current directory and attach to it. `-d` leaves it detached. The name defaults to the directory's basename (`-2`, `-3`, … added if that name is taken). The command defaults to `default_cmd`, then `$SHELL`. |
| `pmux -l` | Print the processes as tab-separated `NAME STATE PID DIR COMMAND` lines. |
| `pmux -a <name>` | Attach to a process. |
| `pmux -k <name>` | Kill a process (`SIGHUP`, then `SIGKILL` after 3 s) and remove it from the list once it has exited. A process that has already exited is just removed. |
| `pmux --daemon` | Run the daemon in the foreground, for debugging. |

### Keys in the list

| Key | Action |
|---|---|
| `↑` `↓` `PgUp` `PgDn` `Home` `End` | Select a process |
| `Enter`, double-click | Attach. On an exited process, show its final screen and history read-only. |
| `Ctrl+N` | New process: a dialog with Name, Dir and Command fields |
| `Ctrl+R` | Rename the selected process |
| `Ctrl+X` twice | Kill the selected process and remove it from the list. On an exited process, remove it. The first press asks `press ctrl+x again to kill <name>`; another key or 2 s cancels. |
| typing | Filter by name, directory or command. `Backspace` edits the filter and `Esc` clears it. |
| mouse wheel, click | Move or set the selection |
| `Ctrl+C` twice | Quit the list. The daemon and all processes keep running. The first press asks `press ctrl+c again to quit`; another key or 2 s cancels. |

**Attached:** `Ctrl+←` returns to the list (set `detach_key` to use `Ctrl+Shift+←` or `Ctrl+\` instead). pmux intercepts nothing else. Every other key, mouse event, paste and focus event goes to the process unchanged. With the default, `Ctrl+\` reaches the process, but `Ctrl+←` (word-left in shells and editors) does not. Mouse-wheel scrolling uses your terminal's own scrollback.

## Config

`~/.pmux/config` is optional. Each line is `key = value`, and `#` starts a comment. A leading `~` and `$VAR` / `${VAR}` in values are expanded. pmux prints a warning for unknown keys and invalid values, then ignores them.

```ini
default_dir = ~/work      # used when the list is empty
default_cmd = $SHELL      # command for new processes
theme       = auto        # auto | dark | light | ansi
accent      = clay        # clay | blue | purple | teal
scrollback_lines = 10000  # history lines kept per process, 0–100000
detach_key  = ctrl+left   # ctrl+left | ctrl+shift+left | ctrl+backslash
```

- `default_dir`: the directory pre-filled in the New process dialog when the list is empty. If it isn't set, pmux uses the directory it was launched from. Otherwise the dialog uses the selected process's directory.
- `default_cmd`: the command for new processes. Simple `'…'` / `"…"` quoting is supported. If it isn't set, pmux uses `$SHELL`, then `/bin/sh`.
- `theme`: `auto` asks the terminal for its background color at startup. `dark` and `light` force a palette. `ansi` uses only the terminal's 16 palette colors.
- `accent`: the color of the logo, the dialog titles, the cursor and the filter prompt.
- `scrollback_lines`: the number of history lines the daemon keeps for each process (0–100000). The setting applies to processes created after the change.
- `detach_key`: the one key pmux intercepts while attached, for `pmux -a`, `pmux -n` and the list. `ctrl+left` (the default) also matches the kitty keyboard protocol and rxvt encodings of `Ctrl+←`; `ctrl+backslash` is the classic `Ctrl+\`.

## How it works

- `pmux` is a single binary with two roles: a client (the list TUI and the CLI commands) and a daemon. The first command you run starts the daemon (fork + `setsid`), which runs a single-threaded `epoll` loop.
- The daemon owns a PTY for each process (`forkpty`). Clients talk to it over a Unix socket at `$XDG_RUNTIME_DIR/pmux/pmux.sock` (fallback `/tmp/pmux-$UID/pmux.sock`, directory mode `0700`).
- All of a process's output goes into its own libvterm screen with a scrollback ring buffer, whether a client is attached or not. A small parser also tracks terminal modes that libvterm doesn't expose: mouse, bracketed paste, application keys, kitty keyboard flags, the title and color changes.
- On attach, the daemon resizes that screen to your terminal and sends a snapshot: the history, the screen, the cursor and the modes. After that it forwards the PTY output unchanged. Detaching undoes the modes and color changes the app made, so the list always comes back clean.

## Limitations

- Linux only.
- One attached client per process. A new attach detaches the previous client.
- Some terminal state isn't restored on reattach: OSC 8 hyperlinks, scroll margins, character sets, and attributes libvterm doesn't store (such as faint text and alternate fonts).
- The attached process never receives the detach key. With the default that is `Ctrl+←`, which shells and editors use for word-left; pick another `detach_key` if you need it.
- On the Linux virtual console (no X/Wayland), `Ctrl+←` sends the same bytes as `←`. Set `detach_key = ctrl+backslash` there.
- A process keeps the `TERM` of the terminal that created it, even if you attach from a different terminal type. tmux and screen behave the same way.

## Testing

```sh
tests/run.sh                  # the whole suite
tests/run.sh -k test_tui      # a single module or test name
PMUX_FAST=1 tests/run.sh      # skip the slow (>5 s silence) tests
```

The tests need python3, `pexpect` and tmux 3.x. `PMUX_BIN` selects the binary under test (default `build/pmux`). See [tests/README.md](tests/README.md) for details.

The screenshots come from the real TUI and are regenerated with `python3 scripts/screenshots.py`. The script needs tmux, perl and a headless Chromium (`--chrome` or `$CHROME`).

## Gallery

| | |
|---|---|
| ![Light theme](docs/screenshots/list-light.png) | ![Filter](docs/screenshots/filter.png) |
| Light theme | Typing filters the list |
| ![New process dialog](docs/screenshots/new-dialog.png) | ![Kill confirmation](docs/screenshots/kill-confirm.png) |
| `Ctrl+N`: new process | `Ctrl+X`: press again to kill |
