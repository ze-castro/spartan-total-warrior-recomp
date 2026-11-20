# Building

## One-time preparation

```sh
scripts/prepare.sh "/path/to/Spartan - Total Warrior (Europe).iso"      # or an already extracted folder
```

This runs, in order (each step can be run by hand; see the script):

1. `scripts/setup.sh`: creates `.venv` and installs `capstone` and `unicorn`.
2. `scripts/fetch_oovpa.sh`: downloads the Xbox library signature database (a pinned XbSymbolDatabase commit) into `tools/oovpa/`.
3. `tools/xiso_extract.py`: extracts the disc into `game/` (skipped if `game/default.xbe` exists).
4. `tools/discover.py`: finds the functions in `default.xbe` -> `analysis/functions.json`.
5. `tools/oovpa_match.py`: names library functions -> `analysis/libsyms.json`.
6. `tools/lift.py`: translates the code to C -> `build/lifted.c`.

Everything these produce is derived from the game and is git-ignored. Do not publish it.

## Build

```sh
make                # build/stw
make clean          # removes build/
```

`build/lifted.c` is large; compiling it takes a couple of minutes the first time and it is only rebuilt when it changes. The build also writes
`build/xbe_id.h` (a hash of `game/default.xbe`): at run time the launcher refuses a disc whose executable is not the one this build was made from.

## Run

`./build/stw` opens the launcher. Where it keeps files:

| What | Where |
| --- | --- |
| settings, key bindings | `stw_settings.txt` |
| save games | `save/` |
| game files copied from a disc image | `game_disc/` |

all under the *data directory*: `$STW_HOME` if set; otherwise the current folder when it already holds `game/`, `save/` or settings (the
developer layout); otherwise `~/Library/Application Support/Spartan Total Warrior Recomp/`.

## Troubleshooting

* *"not the version this build was made from"*: the build is tied to one executable (PAL). Use the PAL disc, or run `scripts/prepare.sh` again
  with your disc into a clean checkout.
* *`clang` or `python3` missing*: install the Xcode command line tools (`xcode-select --install`) and Python 3.
* *Black window*: run with `STW_FPS=1 ./build/stw` and check the log; `docs/DEBUGGING.md` lists the switches.
* *Crash with "guest heap exhausted"*: send the log produced with `STW_FPS=1 STW_HEAPLOG=1`.

## The app (what the release contains and how it works)

`scripts/make_app.sh` produces `build/Spartan Total Warrior.app`, a `.zip` and a `.dmg`. It needs no disc and no game files:

* `Contents/MacOS/stw`: the runtime only (`make build/stw-app`), linked with `-export_dynamic`; it has no translated game code in it.
* `Contents/Resources/tools`, `runtime`: the translator (Python) and the runtime sources/headers/data its generators and the compiler read; `vendor/`: a capstone
  wheel (BSD) installed offline into a virtual environment; `tools/oovpa/`: the XbSymbolDatabase signatures (ODbL-1.0) bundled unmodified with their license
  file so the first run works offline; `build_id.txt`: a hash of all of that, part of the cache key.
* On first run the launcher (`runtime/prepare.m`) uses the system `python3` and `clang` from Apple's command line tools: it creates `venv/`, runs
  `tools/discover.py`, `oovpa_match.py`, `lift.py`, `gen_libstubs.py`, `gen_kernel.py` in `work/<key>/`, compiles `lifted.c`, `ktable.c`, `libstubs.c` and
  `runtime/gametables.c` into **`lifted/<game>-<build>/lifted.dylib`** and deletes the intermediate source. The runtime `dlopen`s that library
  (`runtime/gamelib.c`); its undefined symbols (`G`, `mmio_write`, the kernel/library handlers, ...) are resolved against the program.
* A different `build_id` (new app version) or a missing library triggers preparation again; the key also includes the supported `default.xbe` hash
  (`runtime/data/supported_xbe.sha256`).
* When started from Finder the log goes to `<data dir>/stw.log`.

`make` still builds the developer binary with the translated code linked in (`-DSTW_STATIC`); the generated tables reach the runtime through
`StwGameTables` (`runtime/gametables.h`) in both builds.

### Signing and notarization

The release is ad-hoc signed (`codesign --sign -`), which is enough to run after the right-click-Open step. With an Apple Developer ID the maintainer can
remove that step: sign with `codesign --force --deep --options runtime --timestamp --entitlements entitlements.plist --sign "Developer ID Application: NAME (TEAMID)"`,
then `xcrun notarytool submit <zip> --apple-id ... --team-id ... --wait` and `xcrun stapler staple`. The entitlements file must contain
`com.apple.security.cs.disable-library-validation` (the app loads the library it compiles on the user's machine, which is not signed by the same team).
This has not been attempted here.
