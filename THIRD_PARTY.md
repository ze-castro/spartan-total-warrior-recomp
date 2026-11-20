# Third-party material

This repository contains no code or data from the game itself. It builds from a disc image the user supplies.

| Item | Where | License | How it is used |
| --- | --- | --- | --- |
| Xbox kernel export list (`xboxkrnl.def`) | `runtime/data/xboxkrnl.def` | nxdk (MIT-style) | ordinal-to-name table so imported kernel calls can be named and implemented |
| XbSymbolDatabase (Cxbx-Reloaded) OOVPA signatures | **not in this repo**; `scripts/fetch_oovpa.sh` downloads it into `tools/oovpa/`; the release app bundles it unmodified with its license file | code MIT, signature data ODbL-1.0 | recognise statically linked XDK functions (Direct3D, DirectSound, XAPI) in the game's executable |
| Capstone | Python package; a wheel is bundled in the release app (`Resources/vendor`) | BSD-3-Clause | disassembler for the function finder and the lifter |
| Unicorn | Python package, dev-time (`tools/difftest.py` only) | GPL-2.0 | reference emulator used to test the lifter; not linked into the game |
| xemu, Cxbx-Reloaded, nxdk and public NV2A documentation | none copied | various | consulted as references for how the original Xbox GPU, kernel and libraries behave |

If you redistribute anything built from this project, note that the generated translation (`build/lifted.c`, `analysis/`) is derived from the
game's copyrighted executable and must not be published.
