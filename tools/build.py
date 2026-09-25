"""Build driver (MSVC).

  python tools/build.py difftest   translate the CRT slice, build and run tests/difftest.c (x86)
  python tools/build.py host       translate everything, build the stand-in FFXiMain.dll and
                                   tests/boot.exe, and run the boot test (x86, R2)
  python tools/build.py boot64     the same translation built for a 64-bit host with the portable
                                   runtime (runtime/portable), and tests/boot64.exe run (x64, R3.1)

Needs generated/FFXiMain.unpacked.dll (python tools/prepare.py). Incremental: the recompiler
only rewrites C files whose text changed, and only stale objects are recompiled.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RE_DIR = os.path.normpath(os.path.join(ROOT, '..', 'ffxi-re', 'recomp'))
META = os.path.join(RE_DIR, 'FFXiMain.2026-08-22.meta.json')
IMAGE = os.path.join(ROOT, 'generated', 'FFXiMain.unpacked.dll')
RETAIL = os.path.join(ROOT, 'generated', 'FFXiMain.retail.dll')  # verified copy, written by prepare.py
SLICE = ('0x10317480,0x10312980,0x10312090,0x10317270,0x10316dd0,0x103169e0,0x10315c20,0x10316840,'
         '0x10316970,0x103168f0,0x10312a60,0x103152d0,0x10311c6c,0x10312800,0x10322bf0')
CFLAGS = ['/nologo', '/O2', '/MT', '/W3', '/std:c11', '/bigobj', '/I', 'runtime', '/I', 'runtime\\win32']
# 64-bit hosts: guest memory is a window (guest.h) and the runtime is runtime/portable
CFLAGS64 = ['/nologo', '/O2', '/MT', '/W3', '/std:c11', '/bigobj', '/DRT_GUEST_WINDOW', '/I', 'runtime', '/I',
            'runtime\\portable']
PORTABLE = ['runtime\\runtime.c', 'runtime\\portable\\plat_win.c', 'runtime\\portable\\gwin.c',
            'runtime\\portable\\gthread.c', 'runtime\\portable\\thunk.c', 'runtime\\portable\\pe.c',
            'runtime\\portable\\k32.c', 'runtime\\portable\\polcore.c', 'runtime\\portable\\vfs.c',
            'runtime\\portable\\kobj.c', 'runtime\\portable\\k32_io.c', 'runtime\\portable\\polcore_slots.c',
            'runtime\\portable\\reg.c', 'runtime\\portable\\ole.c', 'runtime\\portable\\polcore_files.c',
            'runtime\\portable\\k32_misc.c', 'runtime\\portable\\polcore_polpro.c']


def msvc_env(arch='x86'):
    pf = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = os.path.join(pf, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    vs = subprocess.check_output([vswhere, '-latest', '-products', '*', '-property', 'installationPath'], text=True).strip()
    vcvars = os.path.join(vs, 'VC', 'Auxiliary', 'Build', 'vcvarsall.bat')
    env = dict(os.environ)
    env['PATH'] = os.path.dirname(vswhere) + ';' + env['PATH']
    out = subprocess.check_output('"%s" %s >nul && set' % (vcvars, arch), shell=True, text=True, env=env)
    for line in out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    return env


def run(cmd, env=None):
    print('>', ' '.join(cmd) if len(' '.join(cmd)) < 200 else ' '.join(cmd)[:200] + ' ...')
    if env is not None:
        # CreateProcess searches the parent's PATH, not env's: resolve cl/link against the VS PATH.
        exe = shutil.which(cmd[0], path=env['PATH'])
        if exe:
            cmd = [exe] + cmd[1:]
    if not os.path.isabs(cmd[0]) and os.path.exists(os.path.join(ROOT, cmd[0])):
        cmd = [os.path.join(ROOT, cmd[0])] + cmd[1:]  # relative programs are ours, under ROOT
    subprocess.check_call(cmd, cwd=ROOT, env=env)


def compile_stale(env, sources, objdir, extra, cflags=CFLAGS):
    os.makedirs(os.path.join(ROOT, objdir), exist_ok=True)
    stale = []
    for s in sources:
        obj = os.path.join(ROOT, objdir, os.path.splitext(os.path.basename(s))[0] + '.obj')
        if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(os.path.join(ROOT, s)):
            stale.append(s)
    if stale:
        print('compiling %d of %d' % (len(stale), len(sources)))
        run(['cl', '/c', '/MP'] + cflags + extra + stale + ['/Fo:%s\\' % objdir], env)
    return [os.path.join(objdir, os.path.splitext(os.path.basename(s))[0] + '.obj') for s in sources]


def recomp(out, extra):
    run([sys.executable, 'recomp/recomp.py', '--meta', META, '--image', IMAGE, '--retail', RETAIL, '--out', out] + extra)


def difftest(env):
    recomp('generated/slice', ['--functions', SLICE])
    gen = ['generated\\slice\\' + f for f in sorted(os.listdir(os.path.join(ROOT, 'generated', 'slice'))) if f.endswith('.c')]
    objs = compile_stale(env, gen + ['runtime\\runtime.c', 'tests\\difftest.c'], 'build\\slice', ['/I', 'generated\\slice'])
    run(['link', '/nologo', '/OUT:build\\difftest.exe', '/MACHINE:X86', '/DYNAMICBASE:NO', '/BASE:0x00400000'] + objs, env)
    run(['build\\difftest.exe'], env)


def host(env):
    recomp('generated/all', ['--all'])
    gen = ['generated\\all\\' + f for f in sorted(os.listdir(os.path.join(ROOT, 'generated', 'all'))) if f.endswith('.c')]
    objs = compile_stale(env, gen + ['runtime\\runtime.c', 'runtime\\win32\\bridge.c', 'runtime\\win32\\loader.c',
                                     'host\\ffximain.c'], 'build\\all', ['/I', 'generated\\all'])
    os.makedirs(os.path.join(ROOT, 'build', 'host'), exist_ok=True)
    # Fixed base, out of the way of the game image's preferred 0x10000000 (it may still be relocated).
    run(['link', '/nologo', '/DLL', '/DEF:host\\ffximain.def', '/OUT:build\\host\\FFXiMain.dll', '/MACHINE:X86',
         '/BASE:0x60000000', '/DYNAMICBASE:NO', 'ole32.lib', 'user32.lib', 'synchronization.lib'] + objs, env)
    boot = compile_stale(env, ['tests\\boot.c'], 'build\\boot', [])
    run(['link', '/nologo', '/OUT:build\\boot.exe', '/MACHINE:X86', 'ole32.lib', 'uuid.lib'] + boot, env)
    run(['build\\boot.exe', 'build\\host\\FFXiMain.dll', RETAIL], env)
    run(['build\\boot.exe', 'build\\host\\FFXiMain.dll', RETAIL, 'relocate'], env)  # as inside pol.exe


def boot64(env):
    recomp('generated/all', ['--all'])
    gen = ['generated\\all\\' + f for f in sorted(os.listdir(os.path.join(ROOT, 'generated', 'all'))) if f.endswith('.c')]
    objs = compile_stale(env, gen + PORTABLE + ['tests\\boot64.c'], 'build\\all64', ['/I', 'generated\\all'], CFLAGS64)
    run(['link', '/nologo', '/OUT:build\\boot64.exe', '/MACHINE:X64', 'synchronization.lib'] + objs, env)
    # the PlayOnline keys, as a Mac player would bring them (build/ is gitignored)
    reg = os.path.join(ROOT, 'build', 'playonline.reg')
    subprocess.call(['reg', 'export', r'HKLM\SOFTWARE\WOW6432Node\PlayOnlineUS', reg, '/y'], stdout=subprocess.DEVNULL)
    game = r'C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI'
    run(['build\\boot64.exe', RETAIL, game] + ([reg] if os.path.exists(reg) else []), env)


FFXI_META = os.path.join(RE_DIR, 'FFXi.2026-08-22.meta.json')
FFXI_IMAGE = os.path.normpath(os.path.join(RE_DIR, '..', 'unpacked', 'FFXi.unpacked.dll'))
GAME = r'C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI'
SDL3 = r'C:\Dev\SDL3\SDL3-3.4.16'


def host64(env):
    """The 64-bit game host: FFXiMain and FFXi.dll translated, the portable runtime, SDL3."""
    recomp('generated/all', ['--all'])
    run([sys.executable, 'recomp/recomp.py', '--meta', FFXI_META, '--image', FFXI_IMAGE, '--retail',
         os.path.join(GAME, 'FFXi.dll'), '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
    gen = ['generated\\all\\' + f for f in sorted(os.listdir(os.path.join(ROOT, 'generated', 'all'))) if f.endswith('.c')]
    gen_ffxi = ['generated\\ffxi\\' + f for f in sorted(os.listdir(os.path.join(ROOT, 'generated', 'ffxi'))) if f.endswith('.c')]
    objs = compile_stale(env, gen, 'build\\all64', ['/I', 'generated\\all'], CFLAGS64)
    objs += compile_stale(env, gen_ffxi, 'build\\ffxi64', ['/I', 'generated\\ffxi'], CFLAGS64)
    objs += compile_stale(env, PORTABLE + ['runtime\\portable\\user32.c', 'runtime\\portable\\d3d8.c', 'runtime\\portable\\dsound.c', 'runtime\\portable\\input.c', 'runtime\\portable\\dinput.c', 'runtime\\portable\\ws2.c', 'host\\host64.c'], 'build\\host64', ['/I', os.path.join(SDL3, 'include')], CFLAGS64)
    run(['link', '/nologo', '/OUT:build\\host64.exe', '/MACHINE:X64', 'synchronization.lib', 'ws2_32.lib',
         os.path.join(SDL3, 'lib', 'x64', 'SDL3.lib')] + objs, env)
    shutil.copy(os.path.join(SDL3, 'lib', 'x64', 'SDL3.dll'), os.path.join(ROOT, 'build'))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else 'difftest'
    env = msvc_env('x64' if what in ('boot64', 'host64') else 'x86')
    {'difftest': difftest, 'host': host, 'boot64': boot64, 'host64': host64}[what](env)


if __name__ == '__main__':
    main()
