"""Build driver for POSIX hosts (clang): arm64 macOS for R3, Linux as a by-product.

  python3 tools/build_posix.py prepare --game <FINAL FANTASY XI folder>
        verify the player's FFXiMain.dll against the pinned build and unpack FFXiMain.dll and
        FFXi.dll into generated/ (Square Enix code: generated/ is gitignored, never committed)
  python3 tools/build_posix.py boot64 --game <folder>    the boot test (R3.1)
  python3 tools/build_posix.py host64 --game <folder>    the game host, with SDL3

The same sources as tools/build.py's boot64/host64 targets, with plat_posix.c for plat_win.c.
Needs: clang (Xcode command line tools), python3 with capstone and pefile, and for host64 SDL3
(`brew install sdl3`, found through pkg-config). the research checkout must sit next to this repository,
as on Windows. The game folder is the retail "FINAL FANTASY XI" folder copied from a Windows
install, with "PlayOnlineViewer" next to it.
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
GEN_FFXI_IMAGE = os.path.join(ROOT, 'generated', 'FFXi.unpacked.dll')
CFLAGS = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable']
# the generated C: every label and local is emitted whether used or not
GEN_WARNINGS = ['-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                '-Wno-parentheses-equality', '-Wno-unreachable-code']
HOST_SOURCES = ['runtime/portable/user32.c', 'runtime/portable/d3d8.c', 'runtime/portable/dsound.c',
                'runtime/portable/input.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'host/host64.c']


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


def compile_stale(sources, objdir, extra):
    """Compiles every source whose object is missing or older, in parallel; returns the objects."""
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    objs, jobs = [], []
    for s in sources:
        obj = os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.o')
        objs.append(obj)
        full_obj = os.path.join(ROOT, obj)
        if not os.path.exists(full_obj) or os.path.getmtime(full_obj) < os.path.getmtime(os.path.join(ROOT, s)):
            flags = CFLAGS + extra + (GEN_WARNINGS if s.startswith('generated/') else [])
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
    run([sys.executable, 'tools/prepare.py', '--dll', os.path.join(game, 'FFXiMain.dll')])
    run([sys.executable, os.path.join(build.RE_DIR, 'pol1_unpack.py'), os.path.join(game, 'FFXi.dll'), GEN_FFXI_IMAGE])


def translate():
    build.recomp('generated/all', ['--all'])


def boot64(game):
    translate()
    objs = compile_stale(generated('all') + PORTABLE + ['tests/boot64.c'], 'build/boot64', ['-I', 'generated/all'])
    run(['clang', '-o', 'build/boot64'] + objs + ['-lm', '-lpthread'])
    run(['build/boot64', build.RETAIL, game])


def host64(game):
    translate()
    run([sys.executable, 'recomp/recomp.py', '--meta', build.FFXI_META, '--image', GEN_FFXI_IMAGE, '--retail',
         os.path.join(game, 'FFXi.dll'), '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    sdl_cflags, sdl_libs = pkg_config('--cflags', 'sdl3'), pkg_config('--libs', 'sdl3')
    objs = compile_stale(generated('all'), 'build/all64', ['-I', 'generated/all'])
    objs += compile_stale(generated('ffxi'), 'build/ffxi64', ['-I', 'generated/ffxi'])
    objs += compile_stale(PORTABLE + HOST_SOURCES, 'build/host64', sdl_cflags)
    run(['clang', '-o', 'build/host64'] + objs + sdl_libs + ['-lm', '-lpthread'])
    print('built build/host64; run: build/host64 --game %s --session <V>' % shlex.quote(game))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['prepare', 'boot64', 'host64'])
    ap.add_argument('--game', default=os.path.expanduser('~/PlayOnline/SquareEnix/FINAL FANTASY XI'))
    args = ap.parse_args()
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
