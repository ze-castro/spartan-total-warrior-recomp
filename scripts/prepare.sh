#!/bin/bash
# Turns your own copy of the game into the generated sources this project builds from:
#   game/            the files from the disc (only default.xbe is needed to build; the rest is read at run time)
#   analysis/        function map of default.xbe (tools/discover.py) and library function names (tools/oovpa_match.py)
#   build/lifted.c   the game's x86 code translated to C (tools/lift.py)
# Nothing generated here is meant to be committed or shared: it is derived from the game.
#
# usage: scripts/prepare.sh "/path/to/Spartan - Total Warrior (Europe).iso"   (or a folder you already extracted)
set -euo pipefail
cd "$(dirname "$0")/.."
SRC="${1:-}"
[ -n "$SRC" ] || { echo "usage: $0 <disc image .iso | extracted folder>"; exit 1; }
scripts/setup.sh
scripts/fetch_oovpa.sh
PY=.venv/bin/python
if [ ! -f game/default.xbe ]; then
  mkdir -p game
  if [ -d "$SRC" ]; then
    echo "copying $SRC -> game/"; cp -R "$SRC"/. game/
  else
    echo "extracting the disc image -> game/"; $PY -I tools/xiso_extract.py "$SRC" game
  fi
fi
[ -f game/default.xbe ] || { echo "no default.xbe found: is that the game's disc?"; exit 1; }
mkdir -p analysis build
echo "== finding functions in default.xbe"
$PY -I tools/discover.py game/default.xbe analysis
echo "== naming library functions"
$PY tools/oovpa_match.py game/default.xbe analysis/libsyms.json
echo "== translating x86 to C (a few minutes)"
$PY -I tools/lift.py game/default.xbe analysis/functions.json build/lifted.c
echo
echo "Prepared. Now run:  make   (compiling the translated code takes a couple of minutes)"
