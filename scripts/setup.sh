#!/bin/bash
# Creates the Python environment the build tools need (.venv) and checks the toolchain.
set -euo pipefail
cd "$(dirname "$0")/.."
[ "$(uname -s)" = "Darwin" ] || { echo "This project targets macOS (Metal, Cocoa)."; exit 1; }
[ "$(uname -m)" = "arm64" ] || echo "warning: built and tested on Apple Silicon (arm64) only."
command -v clang >/dev/null || { echo "clang not found: run 'xcode-select --install' first."; exit 1; }
command -v python3 >/dev/null || { echo "python3 not found."; exit 1; }
if [ ! -x .venv/bin/python ]; then python3 -m venv .venv; fi
.venv/bin/python -m pip install --quiet --upgrade pip
.venv/bin/python -m pip install --quiet -r requirements.txt
echo "ok: .venv ready ($(.venv/bin/python --version))"
