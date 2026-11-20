#!/bin/bash
# Downloads the Xbox library signature database (OOVPA) from Cxbx-Reloaded's XbSymbolDatabase into tools/oovpa/.
# It is used to name the statically linked XDK functions (Direct3D, DirectSound, XAPI) in the game's executable.
# The data is third-party (ODbL-1.0), so it is fetched at build time instead of being copied into this repository.
set -euo pipefail
cd "$(dirname "$0")/.."
COMMIT=e0034721ef1bcb683917f70ccbce9d2a80ccc9ae          # pinned: later signature changes could rename functions
if [ -f tools/oovpa/OOVPA.h ] && [ -d tools/oovpa/D3D8 ]; then echo "tools/oovpa already present"; exit 0; fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init --quiet "$tmp/x"
git -C "$tmp/x" fetch --quiet --depth 1 https://github.com/Cxbx-Reloaded/XbSymbolDatabase.git "$COMMIT"
git -C "$tmp/x" checkout --quiet FETCH_HEAD
mkdir -p tools/oovpa
for d in D3D8 DSound Xapi XGraphic; do cp -R "$tmp/x/src/OOVPADatabase/$d" tools/oovpa/; done
cp "$tmp/x/src/OOVPADatabase/OOVPA.h" "$tmp/x/src/OOVPADatabase/D3D8_OOVPA.c" tools/oovpa/
cp "$tmp/x/LICENSE" tools/oovpa/LICENSE-XbSymbolDatabase
echo "ok: tools/oovpa fetched (XbSymbolDatabase $COMMIT)"
