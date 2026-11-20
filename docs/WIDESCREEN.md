# Widescreen (16:9)

Choose **Aspect ratio: 16:9** in the launcher (Display tab); it is stored as `widescreen=1` in `stw_settings.txt` and read when the game starts
(`STW_WIDE=1` / `STW_WIDE=0` overrides it for one run).

## How the game handles it

The original game has **no widescreen mode**: its present parameters only use the 480p/PAL60 video flags (`XGetVideoFlags`, function
`0x1b2309`), never the widescreen flag, and its options menu has no screen format entry. So 16:9 here is an adaptation, done the way
anamorphic widescreen games on the original Xbox were: render a 16:9 view into the same 640x480 buffer and let the display stretch it.

1. **Wider 3D view.** The game's device setup (function `0x19d730`) stores the aspect ratio it builds its projection (and its own
   frustum culling) from with `movss xmm0, [0x297c68]` (the constant 4/3). That constant is shared with unrelated code, so the lifter
   patches only that one load (`runtime/data/patches.txt`, applied by `tools/lift.py`) to read a runtime-owned word at `0x7E0F0000`, which
   `runtime/main.c` fills with 4/3 or 16/9. The game then produces a real 16:9 projection: more of the world at the sides, same vertical field of
   view, and its culling matches, so objects do not pop in at the edges.
2. **Stretch.** The presenter (`display_metal.m`) shows the 640x480 (or internal-resolution) picture at 16:9 in a 16:9 window, with bars in
   any other window shape and in full screen. The 3D picture is squeezed horizontally in the buffer and looks right when stretched.
3. **Proportional 2D.** HUD, text, menu items and logos are laid out for 4:3, so the vertex shader (`vsh.c`) squeezes every
   orthographic (`w = 1`) vertex to 3/4 about the screen centre, except in full-screen passes (letterbox bars, fades, bloom, backgrounds),
   which `gpu_metal.m` `ui_squeeze()` detects from their window-coordinate extent and leaves alone.
4. **Movies** (video overlay) are 4:3 pictures and are shown pillarboxed in widescreen mode.

## Approximations

* The menu backgrounds and the loading/title art are 4:3 images stretched across the full 16:9 picture; the elements on top keep their proportions.
* A 2D element drawn with a perspective (w != 1) transform, if the game has any, is not squeezed and would look about 33% wide.
* Applying a change to `runtime/data/patches.txt` needs the game to be translated again (`scripts/prepare.sh`).

## Debugging

`STW_WINSIZE=1280x400` opens the window at a given size without the aspect lock (to check bars), `STW_WIDETRACE=1` logs what kinds of draws
make up the main picture.
