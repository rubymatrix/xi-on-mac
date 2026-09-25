"""Produce generated/FFXiMain.unpacked.dll from the player's retail install.

Finds FFXiMain.dll through the PlayOnline registry key (or --dll), checks its SHA-256 against the
metadata's pinned build, and unpacks POL1 statically with tools/pol1_unpack.py - FFXiMain.dll, and FFXi.dll
from the same folder. Output lands in generated/, which is gitignored: it is Square Enix code
and never committed.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
META = os.path.join(ROOT, 'meta', 'FFXiMain.2026-08-22.meta.json')


def retail_dll():
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\PlayOnlineUS\InstallFolder', 0, winreg.KEY_READ | view)
            path, _ = winreg.QueryValueEx(k, '0001')
            return os.path.join(path, 'FFXiMain.dll')
        except OSError:
            continue
    raise SystemExit('PlayOnline install not found in the registry; pass --dll')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll')
    ap.add_argument('--meta', default=META)
    ap.add_argument('--out', default=os.path.join(ROOT, 'generated'))
    args = ap.parse_args()

    dll = args.dll or retail_dll()
    meta = json.load(open(args.meta))
    digest = hashlib.sha256(open(dll, 'rb').read()).hexdigest()
    if digest != meta['sha256']:
        raise SystemExit('%s is build %s, metadata is for %s (%s)' % (dll, digest, meta['build'], meta['sha256']))
    os.makedirs(args.out, exist_ok=True)
    # Keep the verified retail file too: the build and the loader use this copy, so nothing
    # depends on the state of the install afterwards.
    with open(dll, 'rb') as src, open(os.path.join(args.out, 'FFXiMain.retail.dll'), 'wb') as dst:
        dst.write(src.read())
    out = os.path.join(args.out, 'FFXiMain.unpacked.dll')
    unpack = os.path.join(HERE, 'pol1_unpack.py')
    subprocess.check_call([sys.executable, unpack, dll, out])
    ffxi = os.path.join(os.path.dirname(dll), 'FFXi.dll')
    if os.path.exists(ffxi):
        subprocess.check_call([sys.executable, unpack, ffxi, os.path.join(args.out, 'FFXi.unpacked.dll')])
    print('ok: %s (build %s)' % (out, meta['build']))


if __name__ == '__main__':
    main()
