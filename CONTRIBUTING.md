# Contributing

Thanks for looking. A few ground rules that keep the project legal and workable:

* **Never commit anything derived from the game**: the disc image, `game/`, `build/`, `analysis/`, `build/lifted.c`, screenshots that are
  only game art are fine in issues but not in the repo. `.gitignore` already excludes the generated folders.
* **Check the picture, not only the numbers.** Renderer and performance changes must be checked by looking at frames
  (`STW_DUMPFRAME`, windowed `STW_PRESENTDUMP`), not just by fps; a change once made the whole screen black while every benchmark looked fine.
* **Compare old and new back to back** when changing timing-sensitive code (the GPU thread, fences, vblank), with several runs of the same
  scripted input (`STW_SCRIPT`). See `docs/DEBUGGING.md`.
* Match the surrounding style: short comments that say *why*, no new dependencies unless needed.
* CI only compiles the runtime and syntax-checks the tools (it has no game). Say in the pull request how you tested.
