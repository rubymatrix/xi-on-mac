"""Build driver for POSIX hosts (clang): arm64 macOS for R3, Linux as a by-product.

  python3 tools/build_posix.py prepare --game <FINAL FANTASY XI folder>
        identify the player's build (meta/builds.json) and unpack FFXiMain.dll and FFXi.dll into
        generated/ (Square Enix code: generated/ is gitignored, never committed)
  python3 tools/build_posix.py boot64 --game <folder>    the boot test (R3.1)
  python3 tools/build_posix.py host64 --game <folder>    the game host, with SDL3
  python3 tools/build_posix.py gfxtest                   the graphics back end and the D3D8 front end,
        offscreen, without the game (tests/gfx_test.c, tests/d3d8_test.c)
  python3 tools/build_posix.py datuitest --game <folder>  the game's UI art read from its DATs (host/datui.c):
        parse checks, and renders in build/datui/ (tests/datui_test.c)
  python3 tools/build_posix.py app --game <folder> [--sign-in pol|lsb] [--server name] [--resolution WxH]
        [--menu-resolution WxH] [--window-mode 0-3] [--background picture]
        build/Final Fantasy XI.app: host64 with its libraries, playonline.reg and the defaults above in
        its Info.plist (host/appdefaults.h), so it starts from Finder with no command line. The values
        go into the built app only: nothing names a server in the source.
  python3 tools/build_posix.py launcher                  the launcher (launcher/, Tauri): the PlayOnline
        tests, build/pol-signin, and on macOS build/FFXI Launcher.app with build/host64 inside it

The same sources as tools/build.py's boot64/host64 targets, with plat_posix.c for plat_win.c.
Needs: clang (Xcode command line tools), python3 with capstone and pefile, and for host64 SDL3
(`brew install sdl3`, found through pkg-config) and mbedtls (`brew install mbedtls`). The game folder is the retail "FINAL FANTASY XI"
folder copied from a Windows install, with "PlayOnlineViewer" next to it.
"""
import argparse
import concurrent.futures
import os
import shlex
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402  (constants and source lists; nothing Windows-only runs on import)

ROOT = build.ROOT
GEN_FFXI_IMAGE = build.FFXI_IMAGE
CFLAGS = ['-O2', '-std=c11', '-g', '-DRT_GUEST_WINDOW', '-fno-strict-aliasing', '-I', 'runtime', '-I', 'runtime/portable',
          '-I', 'generated', '-I', 'launcher/pol', '-I', 'third_party/stb']
# the generated C: every label and local is emitted whether used or not
GEN_WARNINGS = ['-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Wno-unused-function',
                '-Wno-parentheses-equality', '-Wno-unreachable-code']
# the graphics back end: Metal on macOS (R3.2), none elsewhere yet
if sys.platform == 'darwin':
    GFX_SOURCES = ['runtime/portable/gfx_msl.c', 'runtime/portable/gfx_msl_shaders.c', 'runtime/portable/gfx_metal.m']
    GFX_LIBS = ['-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'Foundation',
                '-framework', 'Security']  # Security: the sign-in screen's saved passwords
else:
    GFX_SOURCES = ['runtime/portable/gfx_null.c']
    GFX_LIBS = []
HOST_SOURCES = ['runtime/portable/user32.c', 'runtime/portable/d3d8.c', 'runtime/portable/dsound.c',
                'runtime/portable/input.c', 'runtime/portable/dinput.c', 'runtime/portable/ws2.c', 'host/host64.c',
                'host/lsb_login.c', 'host/datui.c', 'host/uidraw.c', 'host/signin.c', 'host/ui_art.c', 'host/keychain.c', 'host/appdefaults.c', 'launcher/pol/polcrypt.c',
                'launcher/pol/polnet.c', 'launcher/pol/polsession.c'] + GFX_SOURCES


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


def stale(s, obj, headers):
    """Whether obj must be rebuilt: missing, older than its source, older than the newest runtime
    header (our own sources), or older than build.h when the source includes it (another build's
    addresses)."""
    src = os.path.join(ROOT, s)
    if not os.path.exists(obj):
        return True
    stamp = os.path.getmtime(src)
    if not s.startswith('generated/'):
        stamp = max(stamp, headers)
    if os.path.getmtime(obj) < stamp:
        return True
    if not s.startswith('generated/') and os.path.getmtime(obj) < os.path.getmtime(build.BUILD_H):
        with open(src, errors='replace') as f:
            return '"build.h"' in f.read()
    return False


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
        if stale(s, full_obj, headers):
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
    run([sys.executable, 'tools/prepare.py', '--game', game])  # and FFXi.dll


