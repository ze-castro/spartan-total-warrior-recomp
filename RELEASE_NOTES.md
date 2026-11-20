# Spartan: Total Warrior for macOS 1.0.2

## What's new in 1.0.2

**Fixed: a direction key could stay stuck after using Cmd.** macOS does not send key-up events for other keys while Cmd is held. If you held a
direction (for example `W`), pressed Cmd + click for a rage move, and let go of the direction while Cmd was still down, the game never saw the key
released and kept moving that way. Keys that were held or pressed while Cmd was down are now checked against the real keyboard state, so they
are released correctly. Keys pressed without Cmd behave as before.

If you notice the opposite (a direction stops for a moment while you hold it during a Cmd attack), please report it.

## What was new in 1.0.1

A small fix to the launcher window. While the game was being prepared on the first run, two things overlapped:

* the "Copying the game files" progress bar stayed on top of the *Choose disc image or folder...* button after the copy had finished;
* the "Preparing the game for this Mac" panel covered the *Show this window at startup* checkbox.

The progress bar now takes the button's place only while the copy runs, and the options above the preparation panel are spaced so nothing
overlaps. Nothing else changed: the game, its saves and your settings are the same as in 1.0.0.

---

# About this release (1.0.0 and later)

First release. A native Apple Silicon port of the original Xbox game, built by translating the game's own code on your Mac. You need your own
copy of the game (a PAL / Europe disc image); nothing from the game is included in this download.

## Install

1. Download `Spartan-Total-Warrior-1.0.2-macos-arm64.dmg` (or `.zip`) and drag the app to Applications.
2. The app is not signed by an Apple developer account. The first time, right-click the app and choose **Open**, then **Open** again (or run
   `xattr -dr com.apple.quarantine "/Applications/Spartan Total Warrior.app"`).
3. You need the Xcode command line tools (`xcode-select --install`) and Python 3 (it comes with them).
4. Open the app, choose your disc image (`.iso`) or an extracted game folder. The first run prepares the game for your Mac: about 5 minutes,
   once. Your disc image stays where it is; the game files and your saves are kept in
   `~/Library/Application Support/Spartan Total Warrior Recomp/`.

## What is in it

* The whole game runs natively: intro movies, menus, cutscenes, gameplay, arena mode, saving and loading, audio.
* Resolution: Original (480 lines), 720p, 1080p, 1440p, 2160p. 4:3 or 16:9 widescreen. Anisotropic filtering, FXAA or 2x/4x MSAA, sharpening.
* A launcher to choose the disc, resolution, aspect ratio, mouse sensitivity, Metal performance HUD and full screen, plus **fully rebindable
  keyboard and mouse controls** and a controller tab (rumble, dead zones, remapping).
* Keys in game: `F1` / `F2` lower / raise the resolution, `F3` Metal HUD, `F10` full screen, `Cmd+Q` quit.

## Known issues

**Controls**
* **Enter sometimes does not work** on menus, in cutscenes or on the death screen. **Use `Esc`**, which always works (during play, cutscenes and
  the death screen it sends Start; in menus it goes back). `Shift+Enter` also sends Start.
* The mouse-look camera does not feel right for everyone yet; the sensitivity slider in the launcher changes it. Keyboard and mouse have had much
  less testing in a real window than scripted input.
* On a Mac the `F` keys act as media keys unless you hold `fn` (or turn on *Use F1, F2, etc. keys as standard function keys* in System Settings).
* Game controllers (Xbox, PlayStation, Switch) are implemented but have **not been tested on real hardware**. Rumble feel and hot-plug are unverified.

**Graphics**
* **The glowing ring that should appear in front of treasure chests is missing.** The game never asks for it, so the cause is in its own
  effect code; it has not been found.
* Widescreen: the 3D view is truly wider, but menu backgrounds and title art are 4:3 pictures stretched across the screen, and a 2D element drawn
  with perspective (if the game has any) would look about a third too wide. Fights and later acts have not been played in widescreen.
* MSAA 4x costs a lot of GPU time, especially at 1440p and 2160p (not tested at 2160p).
* A few rarely used pixel-shader texture modes (cube maps, dot-product modes) are not implemented; they do not appear in the parts played.

**Stability and coverage**
* Only the early game has been played through: the opening, the first levels and the arena. **Act 2 and later, boss fights and the rest of the
  arena modes are untested; expect crashes or glitches there.** An out-of-memory crash going into Act 2 was fixed (the game now frees memory), but
  the Act 1 to Act 2 transition itself has not been verified.
* Performance is steady at about 55-60 fps in menus, cutscenes and the first areas. **Heavy combat scenes have not been measured.**
* Rare freezes: an intermittent hang (the game waiting forever on the GPU) was found and mitigated, but may still occur very occasionally. If the
  picture stops, quit with `Cmd+Q` and start again; saves are not affected.
* Movie audio and video sync is untested. Audio is mixed in software: 3D sounds use simple distance and panning, with no Doppler.
* Only the **PAL** executable is supported. Another region or revision needs its own build.

**App**
* The first-run preparation needs the Xcode command line tools. If they are missing the app tells you; it cannot continue without them.
* The app is ad-hoc signed only (see Install). A proper Developer ID signature and notarization are not done.

## Reporting problems

Open an issue and include the log: `~/Library/Application Support/Spartan Total Warrior Recomp/stw.log`. For crashes or freezes, run the app from
Terminal with `STW_FPS=1 STW_HEAPLOG=1` for more detail (`docs/DEBUGGING.md`).
