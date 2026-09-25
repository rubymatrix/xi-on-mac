# FFXI on Mac

> [!WARNING]
> **This does not work with retail FINAL FANTASY XI or Square Enix's PlayOnline service, and is
> not meant to.** It is for private servers only: LandSandBoat servers and private servers that
> run their own PlayOnline emulation. Its PlayOnline sign-in speaks to such emulated servers; it
> is not a client for Square Enix's PlayOnline network, it cannot sign in to or play on the
> official FINAL FANTASY XI worlds, and it must not be pointed at them. Using unofficial clients on
> Square Enix's service breaks its Terms of Service. This project is not affiliated with, endorsed
> by, or supported by Square Enix. FINAL FANTASY XI and PlayOnline are trademarks of Square Enix;
> you need your own legitimately obtained copy of the game's files.

Static recompilation of FINAL FANTASY XI's `FFXiMain.dll` (and `FFXi.dll`) from 32-bit x86 to C,
so the game runs natively on arm64 macOS without Wine or Rosetta. This repo holds the recompiler,
its runtime, the platform layer (Win32, Direct3D 8 on Metal, audio, input, sockets), `host64`,
the game host, with its own sign-in screen drawn in the game's UI art, and the older launcher, a
desktop app for macOS and Windows that keeps your accounts and settings, signs in and starts
`host64`.

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

The metadata comes from a Ghidra-based discovery pass over each build's unpacked DLLs:
`discovery/` (Ghidra post-scripts and the per-build manual verdicts), run by `tools/discover.py`.
The committed metadata is what the build uses; the pass is only needed for a new build.

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

The launcher needs Rust and the Tauri CLI (`cargo install tauri-cli`). Build `host64` first; the
launcher target puts it inside the app:

```
python3 tools/build_posix.py launcher    # build/FFXI Launcher.app, build/pol-signin, PlayOnline tests
```

### Windows

```
python tools\prepare.py [--game "<FINAL FANTASY XI>"]   # identify the build, unpack into generated\
python tools\build.py difftest   # x86: translate the CRT slice, differential test
python tools\build.py host       # x86: the stand-in FFXiMain.dll + boot test
python tools\build.py boot64     # x64: the portable runtime + boot test
python tools\install.py install  # put the stand-in in the game folder (restore: undo)
python tools\trace_report.py <FFXiMain.trace.txt> [--seq <FFXiMain.seq.txt>]   # resolve a boundary trace
python tools\build.py host64     # x64: the game host (SDL3 at C:\Dev\SDL3)
python tools\build.py launcher   # x64: build\ffxi-launcher.exe beside host64.exe (Rust, cargo install tauri-cli)
```

## The sign-in screen and `Final Fantasy XI.app`

Started without a way in on its command line (no `--session`, no `--user` with a password),
`host64` shows its own sign-in screen before the game, in the game's own UI art read from the
install (window themes, font, title art; `host/datui.c`), in the window the game then takes over.
It signs in to a **LandSandBoat server** (username, password, one-time code) or a **private
PlayOnline server** (PlayOnline ID, password), switched in its **Settings** with the server,
whether to remember the password (macOS Keychain) and the window theme. It keeps its files in
`~/Library/Application Support/FFXIRecompile/FFXI` (or `--data-dir`): `signin.cfg`,
`settings.reg` (the display settings, written once with defaults), `saved.reg` (the game's own
saves), `host64.log` when started from Finder, and an optional `background.png`/`.jpg` behind the
screen. Cmd+Q, the Dock's Quit and Ctrl+C quit at any point.

`python3 tools/build_posix.py app` makes `build/Final Fantasy XI.app`: `host64` with its libraries
and `playonline.reg`, and first-run defaults in its `Info.plist` so it starts from Finder with no
command line (`host/appdefaults.h`):

```
python3 tools/build_posix.py app --game <FINAL FANTASY XI folder> --sign-in lsb --server <name> \
    --resolution 2560x1440 --menu-resolution 1280x720 --window-mode 3 --background <picture>
```

The values go into the built app only. It is signed with the code-signing identity
`FFXI Local Code Signing` when the login keychain has one (or `--sign-identity`), else ad hoc. Ad hoc,
every rebuild is a new app to macOS, which then asks again before the app reads its saved password;
signed with one certificate, "Always Allow" holds. Make the certificate once (self-signed; it needs no
trust settings), with a `cs.cnf` of:

```
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = FFXI Local Code Signing
[ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
```

then:

