# Debugging

Everything is controlled by environment variables; run from the project folder.

## Headless and scripted runs

| Variable | Effect |
| --- | --- |
| `STW_HEADLESS=1` | no window; the guest runs on the main thread (use with the dump options below) |
| `STW_SCRIPT="30:start;36:a;50:l_up*3"` | scripted pad input: `second:button[*seconds]`. Buttons: `start back a b x y up down left right l_up l_down l_left l_right r_left r_right r_up r_down lt rt white black lclick rclick shift s_up s_down mdx=<px> rx=<n> ry=<n>` |
| `STW_QUITAFTER=<s>` | quit after that many seconds (windowed runs) |
| `STW_NOLAUNCHER=1` | skip the launcher |
| `STW_HOME`, `STW_GAME` | data directory / game folder |

## Looking at output

| Variable | Effect |
| --- | --- |
| `STW_DUMPFRAME=/tmp/f_%03d.ppm` | the shown 1x frame about once a second (`STW_DUMPEVERY=<frames>`, `STW_DUMPFROM=<sec>`) |
| `STW_DUMPFRAME_HI=/tmp/h_%03d.ppm` | the same at the internal resolution |
| `STW_PRESENTDUMP=<n>` | windowed: read back the n-th frame actually presented to the window to `/tmp/stw_present.ppm` |
| `STW_DUMPALL=<sec>` | every known render target to `/tmp/surf_*.ppm` |
| `STW_FILTERDUMP=<n,n,...>` + `STW_FILTERDUMP_PATH=/tmp/f_%d.ppm` | windowed: the n-th presented frame at the internal resolution after the presenter filters |
| `STW_FILTERDUMP_ALL=<n>` | the same frame n through every post filter: `<STW_FILTERDUMP_PATH>_0..3.ppm` = off, FXAA, sharpen 50, both |
| `STW_AASCRIPT="5399:3;5410:0"` / `STW_BLOOMSCRIPT="5395:0;5402:1"` | change the anti-aliasing mode / bloom smoothing at those presented frames (debug; switching MSAA at run time glitches a frame or two) |
| `STW_PRESENTDUMP_PATH` | where the windowed `STW_PRESENTDUMP` goes (default `/tmp/stw_present.ppm`) |
| `STW_DUMPTEX=<hex format>` | dump decoded textures of that format |
| `tools/montage.py out.png a.ppm b.ppm ...`, `tools/ppm2png.py` | view dumps |
| `STW_FPS=1` | per-second frames, draws, CPU, GPU waits (`STW_HEAPLOG=1` adds guest heap use, `STW_POLLSTAT=1` pad polls) |

## Renderer

`STW_RES=0..4` (0 original, 1 720p, 2 1080p, 3 1440p, 4 2160p; the old `STW_SCALE=1..4` still works as whole multiples of 480 lines), `STW_ANISO=1..16`, `STW_AA=0..3` (off / FXAA / MSAA 2x / 4x), `STW_MSAA=2|4`, `STW_SHARPEN=0..100`, `STW_BLOOMHQ=0|1`,
`STW_MSDEBUG=1` (log multisampled targets), `STW_NODEPTH`, `STW_NOBLOOM`, `STW_NOCLAMP`, `STW_DRAWTRACE=1..4`, `STW_MODES`, `STW_METHODS`,
`STW_STENCILSTAT`, `STW_SHADERLOG=<stage program hex>`, `STW_DUMPSHADER`, `STW_TEXTRACE`, `STW_ATTRS`, `STW_GPUTRACE=1|2`.

## Kernel and CPU

`STW_TRACE` (kernel calls), `STW_FILETRACE`, `STW_LIBTRACE`, `STW_INTTRACE`, `STW_MMIOTRACE`, `STW_PORTTRACE`, `STW_FNTRACE=1|ring` with
`STW_DUMPAFTER=<s>`, `STW_TRACEFN=<addr>`, `STW_WATCH=<guest addr>`, `STW_PROTECT=<addr>`, `STW_STALLDUMP` (what every thread waits on when
frames stop). `tools/gdis.py <addr> [n]` disassembles guest code.

## Controllers

`STW_FAKEPAD=<n>` starts with n fake controllers (the only way to test a pad in headless runs); `STW_FAKEPAD_SCRIPT="2:plug;5:a=1,lt=0.6,lx=0.8;6:a=0;9:unplug"` is
a time line in seconds (`p1.` prefix = second pad; names `a b x y lb rb lt rt menu options home ls rs dup ddown dleft dright paddle1-4 touchpad`, sticks `lx ly rx ry`),
`STW_PADLOG=1` prints connections and the device changes the game is told about, `STW_RUMBLETRACE=1` prints the game's rumble requests and what is sent to the pad,
`STW_REALPAD=1` uses real controllers in headless runs.

## Audio, input

`STW_AUDIODUMP=<file.wav>`, `STW_AUDIOTRACE`, `STW_AUDIOSAVE`, `STW_NOAUDIO`, `STW_INPUTLOG` (and `/tmp/stw_input.log` for windowed runs).

## Method for performance or timing changes

1. Run the same `STW_SCRIPT` several times with the old and the new binary, alternating.
2. Check frames by *looking* (`STW_DUMPFRAME` and a windowed `STW_PRESENTDUMP`), not only fps.
3. Run 5-minute soaks and look for seconds at 0 fps (`STW_STALLDUMP=1` explains a freeze).
