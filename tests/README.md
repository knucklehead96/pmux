# pmux tests

Black-box tests for Milestone 1 (daemon, `-n`/`-l`/`-k`/`-a`, attach passthrough).

    tests/run.sh                          # all tests, from anywhere
    tests/run.sh -k Detach                # unittest name filter
    python3 -m unittest -v tests/test_cli.py   # single module (from repo root)

Requirements: python3 (stdlib `unittest`) and `pexpect`.

Environment variables:

- `PMUX_BIN` — pmux binary under test (default `<repo>/build/pmux`).
- `PMUX_FAST=1` — skip slow tests (the >5 s running→idle transition).
- `PMUX_TEST_TMPDIR` — parent for per-test temp dirs (default `/tmp`; keep it
  short, the socket path must fit in 108 bytes).

Each test gets its own `XDG_RUNTIME_DIR`/`HOME`, so it never touches a real
pmux daemon. Teardown kills the test daemon and every process carrying that
test's `XDG_RUNTIME_DIR`.

Files: `helpers.py` (fixture, polling, hex diffs), `probe.py` (the app run
inside pmux; see its docstring for modes), `test_cli.py`, `test_attach.py`.