```
/usr/bin/openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 3650 -config cs.cnf
/usr/bin/openssl pkcs12 -export -inkey key.pem -in cert.pem -name "FFXI Local Code Signing" -out cs.p12 -passout pass:x
security import cs.p12 -k ~/Library/Keychains/login.keychain-db -P x -T /usr/bin/codesign
rm key.pem cs.p12
``` The button art is the screen's own
(`tools/make_ui_art.py`, `assets/ui/`); `third_party/stb/stb_image.h` (public domain) reads
pictures.

## The launcher

`launcher/` is the same app on macOS and Windows (Tauri: Rust, with the UI in `launcher/ui`). It
does what the PlayOnline Viewer and FINAL FANTASY XI's config tool do:

- **Accounts.** Each one is either a **LandSandBoat server** (the account name; `host64` signs in
  itself, xiloader's protocol) or a **PlayOnline server** (the PlayOnline ID; the launcher signs in
  over the retail PlayOnline protocol, `launcher/pol/`, hands `host64` the session value with
  `--session`, and keeps the PlayOnline session up while the game runs). Passwords are kept in the
  system keychain (macOS Keychain, Windows Credential Manager), never in the settings file.
- **Game settings.** Window mode and resolution, menu and background resolution, the graphics
  options, sound, frame rate. They are written as `settings.reg` and given to `host64` with
  `--reg-final`, loaded after the game's own saves (`saved.reg`, `--reg-overlay`), so the launcher's
  values win.
- **Paths.** The `FINAL FANTASY XI` folder, DAT overlays, and optionally which `host64` and base
  registry file to use. By default `host64` is the one inside the app (macOS) or beside the
  launcher (Windows), and the base registry is the bundled `playonline.reg`.

Its settings file is `launcher.json` in the app's config folder
(`~/Library/Application Support/com.rubymatrix.xi-launcher` on macOS, `%APPDATA%\com.rubymatrix.xi-launcher`
on Windows), next to `settings.reg` and `saved.reg`. The game's output is shown in the window and
written to `host64.log` in the app's log folder. For a LandSandBoat account the password reaches
`host64` in `FFXI_PASSWORD`, not on its command line.

`build/pol-signin <PlayOnline ID> [password] [--host h]` signs in without the game and prints the
session value, for testing a server.

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
| `--reg-overlay <file.reg>` | Where the game saves settings it changes. It is loaded after the `--reg` files, and those are never rewritten. |
| `--data-dir <folder>` | Where `host64` writes its own files (the `patch.ver` it makes for an install without one). Default: beside `host64`. |
| `--reg-final <file.reg>` | Loaded after the overlay, so its values win over what the game saved (up to 8). The launcher's game settings. |
| `--dats <folder>` | DAT overlays, the way XIPivot does them (up to 8; the first folder given wins). See below. |
| `--fps-divisor <n>` | The game's frame divisor: `1` is 60 fps (the default here), `2` is 30 fps as shipped. |
| `--aspect <auto, off or w:h>` | The 3D scene's aspect ratio. `auto` (the default) follows the window's shape, as Ashita's aspect addon does, so a widescreen or ultrawide window sees more to the sides instead of a 4:3 view stretched across it. `off` leaves it to the game; a shape (`16:9`, `1.778`) fixes it. |
| `--ui-aspect <w:h>` | Keep the interface at this shape, full height and centered, in a wider window (`16:9` on an ultrawide), instead of stretched across it. The mouse is mapped to match, so the sides outside the box can't be clicked. Off by default. |
| `--nameplates fix\|off` | The names over characters' heads. The game sizes them across by the window's width and down by its height, so they widen with the window (1.8 times at 3440x1440); `fix`, the default, keeps the shape they have in a 4:3 window. Builds with a `nameplate_scale` hook in `meta/builds.json` only. |
| `--nameplate-scale <s>` | Their size: `1.25`, or across x down (`1x1.2`). 1 by default. |

The install folder is never written. The registry's install paths are set to where the game
actually is, and an install that has no `patch.ver` (common for private-server installs) gets one
for its build's version, kept next to `host64` (or in `--data-dir`).

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
| `FFXI_DRAWLOG=<file>` | While `<file>.go` exists, write the next frame's draws to `<file>` (return addresses on the guest stack, texture, vertex box), then remove `.go`. For finding which game code draws what. |
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

In Claude Code, the `game-version-update` skill (`.claude/skills/`) runs all of this: hand it the
install folder. By hand:

