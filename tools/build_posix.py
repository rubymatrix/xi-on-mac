"""Build driver for POSIX hosts (clang): arm64 macOS for R3, Linux as a by-product.

  python3 tools/build_posix.py prepare --game <FINAL FANTASY XI folder>
        verify the player's FFXiMain.dll against the pinned build and unpack FFXiMain.dll and
        FFXi.dll into generated/ (Square Enix code: generated/ is gitignored, never committed)
  python3 tools/build_posix.py boot64 --game <folder>    the boot test (R3.1)
  python3 tools/build_posix.py host64 --game <folder>    the game host, with SDL3
  python3 tools/build_posix.py gfxtest                   the graphics back end and the D3D8 front end,
        offscreen, without the game (tests/gfx_test.c, tests/d3d8_test.c)

The same sources as tools/build.py's boot64/host64 targets, with plat_posix.c for plat_win.c.
Needs: clang (Xcode command line tools), python3 with capstone and pefile, and for host64 SDL3
(`brew install sdl3`, found through pkg-config) and mbedtls (`brew install mbedtls`). The game folder is the retail "FINAL FANTASY XI"
folder copied from a Windows install, with "PlayOnlineViewer" next to it.
"""
import argparse
import concurrent.futures
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402  (constants and source lists; nothing Windows-only runs on import)

