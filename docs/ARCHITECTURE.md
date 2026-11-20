# Architecture

```
disc image ──► tools/xiso_extract.py ──► game/default.xbe
                                            │
                  tools/discover.py ◄───────┤  find every function (trial disassembly, vtables, switch tables)
                  tools/oovpa_match.py ◄────┤  name the statically linked XDK library functions
                  tools/lift.py ────────────┘  x86 → C  ──► build/lifted.c  (~28,000 functions)
                                                             │
runtime/*.c, *.m  (the "console")  ────────────────────────► clang ──► build/stw
```

## The translation (`tools/`)

* **Function discovery** (`discover.py`): seeds from the entry point, direct calls, code pointers in data, and gaps; every candidate is
  trial-disassembled and only kept if it decodes cleanly. Jump tables are resolved and marked as data.
* **Lifter** (`lift.py`): each x86 function becomes a C function over a `CPU` struct of locals with eagerly computed flags. Guest memory is a
  flat 4 GB mapping. It handles the integer instruction set, x87 (80-bit loads/stores), the SSE1 subset and MMX that the game uses, `fs:`
  accesses (per-thread KPCR), `lock`-prefixed atomics, and loop back-edges call `POLL()` so guest threads can be preempted.
* The XDK libraries are in their own sections of the executable (Direct3D, DirectSound, XAPI, XMV, XGRPH, D3DX). The CPU-only ones are
  translated too; the hardware-facing entry points are **overrides**: a hand-written `lib_<Name>` in `runtime/` replaces the function.
* `difftest.py` runs thousands of lifted functions against the Unicorn x86 emulator with random inputs and compares memory and registers.

## The machine (`runtime/`)

| File | Role |
| --- | --- |
| `main.c`, `kernel.c`, `kobj.c`, `kfile.c` | loader, kernel calls (`k_<Export>`), threads, events, timers, DPCs, the NT-path file system, the guest heap |
| `nv2a.c` | the GPU: MMIO registers, the push-buffer reader thread, objects, kelvin methods, interrupts, flip pacing, vblank |
| `vsh.c`, `psh.c` | NV2A vertex programs and register combiners translated to Metal Shading Language |
| `gpu_metal.m` | Metal renderer: render targets as textures, depth/stencil, scaling, batching, texture decoding and caching |
| `display*.c/.m` | scan-out, the Cocoa window, the launcher, the video overlay |
| `audio.c` | DirectSound buffers/streams mixed in software to CoreAudio (Xbox ADPCM decoder included) |
| `input.m`, `xapi.c` | keyboard/mouse bindings and game controllers presented as XInput state |
| `xiso.c`, `paths.c` | reading disc images, data directory and game-root handling |

Guest memory layout: physical RAM is one shared-memory object mapped at 0, at `0x80000000` and at `0xF0000000` (the Xbox's own aliases), only
64 MB of it is handed out (the GPU masks addresses to 26 bits). The NV2A register window at `0xFD000000` is ordinary guest memory whose stores
go through `mmio_write()`.

## Frame flow

The game builds a push buffer; writing `DMA_PUT` wakes the pusher thread, which decodes commands. Draw commands are batched into Metal
command buffers (one render target at a time); fences, semaphores and software-method interrupts are signalled in order. A 60 Hz timer plays
the vblank: it raises the interrupt the game's driver expects, performs the flip and presents the scan-out surface.
