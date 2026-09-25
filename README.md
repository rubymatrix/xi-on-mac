# FFXI on Mac

Static recompilation of FINAL FANTASY XI's `FFXiMain.dll` (and `FFXi.dll`) from 32-bit x86 to C,
so the game runs natively on arm64 macOS without Wine or Rosetta. This repo holds the recompiler,
its runtime, the platform layer (Win32, Direct3D 8 on Metal, audio, input, sockets) and `host64`,
the launcher that signs in and runs the game.

## Rules

- **No Square Enix bytes in this repo, ever.** No retail DLLs, no unpacked images, no DAT files, and
  **no generated C**. Generated code is derived from the game, so it is produced on the player's
  own machine from their own install. `.gitignore` enforces the obvious paths; the rule applies
  everywhere.
- What *is* committed: the recompiler, the runtime, the platform layer, tests, and per-build
  **metadata**: addresses and shapes only (function ranges, switch tables, tail jumps), keyed by
  the SHA-256 of the retail DLL.
- Every supported build is listed in `meta/builds.json`, keyed by the SHA-256 of its retail
  `FFXiMain.dll` and `FFXi.dll`. `tools/prepare.py` identifies the install's build and records it in
  `generated/build.json`; the build tools read it from there.

  | build | `FFXiMain.dll` SHA-256 | client version | taken from |
  | --- | --- | --- | --- |
  | 2026-08-22 | `6f8844eb…3c3b` | `30260805_0` | retail PlayOnline, `C:\Program Files (x86)\PlayOnline` |
  | 2026-09-03 | `f2245d1c…23e4` | `30260903_0` | a private-server install (no `patch.ver`) |

  New labels are the date of the PE timestamp (2026-08-22 predates that rule).

## Inputs

Everything the build reads is in this repository.

| input | here |
| --- | --- |
| per-build metadata `ffxi-recomp-meta/1` (addresses and shapes, no bytes) | `meta/<module>.<build>.meta.json` |
| the builds, their hashes, and the few addresses the runtime and tests name | `meta/builds.json` (written to `generated/build.h`) |
| static POL1 unpacker | `tools/pol1_unpack.py` |
| the specifications the runtime implements: polcore slots, the polcore and D3D8 surfaces | `specs/` |

The metadata comes from a Ghidra-based discovery pass over each build's unpacked DLLs. That tool is
not in this repository; the committed metadata is what the build uses.

## Plan

| phase | what |
| --- | --- |
| **R2**: recompile, x86-32 Windows | recompiler + runtime; the generated C built as a 32-bit stand-in `FFXiMain.dll` that `pol.exe` loads. Real Win32 and D3D8 underneath, so any divergence is a recompiler bug. |
| **R3**: arm64 macOS | platform layer: Win32 subset, D3D8 on Metal, audio, input, sockets; `host64` in place of `pol.exe` |
| R4: install-time pipeline | unpack + recompile + build on the player's machine |

## Status

- **R2** (2026-09-24): the translated game runs inside `pol.exe` on x86 Windows: zone-in, zoning,
  combat, a busy city. Differential test: 133,681 calls, 0 mismatches. Only MMX/3DNow!/SSE are
  unimplemented, in 194 functions (the runtime reports a CPU without them).
- **R3.1** (2026-09-24): the same translation boots on a 64-bit host with the portable runtime
  (`BOOT64 OK`); `host64` runs FFXi.dll → `GameStart` → FFXiMain with sound and input through SDL3
  and our own polcore.
- **R3.2** (2026-09-24): the Metal back end under the D3D8 front end. On arm64 macOS the game signs
  in, zones in and renders the world at about 58 fps in busy scenes (M1 Max, 4096×4096
  background). Pipelines build off the game's thread and are cached across sessions. Windows x64
  still draws nothing (`gfx_null.c`).
- **2026-09-25**: build 2026-09-03 plays on macOS through a LandSandBoat server with
  no PlayOnline. DAT overlays (`--dats`) added.

## Building

### macOS (arm64)

Copy the Windows install's `SquareEnix` folder (`FINAL FANTASY XI` and `PlayOnlineViewer` side by
side) to the Mac, e.g. `~/PlayOnline/SquareEnix`. The game sees it as `C:\PlayOnline\SquareEnix`.

```
brew install sdl3 pkg-config mbedtls
pip3 install capstone pefile
python3 tools/build_posix.py prepare --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"
python3 tools/build_posix.py boot64  --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"   # boot test
python3 tools/build_posix.py host64  --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI"   # build/host64
python3 tools/build_posix.py gfxtest     # Metal back end + D3D8 front end, offscreen, no game needed
build/gfx_test --window                  # the same, then two seconds of frames to a window
    # MTL_DEBUG_LAYER=1: Metal API validation
```

Run `prepare` again whenever the install changes (a new game version); `host64` and `boot64`
re-translate and rebuild what changed.

### Windows