ROOT = build.ROOT
GEN_FFXI_IMAGE = build.FFXI_IMAGE
CFLAGS = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable']
# the generated C: every label and local is emitted whether used or not
GEN_WARNINGS = ['-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                '-Wno-parentheses-equality', '-Wno-unreachable-code']
# the graphics back end: Metal on macOS (R3.2), none elsewhere yet
if sys.platform == 'darwin':
    GFX_SOURCES = ['runtime/portable/gfx_msl.c', 'runtime/portable/gfx_msl_shaders.c', 'runtime/portable/gfx_metal.m']
    GFX_LIBS = ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation']
else:
    GFX_SOURCES = ['runtime/portable/gfx_null.c']
    GFX_LIBS = []
HOST_SOURCES = ['runtime/portable/user32.c', 'runtime/portable/d3d8.c', 'runtime/portable/dsound.c',
                'runtime/portable/input.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'host/host64.c',
                'host/lsb_login.c'] + GFX_SOURCES


def posix(p):
    return p.replace('\\', '/')


PORTABLE = [posix(p).replace('plat_win.c', 'plat_posix.c') for p in build.PORTABLE]


def run(cmd, **kw):
    line = ' '.join(shlex.quote(c) for c in cmd)
    print('>', line if len(line) < 200 else line[:200] + ' ...')
    subprocess.check_call(cmd, cwd=ROOT, **kw)


def pkg_config(*args):
    try:
        return subprocess.check_output(['pkg-config'] + list(args), text=True).split()
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit('SDL3 not found through pkg-config: brew install sdl3 pkg-config')


def tls_config():
    """mbedtls (the LandSandBoat sign-in's TLS, host/lsb_login.c): `brew install mbedtls`."""
    try:
        return (subprocess.check_output(['pkg-config', '--cflags', 'mbedtls'], text=True).split(),
                subprocess.check_output(['pkg-config', '--libs', 'mbedtls', 'mbedx509', 'mbedcrypto'], text=True).split())
    except (OSError, subprocess.CalledProcessError):
        raise SystemExit('mbedtls not found through pkg-config: brew install mbedtls')


def newest_header():
    """The newest runtime/host header: a changed struct must rebuild everything that may include it
    (no per-file dependency tracking; the generated code has its own headers, which it rebuilds with)."""
    newest = 0
    for d in ('runtime', 'runtime/portable', 'runtime/win32', 'host'):
        full = os.path.join(ROOT, d)
        if os.path.isdir(full):
            for f in os.listdir(full):
                if f.endswith('.h'):
                    newest = max(newest, os.path.getmtime(os.path.join(full, f)))
    return newest


def compile_stale(sources, objdir, extra):
    """Compiles every source whose object is missing or older than it (or than the newest runtime
    header, for our own sources), in parallel; returns the objects."""
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    headers = newest_header()
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full_obj = os.path.join(ROOT, obj)
        stamp = os.path.getmtime(os.path.join(ROOT, s))
        if not s.startswith('generated/'):
            stamp = max(stamp, headers)
        if not os.path.exists(full_obj) or os.path.getmtime(full_obj) < stamp:
            flags = CFLAGS + extra + (GEN_WARNINGS if s.startswith('generated/') else [])
            if s.endswith('.m'):  # Objective-C: references counted by hand (gfx_metal.m)
                flags = [f for f in flags if f != '-std=c11'] + ['-fno-objc-arc']
            jobs.append(['clang', '-c'] + flags + [s, '-o', obj])
    if jobs:
        print('compiling %d of %d' % (len(jobs), len(sources)))
        failed = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
            for cmd, rc in zip(jobs, ex.map(lambda c: subprocess.call(c, cwd=ROOT), jobs)):
                if rc:
                    failed.append(cmd[-3])
        if failed:
            raise SystemExit('failed: ' + ', '.join(failed))
    return objs


def generated(sub):
    d = os.path.join(ROOT, 'generated', sub)
    return ['generated/%s/%s' % (sub, f) for f in sorted(os.listdir(d)) if f.endswith('.c')]


def prepare(game):
    run([sys.executable, 'tools/prepare.py', '--dll', os.path.join(game, 'FFXiMain.dll')])  # and FFXi.dll


def translate():
    build.recomp('generated/all', ['--all'])


def boot64(game):
    translate()
    # objects live apart from the binaries (build/boot64 is the program); the translation's are
    # shared with host64
    objs = compile_stale(generated('all'), 'build/all64', ['-I', 'generated/all'])
    objs += compile_stale(PORTABLE + ['tests/boot64.c'], 'build/obj/boot64', [])
    run(['clang', '-o', 'build/boot64'] + objs + ['-lm', '-lpthread'])
    run(['build/boot64', build.RETAIL, game])


def host64(game):
    translate()
    run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', GEN_FFXI_IMAGE, '--retail',
         os.path.join(game, 'FFXi.dll'), '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    sdl_cflags, sdl_libs = pkg_config('--cflags', 'sdl3'), pkg_config('--libs', 'sdl3')
    tls_cflags, tls_libs = tls_config()
    objs = compile_stale(generated('all'), 'build/all64', ['-I', 'generated/all'])
    objs += compile_stale(generated('ffxi'), 'build/ffxi64', ['-I', 'generated/ffxi'])
    objs += compile_stale(PORTABLE + HOST_SOURCES, 'build/obj/host64', sdl_cflags + tls_cflags)
    run(['clang', '-o', 'build/host64'] + objs + sdl_libs + tls_libs + GFX_LIBS + ['-lm', '-lpthread'])
    print('built build/host64; run: build/host64 --game %s --session <V>' % shlex.quote(game))


def gfxtest():
    """The back end alone (gfx_test), then the D3D8 front end on it through its COM thunks (d3d8_test)."""
    sdl_cflags, sdl_libs = pkg_config('--cflags', 'sdl3'), pkg_config('--libs', 'sdl3')
    objs = compile_stale(GFX_SOURCES + ['tests/gfx_test.c'], 'build/gfxtest', sdl_cflags)
    run(['clang', '-o', 'build/gfx_test'] + objs + sdl_libs + GFX_LIBS)
    run(['build/gfx_test'])
    objs = compile_stale(PORTABLE + GFX_SOURCES + ['runtime/portable/user32.c', 'runtime/portable/input.c',
                                                   'runtime/portable/d3d8.c', 'tests/d3d8_test.c'], 'build/d3d8test', sdl_cflags)
    run(['clang', '-o', 'build/d3d8_test'] + objs + sdl_libs + GFX_LIBS + ['-lm', '-lpthread'])
    run(['build/d3d8_test'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['prepare', 'boot64', 'host64', 'gfxtest'])
    ap.add_argument('--game', default=os.path.expanduser('~/PlayOnline/SquareEnix/FINAL FANTASY XI'))
    args = ap.parse_args()
    if args.target == 'gfxtest':
        return gfxtest()
    game = os.path.abspath(args.game)
    if not os.path.exists(os.path.join(game, 'FFXiMain.dll')):
        raise SystemExit('no FFXiMain.dll in %s (--game)' % game)
    if args.target == 'prepare':
        prepare(game)
    elif args.target == 'boot64':
        boot64(game)
    else:
        host64(game)


if __name__ == '__main__':
    main()
