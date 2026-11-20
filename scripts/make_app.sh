#!/bin/bash
# Builds "build/Spartan Total Warrior.app" plus a .zip and a .dmg for release.
#
# The app contains the runtime and the build tools only. Nothing derived from the game is in it: the translated game code is built on the
# user's own machine, from their own disc, the first time the app runs (runtime/prepare.m).
#
#   scripts/make_app.sh                 version = $VERSION, else the VERSION file
#   VERSION=1.2.3 scripts/make_app.sh
set -euo pipefail
cd "$(dirname "$0")/.."
APP_NAME="Spartan Total Warrior"
VERSION="${VERSION:-$(tr -d '[:space:]' < VERSION)}"
BUILD=build
APP="$BUILD/$APP_NAME.app"
PY="${PYTHON:-python3}"

echo "== runtime (no game-derived code)"
make "$BUILD/stw-app"

echo "== signature database (third party, bundled with its license; pinned commit)"
scripts/fetch_oovpa.sh

echo "== capstone wheel (the translator's only dependency; installed offline on first run)"
mkdir -p "$BUILD/vendor"
if ! ls "$BUILD"/vendor/capstone-*.whl >/dev/null 2>&1; then
  tmpv=$(mktemp -d)
  "$PY" -m venv "$tmpv"
  "$tmpv/bin/python" -m pip download --quiet --no-deps --only-binary=:all: -d "$BUILD/vendor" "capstone>=5.0"
  rm -rf "$tmpv"
fi

echo "== bundle"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/vendor"
cp "$BUILD/stw-app" "$APP/Contents/MacOS/stw"
cp "$BUILD"/vendor/capstone-*.whl "$APP/Contents/Resources/vendor/"
# helpers the first-run preparation needs: the translator and the runtime headers/sources/data its generators and compiler read
rsync -a --exclude __pycache__ --exclude '*.pyc' tools/ "$APP/Contents/Resources/tools/"
rsync -a --exclude __pycache__ --exclude harness.c runtime/ "$APP/Contents/Resources/runtime/"
cp LICENSE THIRD_PARTY.md "$APP/Contents/Resources/"
# the cache key of what the first run builds: changes whenever the runtime or the translator changes
BUILD_ID=$( (cd "$APP/Contents/Resources" && find tools runtime -type f ! -name '.DS_Store' | LC_ALL=C sort | xargs shasum -a 256 | shasum -a 256 | cut -c1-12) )
echo "$BUILD_ID" > "$APP/Contents/Resources/build_id.txt"

echo "== icon"
ICONSET=$(mktemp -d)/AppIcon.iconset
mkdir -p "$ICONSET"
"$PY" scripts/make_icon.py "$ICONSET/icon_512x512@2x.png" 1024
for s in 16 32 64 128 256 512; do sips -z $s $s "$ICONSET/icon_512x512@2x.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null; done
cp "$ICONSET/icon_32x32.png" "$ICONSET/icon_16x16@2x.png"
cp "$ICONSET/icon_64x64.png" "$ICONSET/icon_32x32@2x.png"
cp "$ICONSET/icon_256x256.png" "$ICONSET/icon_128x128@2x.png"
cp "$ICONSET/icon_512x512.png" "$ICONSET/icon_256x256@2x.png"
rm -f "$ICONSET/icon_64x64.png"
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$APP_NAME</string>
  <key>CFBundleDisplayName</key><string>$APP_NAME</string>
  <key>CFBundleIdentifier</key><string>io.github.stw-recomp.spartan-total-warrior</string>
  <key>CFBundleExecutable</key><string>stw</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$BUILD_ID</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
  <key>NSHumanReadableCopyright</key><string>Public domain (Unlicense) code; contains no game code or assets. You need your own game disc.</string>
</dict>
</plist>
PLIST

echo "== ad-hoc code signature"
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP" && echo "signature ok"

echo "== zip and dmg"
OUTNAME="Spartan-Total-Warrior-$VERSION-macos-arm64"
rm -f "$BUILD/$OUTNAME.zip" "$BUILD/$OUTNAME.dmg"
ditto -c -k --keepParent "$APP" "$BUILD/$OUTNAME.zip"
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$BUILD/$OUTNAME.dmg"
rm -rf "$STAGE"
echo
echo "built: $APP"
echo "       $BUILD/$OUTNAME.zip"
echo "       $BUILD/$OUTNAME.dmg"