def translate():
    if build.BUILD is None:
        raise SystemExit('run: python3 tools/build_posix.py prepare --game <folder>')
    build.write_build_h()
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
         build.FFXI_RETAIL, '--module', 'ffxi', '--out', 'generated/ffxi', '--all'])
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


def datuitest(game):
    """host/datui.c against the install's DATs; renders the windows and lobby into build/datui/."""
    os.makedirs(os.path.join(ROOT, 'build', 'datui'), exist_ok=True)
    run(['clang', '-O2', '-std=c11', '-Wall', '-I', 'host', '-o', 'build/datui_test', 'tests/datui_test.c',
         'host/datui.c', '-lm'])
    run(['build/datui_test', '--game', game, '--out', 'build/datui'])


APP_NAME = 'Final Fantasy XI'


def plist_escape(v):
    return str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def bundle_dylibs(exe, frameworks):
    """Copies the non-system libraries exe links (and theirs) into frameworks and points exe at
    them through @rpath, so the app runs without Homebrew."""
    os.makedirs(frameworks, exist_ok=True)
    def deps(path):
        out = subprocess.check_output(['otool', '-L', path], text=True).splitlines()[1:]
        return [l.split()[0] for l in out if l.strip()]
    todo, seen = [exe], set()
    while todo:
        img = todo.pop()
        for d in deps(img):
            name = os.path.basename(d)
            if not (d.startswith('/opt/') or d.startswith('/usr/local/')):
                continue
            dst = os.path.join(frameworks, name)
            if name not in seen:
                seen.add(name)
                shutil.copy(d, dst)
                os.chmod(dst, 0o755)
                subprocess.check_call(['install_name_tool', '-id', '@rpath/' + name, dst])
                todo.append(dst)
            if img != dst:
                subprocess.check_call(['install_name_tool', '-change', d, '@rpath/' + name, img])
    subprocess.check_call(['install_name_tool', '-add_rpath', '@executable_path/../Frameworks', exe])
    return sorted(seen)


def app(game, a):
    """host64 as build/Final Fantasy XI.app, with the first-run defaults in its Info.plist."""
    host64(game)
    bundle = os.path.join(ROOT, 'build', APP_NAME + '.app')
    shutil.rmtree(bundle, ignore_errors=True)
    contents = os.path.join(bundle, 'Contents')
    macos, res = os.path.join(contents, 'MacOS'), os.path.join(contents, 'Resources')
    os.makedirs(macos)
    os.makedirs(res)
    exe = os.path.join(macos, APP_NAME)
    shutil.copy(os.path.join(ROOT, 'build', 'host64'), exe)
    shutil.copy(os.path.join(ROOT, 'playonline.reg'), res)
    shutil.copy(os.path.join(ROOT, 'launcher', 'src-tauri', 'icons', 'icon.icns'), res)
    keys = {'FFXIGameFolder': game}
    if a.sign_in:
        keys['FFXISignInMethod'] = a.sign_in
    if a.server:
        keys['FFXIServer'] = a.server
    if a.resolution:
        keys['FFXIResolution'] = a.resolution
    if a.menu_resolution:
        keys['FFXIMenuResolution'] = a.menu_resolution
    if a.window_mode is not None:
        keys['FFXIWindowMode'] = a.window_mode
    if a.background:
        # the sign-in screen reads PNG, JPEG and BMP; anything else (WebP) becomes a PNG
        src, ext = os.path.expanduser(a.background), os.path.splitext(a.background)[1].lower()
        name = 'background' + (ext if ext in ('.png', '.jpg', '.jpeg', '.bmp') else '.png')
        if name.endswith('.png') and ext != '.png':
            run(['sips', '-s', 'format', 'png', src, '--out', os.path.join(res, name)])
        else:
            shutil.copy(src, os.path.join(res, name))
        keys['FFXIBackground'] = name
    extra = ''.join('\t<key>%s</key>%s\n' % (k, '<integer>%d</integer>' % v if isinstance(v, int)
                                              else '<string>%s</string>' % plist_escape(v)) for k, v in keys.items())
    with open(os.path.join(contents, 'Info.plist'), 'w') as f:
        f.write(APP_INFO_PLIST.replace('@NAME@', APP_NAME).replace('@EXTRA@', extra))
    libs = bundle_dylibs(exe, os.path.join(contents, 'Frameworks'))
    run(['codesign', '--force', '--deep', '--sign', '-', bundle])
    print('built %s (%s bundled; defaults: %s)' % (bundle, ', '.join(libs) or 'no libraries',
                                                    ', '.join('%s=%s' % kv for kv in keys.items())))


APP_INFO_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>@NAME@</string>
	<key>CFBundleIdentifier</key><string>com.rubymatrix.xi-on-mac</string>
	<key>CFBundleName</key><string>@NAME@</string>
	<key>CFBundleDisplayName</key><string>@NAME@</string>
	<key>CFBundleIconFile</key><string>icon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
	<key>NSHighResolutionCapable</key><true/>
