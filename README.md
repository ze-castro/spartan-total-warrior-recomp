# spartan-total-warrior-recomp

**Spartan: Total Warrior on macOS (native recompilation)** · version 1.0.0 · [release notes and known issues](RELEASE_NOTES.md)

A native macOS (Apple Silicon) port of **Spartan: Total Warrior** for the original Xbox, made by *statically recompiling* the game's
x86 code to C and running it against a small re-implementation of the console underneath: the kernel, the NV2A GPU (drawn with Metal),
audio (CoreAudio), input and the file system. It is not an emulator of the whole console: the game's own code runs natively.

> **You need your own copy of the game.** This repository contains no game code, no assets and nothing generated from them. You
> supply a disc image of the Xbox PAL release you own; the build tools translate it on your machine. Not affiliated with or endorsed by
> SEGA, Creative Assembly, Microsoft or anyone else. Spartan: Total Warrior is a trademark of its owners.

## What works

* The whole intro, menus, cutscenes, level 1 and the arena mode; save games and loading; intro movies (video overlay) and audio.
* 60 fps in menus and cutscenes, 55-60 in gameplay on an M-series Mac.
* Higher internal resolution than the console: Original (480 lines), 720p, 1080p, 1440p or 2160p, with anisotropic filtering.
* Optional quality extras (launcher *Quality* tab): FXAA or 2x/4x MSAA, output sharpening and smoother bloom.
* Optional **16:9 widescreen** (a real wider view built by the game itself, see [docs/WIDESCREEN.md](docs/WIDESCREEN.md)).
* A launcher window: pick the game disc, resolution, aspect ratio, mouse sensitivity, Metal performance HUD, full screen, and **rebind every
  keyboard/mouse control**. Keyboard + mouse and game controllers (hot-plug, rumble, full remapping) are supported.

Known problems and the roadmap are in [docs/STATUS.md](docs/STATUS.md).

## Requirements

* macOS 13 or later on Apple Silicon (built and tested on an M4), Xcode command line tools (`xcode-select --install`), Python 3.
* Your disc image of *Spartan: Total Warrior* (Xbox, **PAL / Europe**; the build is made from that exact executable) and about 12 GB of free disk space.

## Install the app (no build tools needed except Apple's command line tools)

1. Download `Spartan-Total-Warrior-<version>-macos-arm64.dmg` (or the `.zip`) from the Releases page and drag the app to Applications.
2. The app is not notarized by Apple (it is ad-hoc signed), so macOS blocks the first launch. **Right-click the app, choose Open, then Open.** If macOS
   says the app is damaged, run `xattr -dr com.apple.quarantine "/Applications/Spartan Total Warrior.app"` once in Terminal.
3. If you have never installed Apple's command line tools, the first run tells you to run `xcode-select --install` (a few minutes, free).
4. In the launcher choose **your own** Spartan: Total Warrior (Xbox, PAL) disc image. The app copies the files (seconds) and then *prepares the game for
   your Mac* (about 5 minutes, once): it analyses the game's executable, translates it to C and compiles it with Apple's compiler. The result is
   stored in `~/Library/Application Support/Spartan Total Warrior Recomp/`; nothing derived from the game is in the download, and it is never uploaded anywhere.
5. Press Play. Later launches are instant. A new app version prepares again automatically (it re-translates against the new runtime).

Developers can also build everything from a checkout:

## Build and run

```sh
git clone https://github.com/ze-castro/spartan-total-warrior-recomp.git && cd spartan-total-warrior-recomp
scripts/prepare.sh "/path/to/Spartan - Total Warrior (Europe).iso"   # once: extracts the disc, analyses and translates the game (minutes)
make                                                                  # compiles everything into build/stw (a couple of minutes)
./build/stw                                                           # opens the launcher
```

The first time, the launcher asks for the disc image again (or an extracted folder). It checks that it is the same executable the build was
made from, copies the files to `~/Library/Application Support/Spartan Total Warrior Recomp/` (or next to the project when run from the
project folder) and remembers the choice. Details and troubleshooting: [docs/BUILDING.md](docs/BUILDING.md).

## Controls

Defaults (all rebindable in the launcher's *Controls* tab): `WASD` move, `Shift` walk, `Space` jump, `E` interact, left click attack 1,
`Shift`+left click attack 2, right click shield, `Cmd`+left click / `Cmd`+`Shift`+left click rage moves, mouse = camera, scroll = zoom,
`Enter` accept, `Esc` pause/back, `Shift+Enter` the Start button. In game: `F1`/`F2` lower/raise the resolution, `F3` Metal HUD, `F10` full
screen, `Cmd+Q` quit. Full list: [docs/CONTROLS.md](docs/CONTROLS.md).

## How it works

1. `tools/discover.py` finds ~16,600 functions in the game's executable; `tools/oovpa_match.py` names the statically linked XDK library functions.
2. `tools/lift.py` translates every function to C (integer, x87, SSE and MMX subsets the game uses). Hardware-facing library functions are
   replaced by hand-written handlers in `runtime/`.
3. `runtime/` supplies the machine: kernel calls, threads and synchronisation, an NV2A model whose push-buffer commands are turned into Metal
   draws (vertex programs to MSL, register combiners to MSL), a DirectSound mixer, XInput, and the file system.

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); debugging switches in [docs/DEBUGGING.md](docs/DEBUGGING.md).

## Contributing, license

See [CONTRIBUTING.md](CONTRIBUTING.md). The code here is released into the public domain under the [Unlicense](LICENSE); third-party material and how it is used is listed in
[THIRD_PARTY.md](THIRD_PARTY.md). The license does not extend to the game.
