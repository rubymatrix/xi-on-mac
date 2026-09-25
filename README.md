# FFXIRecompile

Static recompilation of FINAL FANTASY XI's `FFXiMain.dll` (and `FFXi.dll`) from 32-bit x86 to C,
so the game runs natively on arm64 macOS without Wine or Rosetta.

This repo is the tooling that design calls for: the recompiler, its runtime, and the platform
layer.

## Rules

- **No Square Enix bytes in this repo, ever.** No retail DLLs, no unpacked images, no DAT files, and
  **no generated C** — generated code is derived from the game and is produced on the player's own
  machine from their own install (`client-native-arm64.md` §8). `.gitignore` enforces the obvious
  paths; the rule applies everywhere.
- What *is* committed: the recompiler, the runtime, the platform layer, tests, and per-build
  **metadata** — addresses and shapes only (function ranges, switch tables, tail jumps, manual
  verdicts), keyed by the SHA-256 of the retail DLL.
- One pinned build at a time. Currently `FFXiMain.dll` 2026-08-22,
  SHA-256 `6f8844eb7f0380f30a3db2fc3c435e1145f5c450bdd0999133cc75c516ec3c3b`.

## Inputs

Everything the build reads is in this repository.

| input | here |
| --- | --- |
| per-build metadata `ffxi-recomp-meta/1` (addresses and shapes, no bytes) | `meta/FFXiMain.2026-08-22.meta.json`, `meta/FFXi.2026-08-22.meta.json` |
| static POL1 unpacker | `tools/pol1_unpack.py` |
| the specifications the runtime implements: polcore slots, the polcore and D3D8 surfaces | `specs/` |

These were produced by the discovery pass; the copies here
are the ones the build uses.

## Plan (from `client-native-arm64.md` §7)

| phase | here |
| --- | --- |
| **R2** — recompile, x86-32 Windows | recompiler + runtime; the generated C built as a 32-bit DLL that `pol.exe` loads in place of retail `FFXiMain.dll`. Real Win32 and D3D8 underneath, so any divergence is a recompiler bug. Includes the guest-wide lock. |
| R3 — arm64 macOS | platform layer: Win32 subset, D3D8 (the measured 44-method subset) on Metal, audio, input, sockets |
| R4 — install-time pipeline | unpack + recompile + build on the player's machine |

## Status

- **R2** (2026-09-24): the translated game runs inside `pol.exe` on x86 Windows — zone-in, zoning,
  combat, a busy city. Differential test: 133,681 calls, 0 mismatches. Only MMX/3DNow!/SSE are
  unimplemented, in 195 functions (the runtime reports a CPU without them).
- **R3.1** (started 2026-09-24): the same translation boots on a 64-bit host with the portable
  runtime (`BOOT64 OK`), and `host64` runs FFXi.dll → `GameStart` → FFXiMain to its title-screen
  loop on Windows x64 with sound and input through SDL3 and our own polcore. Nothing is drawn
  yet: the D3D8 front end is in, the Metal back end is next.


```
python tools\prepare.py          # verify + unpack the retail DLL into generated\
python tools\build.py difftest   # x86: translate the CRT slice, differential test
python tools\build.py host       # x86: the stand-in FFXiMain.dll + boot test (R2)
python tools\build.py boot64     # x64: the portable runtime + boot test (R3.1)
python tools\install.py install  # put the stand-in in the game folder (restore: undo)
python tools\trace_report.py <FFXiMain.trace.txt> [--seq <FFXiMain.seq.txt>]   # resolve a boundary trace
python tools\build.py host64     # x64: the game host (SDL3 at C:\Dev\SDL3)
build\host64.exe --game "<FINAL FANTASY XI>" --session <V> [--lobby a.b.c.d]
    # FFXI_RECOMP_TRACE=1: every shim call;  FFXI_RECOMP_MISSING=1: imports without a shim
```

### macOS (arm64)

Copy the Windows install's `SquareEnix` folder (`FINAL FANTASY XI` and `PlayOnlineViewer` side by
side) to the Mac, e.g. `~/PlayOnline/SquareEnix`.
The game sees it as `C:\PlayOnline\SquareEnix`.

```
brew install sdl3 pkg-config
pip3 install capstone pefile
python3 tools/build_posix.py prepare --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py boot64  --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py host64  --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"
build/host64 --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI" --session <V> --lobby <server IP>
```

## Layout

```
recomp/            x86c.py (one function -> C), recomp.py (driver: closure or --all, coverage stats)
runtime/           guest.h (state, memory, x87 helpers), runtime.c (dispatch, traps, cpuid)
runtime/win32/     R2, 32-bit Windows: loader (retail DLL mapped by Windows, entries patched),
                   bridges (host<->guest on the x86 stack), guest lock, boundary trace, profiler
runtime/portable/  R3, 64-bit hosts: plat.h (+ plat_win.c, plat_posix.c), gwin (guest window,
                   pages, heap), gthread (threads, lock, guest_call), thunk (imports -> shims),
                   pe (image loader), k32*/kobj/vfs/reg/ole (Win32), polcore* (our own polcore),
                   user32 + input + dinput + dsound (SDL3), d3d8 (the D3D8 front end), ws2 (sockets)
host/              ffximain.c: the R2 stand-in FFXiMain.dll; host64.c: the R3 game host
tests/             difftest.c (original vs translation), boot.c (x86), boot64.c (x64)
tools/             prepare.py, pol1_unpack.py, build.py (MSVC), build_posix.py (clang), install.py,
                   trace_report.py
meta/              per-build metadata the recompiler reads
specs/             the specifications the runtime implements (polcore slots, D3D8 and polcore surfaces)
generated/         (gitignored) unpacked image and recompiler output
build/             (gitignored)
```