@EXTRA@</dict>
</plist>
'''


GAME_INFO_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>host64</string>
	<key>CFBundleIdentifier</key><string>com.rubymatrix.xi-launcher.game</string>
	<key>CFBundleName</key><string>FINAL FANTASY XI</string>
	<key>CFBundleDisplayName</key><string>FINAL FANTASY XI</string>
	<key>CFBundleIconFile</key><string>icon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
'''
POL_SOURCES = ['launcher/pol/polcrypt.c', 'launcher/pol/polnet.c', 'launcher/pol/polsession.c']


def launcher():
    """The launcher: its PlayOnline C (tested here, and as build/pol-signin), then the Tauri app.
    Needs Rust and the Tauri CLI (`cargo install tauri-cli`). Build host64 first to bundle it."""
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    cc = ['clang', '-O2', '-std=c11', '-Wall', '-I', 'launcher/pol']
    run(cc + ['-o', 'build/polcrypt_test', 'tests/polcrypt_test.c'] + POL_SOURCES)
    run(['build/polcrypt_test'])
    run(cc + ['-o', 'build/pol-signin', 'launcher/pol/pol_signin.c'] + POL_SOURCES)
    env = dict(os.environ, CARGO_TARGET_DIR=os.path.join(ROOT, 'build', 'launcher-target'))
    tauri_dir = os.path.join(ROOT, 'launcher', 'src-tauri')
    release = os.path.join(env['CARGO_TARGET_DIR'], 'release')
    if sys.platform != 'darwin':
        subprocess.check_call(['cargo', 'tauri', 'build', '--no-bundle'], cwd=tauri_dir, env=env)
        shutil.copy(os.path.join(release, 'ffxi-launcher'), os.path.join(ROOT, 'build', 'ffxi-launcher'))
        print('built build/ffxi-launcher')
        return
    subprocess.check_call(['cargo', 'tauri', 'build', '--bundles', 'app'], cwd=tauri_dir, env=env)
    app = os.path.join(ROOT, 'build', 'FFXI Launcher.app')
    shutil.rmtree(app, ignore_errors=True)
    shutil.copytree(os.path.join(release, 'bundle', 'macos', 'FFXI Launcher.app'), app, symlinks=True)
    # host64 goes in as an app of its own (Contents/Helpers/FINAL FANTASY XI.app), so the game has its
    # own name and icon in the Dock; the launcher leaves the Dock while it runs
    host = os.path.join(ROOT, 'build', 'host64')
    if os.path.exists(host):
        game_app = os.path.join(app, 'Contents', 'Helpers', 'FINAL FANTASY XI.app', 'Contents')
        os.makedirs(os.path.join(game_app, 'MacOS'), exist_ok=True)
        os.makedirs(os.path.join(game_app, 'Resources'), exist_ok=True)
        shutil.copy(host, os.path.join(game_app, 'MacOS', 'host64'))
        shutil.copy(os.path.join(ROOT, 'launcher', 'src-tauri', 'icons', 'icon.icns'), os.path.join(game_app, 'Resources'))
        with open(os.path.join(game_app, 'Info.plist'), 'w') as f:
            f.write(GAME_INFO_PLIST)
    else:
        print('note: no build/host64 to bundle; the launcher will ask where it is')
    run(['codesign', '--force', '--deep', '--sign', '-', app])
    print('built %s' % app)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('target', choices=['prepare', 'boot64', 'host64', 'gfxtest', 'datuitest', 'app', 'launcher'])
    ap.add_argument('--game', default=os.path.expanduser('~/PlayOnline/SquareEnix/FINAL FANTASY XI'))
    # app: its first-run defaults (host/appdefaults.h)
    ap.add_argument('--sign-in', choices=['pol', 'lsb'])
    ap.add_argument('--server')
    ap.add_argument('--resolution')
    ap.add_argument('--menu-resolution')
    ap.add_argument('--window-mode', type=int, choices=[0, 1, 2, 3])
    ap.add_argument('--background')
    args = ap.parse_args()
    if args.target == 'gfxtest':
        return gfxtest()
    if args.target == 'launcher':
        return launcher()
    game = os.path.abspath(args.game)
    if not os.path.exists(os.path.join(game, 'FFXiMain.dll')):
        raise SystemExit('no FFXiMain.dll in %s (--game)' % game)
    if args.target == 'datuitest':
        return datuitest(game)
    if args.target == 'app':
        return app(game, args)
    if args.target == 'prepare':
        prepare(game)
    elif args.target == 'boot64':
        boot64(game)
    else:
        host64(game)


if __name__ == '__main__':
    main()
