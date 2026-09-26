#!/bin/sh
# Run the pmux test suite from the repo root.  Extra args go to unittest.
cd "$(dirname "$0")/.." || exit 1
exec python3 -m unittest discover -s tests -v "$@"
