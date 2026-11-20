# Status

Tested with the PAL release (`default.xbe`, XDK build 5849) on an Apple M4.

## Works

Boot, intro movies, title, menus, new game, save and load, level 1, cutscenes, the arena mode, audio, keyboard/mouse/controller input,
internal resolution scaling, anisotropic filtering, stencil shadows, bump-mapped bloom post-processing.

## Known problems

* **Enter sometimes does not register** on menus, in cutscenes or on the death screen. Press `Esc` instead (Start during a scene, back in menus). Cause not found.
* The glowing ring that should appear in front of treasure chests is never drawn: the game does not submit any draw for it, so the cause is in
  game logic rather than the renderer. Not yet found.
* Only part of the game has been played through. Later acts, boss fights and the rest of the arena modes have not been verified; expect bugs.
* Heavy combat scenes have not been profiled. Frame rate has only been measured in menus, cutscenes and the first area.
* Memory: the game uses about 52 MB of the console's 64 MB in level 1. A real heap with frees is implemented, but the Act 1 -> Act 2
  transition has not been verified.
* Movie audio/video sync is untested; some rarely used pixel-shader texture modes and cube maps are not implemented (the game does not use them in the parts played).
* Real keyboard/mouse behaviour in the window has had little testing beyond scripted input; mouse-look feel is still being tuned.
* Only the PAL executable is supported by a given build.

## Ideas

* A 60 fps-unlocked simulation if the game's timing allows it (widescreen is done: see WIDESCREEN.md).
* Controller rumble, controller remapping UI, gyro-free aim assist options.
* A `.app` bundle and a notarised release that builds from the user's disc on first launch.
* Save states, input recording/replay for regression tests (the scripted-input system is the start of it).
* A Linux/Windows renderer (Vulkan) now that the NV2A model is separate from Metal.
* A regression suite that plays scripted runs and compares frames.