```
python tools\prepare.py [--game "<FINAL FANTASY XI>"]   # identify the build, unpack into generated\
python tools\build.py difftest   # x86: translate the CRT slice, differential test
python tools\build.py host       # x86: the stand-in FFXiMain.dll + boot test (R2)
python tools\build.py boot64     # x64: the portable runtime + boot test (R3.1)
python tools\install.py install  # put the stand-in in the game folder (restore: undo)
python tools\trace_report.py <FFXiMain.trace.txt> [--seq <FFXiMain.seq.txt>]   # resolve a boundary trace
python tools\build.py host64     # x64: the game host (SDL3 at C:\Dev\SDL3)
```

## Running: `host64`

`host64` does what `pol.exe` and COM do for retail FFXI: it maps the game's DLLs, starts them, and
runs the game with its own polcore in place of PlayOnline's. It takes its options as
`--name value` pairs.

```
build/host64 --game <FINAL FANTASY XI folder> [options]
```

There are two ways to sign in. Use one.

**A LandSandBoat server** (xiloader's protocol, no PlayOnline). This is the usual way to play on a
private server:

```
build/host64 --game ~/PlayOnline/SquareEnix/"FINAL FANTASY XI" \
    --server <server name> --user <account> \
    --reg playonline.reg --reg-overlay build/settings.reg --dats ~/FFXI/DATs
```

**A server with PlayOnline behind it**: pass the session value V that a PlayOnline sign-in
returned, and the lobby checks it:

```
build/host64 --game ... --server <name or a.b.c.d> --session <V>
```

### Options

| option | what it does |
| --- | --- |
| `--game <folder>` | **Required.** The `FINAL FANTASY XI` folder, with `PlayOnlineViewer` beside it. On macOS it is mounted at `C:\PlayOnline\SquareEnix\FINAL FANTASY XI`. |
| `--server <name or a.b.c.d>` | Where the game's servers are. `ffxi00.pol.com` (the lobby) and every other `*.pol.com` name resolve here instead of through DNS; a LandSandBoat sign-in connects here too. Default `127.0.0.1`. `--pol-server` and `--lobby` are older names for it. |
| `--user <account>` | Sign in to a LandSandBoat server with this account, before anything is loaded. |
| `--pass <password>` | The account's password. Without it, `host64` reads `FFXI_PASSWORD`, else asks on the terminal (the prompt does not echo). Prefer the prompt: a password on the command line ends up in your shell history. |
| `--otp <code>` | The two-factor code, for an account that has one. |
| `--login-token <token>` | A single-use launch token from a server's own launcher (for example a Discord login). It stands in for the password. |
| `--authport`, `--dataport`, `--viewport` | LandSandBoat's ports, as xiloader: 54231 (sign-in, TLS), 54230 (data), 54001 (lobby view). |
| `--session <V>` | A server with PlayOnline behind it: the session value its lobby checks, as 16 characters or 32 hex digits. |
| `--reg <file.reg>` | A registry export to load (up to 8; later files win). The game reads its settings (resolution, window mode, sound) from `HKLM\SOFTWARE\PlayOnlineUS`. `playonline.reg` in this repo is a starting point. |
| `--reg-overlay <file.reg>` | Where the game saves settings it changes. It is loaded last, and the `--reg` files are never rewritten. |
| `--dats <folder>` | DAT overlays, the way XIPivot does them (up to 8; the first folder given wins). See below. |
| `--fps-divisor <n>` | The game's frame divisor: `1` is 60 fps (the default here), `2` is 30 fps as shipped. |

The install folder is never written. The registry's install paths are set to where the game
actually is, and an install that has no `patch.ver` (common for private-server installs) gets one
for its build's version, kept next to `host64`.

### DAT overlays (`--dats`)

A private server often ships its own DATs: era item and spell text, zones, menus. `--dats` loads
them without touching the install. A folder can be:

- **one overlay**: it holds `ROM`, `ROM2`, …, `sound`, `sound2`, … folders laid out like the
  install's; or
- **a folder of overlays**: each subfolder that holds those is an overlay, in name order.

```
DATs/
  era-dats/
    ROM/301/12.DAT
    ROM2/14/5.DAT
    ROM255/6/1.DAT
```

When the game opens a path through a `ROM<n>\` or `sound<n>\` folder, `host64` looks for the rest of
the path (`ROM2\14\5.DAT`, ignoring case) in the overlays first, then in the install. Folders that
exist only in an overlay (`ROM255` above) work too. The overlays are indexed once at start-up, and
the log reports each one: `[recomp] dats: era-dats, 163 files`.

### Environment variables

| variable | what it does |
| --- | --- |
| `FFXI_PASSWORD` | The LandSandBoat password when `--pass` is not given. |
| `FFXI_DATS_TRACE=1` | Log every file an overlay supplies: `[dats] <game path> -> <overlay file>`. |
| `FFXI_PROFILE=1` | Every 2 seconds, log a frame breakdown (game code, API calls, draws, GPU time) and the most-called APIs. |
| `FFXI_FPS=0` | Hide the frame-rate overlay. |
| `FFXI_PROBE=gpu` | Read the game's 16×16 occlusion probe from the GPU. By default it answers "visible" at once, which saves 7–8 ms a frame. |
| `FFXI_ASYNC_READBACK=1` | Small read-only surface locks take the newest finished copy instead of waiting for the GPU. |
| `FFXI_CACHE_DIR` | Where the pipeline cache goes. Default `~/Library/Caches/FFXI`. |
| `FFXI_RECOMP_TRACE=1` | Log every shim call, and every failed `CreateFileA` / `FindFirstFileA` path. |
| `FFXI_RECOMP_MISSING=1` | Log imports that have no shim. |
| `MTL_DEBUG_LAYER=1` | Metal API validation. |

## Supporting a new client version

A game update replaces `FFXiMain.dll` and often `FFXi.dll`. The metadata holds absolute addresses,
so every build needs its own entry before it can be translated. `prepare` refuses a build it
does not know:

```
.../FFXiMain.dll is build <sha256>, which meta/builds.json does not know
```

1. **Hash the new DLLs.**

   ```
   shasum -a 256 "<FINAL FANTASY XI>/FFXiMain.dll" "<FINAL FANTASY XI>/FFXi.dll"
   ```

2. **Metadata.** Unpack the new DLLs (`tools/pol1_unpack.py <dll> <out>`) and run them through the
   discovery pass (not in this repo) to produce `meta/FFXiMain.<build>.meta.json` and
   `meta/FFXi.<build>.meta.json`. Name the build after the date of the PE timestamp. When a DLL's
   `.text` is byte-identical to a known build, its metadata carries over; only the hash changes.

3. **Add the build to `meta/builds.json`.** Copy the newest entry and update:
   - `FFXiMain.dll` / `FFXi.dll`: `sha256` of the **retail** (packed) files, and their `meta` file names.
   - `version`: the client version string. Take it from the install's `patch.ver`; for an
     install with none, use the date of the PE timestamp as `3YYYYMMDD_0` (2026-09-03 →
     `30260903_0`). The lobby compares it with the server's `CLIENT_VER`, and `host64` writes it
     into the `patch.ver` it makes.
   - `addresses`: `chars_ptr` (the global the character list hangs from, read by polcore) and
     `present_site` (the return address of the game's `IDirect3DDevice8::Present` call, used on
     Windows). Find the same code in the new build; it usually moves by a few bytes.
   - `crt`: the addresses of the CRT functions the differential test compares (`strlen`, `memcpy`,
     `_ftol`, …).

4. **Prepare and build.**

   ```
   python3 tools/build_posix.py prepare --game "<FINAL FANTASY XI>"   # should report the new build
   python3 tools/build_posix.py boot64  --game "<FINAL FANTASY XI>"   # expect BOOT64 OK
   python3 tools/build_posix.py host64  --game "<FINAL FANTASY XI>"
   ```

   On Windows also run `python tools\build.py difftest` (expect 0 mismatches) and
   `python tools\build.py host`.

5. **Play it.** Sign in, zone in, fight, and zone again. A crash like

   ```
   [recomp] FATAL at 100542e0: indirect call/jump to an address with no translation
   ```

   means the game reached code the metadata does not list as a function. The recompiler already
   makes an entry of every code address the image's data points at (vtables, callbacks,
   exception handlers), including small functions the discovery pass folded into a neighbour, so
   what remains is a real gap: add the function to the metadata and rebuild.

6. **Record it.** Add the build to the table under *Rules*, and commit the metadata and
   `builds.json` together. Never commit anything from `generated/`.

## Layout

```
recomp/            x86c.py (one function -> C), recomp.py (driver: closure or --all, coverage stats)
runtime/           guest.h (state, memory, x87 helpers), runtime.c (dispatch, traps, cpuid)
runtime/win32/     R2, 32-bit Windows: loader (retail DLL mapped by Windows, entries patched),
                   bridges (host<->guest on the x86 stack), guest lock, boundary trace, profiler
runtime/portable/  R3, 64-bit hosts: plat.h (+ plat_win.c, plat_posix.c), gwin (guest window,
                   pages, heap), gthread (threads, lock, guest_call), thunk (imports -> shims),
                   pe (image loader), k32*/kobj/vfs/reg/ole (Win32; vfs also does the DAT overlays),
                   polcore* (our own polcore), user32 + input + dinput + dsound (SDL3),
                   d3d8 (the D3D8 front end), ws2 (sockets), gfx.h (the graphics back end):
                   gfx_metal.m (Metal) + gfx_msl*.c (D3D8 state and shaders -> MSL), gfx_null.c (elsewhere)
host/              ffximain.c: the R2 stand-in FFXiMain.dll; host64.c: the R3 game host;
                   lsb_login.c: the LandSandBoat sign-in
tests/             difftest.c (original vs translation), boot.c (x86), boot64.c (x64),
                   gfx_test.c (the Metal back end), d3d8_test.c (the D3D8 front end on it)
tools/             prepare.py, buildinfo.py, pol1_unpack.py, build.py (MSVC), build_posix.py (clang),
                   install.py, trace_report.py
meta/              builds.json, and the per-build metadata the recompiler reads
specs/             the specifications the runtime implements (polcore slots, D3D8 and polcore surfaces)
playonline.reg     the PlayOnlineUS registry keys, a starting point for --reg
generated/         (gitignored) unpacked image and recompiler output
build/             (gitignored)
```