1. **Identify and unpack.**

   ```
   python tools/newbuild.py identify --game "<FINAL FANTASY XI>"   # hashes, label, version
   python tools/newbuild.py unpack   --game "<FINAL FANTASY XI>"   # into generated/images/<label>/
   ```

   The label is the date of `FFXiMain.dll`'s PE timestamp. The version is decrypted from the
   install's `patch.ver`; an install with none gets `30` + the timestamp's YYMMDD
   (2026-09-03 → `30260903_0`), which the lobby compares with the server's `CLIENT_VER` and
   `host64` writes into the `patch.ver` it makes. `unpack` also says whether each DLL's `.text`
   is byte-identical to a known build's (the previous build's images must be in
   `generated/images/`: `prepare` keeps one per build).

2. **Carry the previous build over.**

   ```
   python tools/newbuild.py carry --from <previous> --to <label> [--write]
   ```

   Maps every address the previous build's entry names onto the new image: `chars_ptr` (the
   global the character list hangs from, read by polcore), `present_site` (the return address of
   the game's `IDirect3DDevice8::Present` call, used on Windows), the CRT functions the
   differential test compares, and the manual verdicts in `discovery/verdicts.py`. `--write` adds
   the build to `meta/builds.json` and `discovery/verdicts.py`. Addresses it cannot map come with
   a hint (how their neighbours moved); check each with
   `python tools/newbuild.py dis --label <build> --at <addr>` in both builds and fill it in.

3. **Metadata.** A DLL whose `.text` is identical to the previous build's carries its metadata
   over (`python tools/newbuild.py meta --from <previous> --to <label> --module FFXi.dll`). Otherwise:

   ```
   python tools/discover.py --label <label> --module FFXiMain.dll   # Ghidra headless; minutes
   ```

   It stops before exporting if a decode conflict, a function that looks like data, or an
   unaudited switch remains; the report says which. Each needs a manual verdict in
   `discovery/verdicts.py` (see the entries there and `discovery/notes/`); then rerun with
   `--reuse`.

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

6. **Record it.** Add the build to the table under *Rules*, write its discovery notes in
   `discovery/notes/`, and commit the metadata, `builds.json` and `discovery/verdicts.py`
   together. Never commit anything from `generated/`.

## Layout

```
recomp/            x86c.py (one function -> C), recomp.py (driver: closure or --all, coverage stats)
runtime/           guest.h (state, memory, x87 helpers), runtime.c (dispatch, traps, cpuid)
runtime/win32/     32-bit Windows: loader (retail DLL mapped by Windows, entries patched),
                   bridges (host<->guest on the x86 stack), guest lock, boundary trace, profiler
runtime/portable/  64-bit hosts: plat.h (+ plat_win.c, plat_posix.c), gwin (guest window,
                   pages, heap), gthread (threads, lock, guest_call), thunk (imports -> shims),
                   pe (image loader), k32*/kobj/vfs/reg/ole (Win32; vfs also does the DAT overlays),
                   polcore* (our own polcore), user32 + input + dinput + dsound (SDL3),
                   d3d8 (the D3D8 front end), ws2 (sockets), gfx.h (the graphics back end):
                   gfx_metal.m (Metal) + gfx_msl*.c (D3D8 state and shaders -> MSL), gfx_null.c (elsewhere)
host/              ffximain.c: the 32-bit stand-in FFXiMain.dll; host64.c: the 64-bit game host;
                   lsb_login.c: the LandSandBoat sign-in
launcher/          the launcher (Tauri): src-tauri/ (Rust: accounts, settings, starting host64),
                   ui/ (HTML/JS), pol/ (PlayOnline sign-in in portable C, and pol_signin.c)
tests/             difftest.c (original vs translation), boot.c (x86), boot64.c (x64),
                   gfx_test.c (the Metal back end), d3d8_test.c (the D3D8 front end on it),
                   polcrypt_test.c (the launcher's PlayOnline primitives)
tools/             prepare.py, buildinfo.py, pol1_unpack.py, build.py (MSVC), build_posix.py (clang),
                   install.py, trace_report.py; newbuild.py and discover.py (a new client version)
discovery/         the discovery pass: Ghidra (Jython) post-scripts, verdicts.py (the manual verdicts
                   per build), notes/ (what each build's run found)
meta/              builds.json, and the per-build metadata the recompiler reads
.claude/skills/    game-version-update (-analyze, -build): a new client version end to end
specs/             the specifications the runtime implements (polcore slots, D3D8 and polcore surfaces)
playonline.reg     the PlayOnlineUS registry keys, a starting point for --reg
generated/         (gitignored) unpacked images (images/<build>/), discovery projects, recompiler output
build/             (gitignored)
```
