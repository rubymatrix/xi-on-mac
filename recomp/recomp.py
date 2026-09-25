"""FFXiMain recompiler driver.

  python recomp/recomp.py --meta <meta.json> --image <unpacked.dll> --out generated/ \
      [--functions 0x10317480,0x10312980,...] [--all] [--stats]

--functions translates those entries plus everything they reach by direct call or tail jump
(the closure), which is what a differential test needs. --all translates every function in the
metadata. Output: funcs.h (prototypes), funcs_NNN.c (translations), table.c (address -> function
table for indirect calls), and with --stats a coverage report of unimplemented instructions.
"""
import argparse
import collections
import json
import os
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from x86c import FunctionTranslator  # noqa: E402


class Program:
    def __init__(self, meta, image_path):
        self.meta = meta
        self.prefix = 'f_'  # translated function names: f_XXXXXXXX (FFXiMain), <module>_XXXXXXXX otherwise
        pe = pefile.PE(image_path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        if self.base != meta['image_base']:
            raise SystemExit('image base %#x does not match metadata %#x' % (self.base, meta['image_base']))
        self.image = pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.functions = {f['entry']: f['ranges'] for f in meta['functions']}
        self.entries = set(self.functions)
        self.switches = {s['at']: s['targets'] for s in meta['switches'] if s['targets']}
        self.referenced = set()
        self.relocs = self.text_relocations(pe)

    def text_relocations(self, pe):
        """Every location in .text that holds an absolute image address.

        They are not in the PE relocation directory (which covers .rdata/.data only): the POL1
        stub applies them itself from a private table at the start of .reloc, in the ordinary
        base-relocation block format, ending at the first block past .text or of size 0.
        """
        import struct
        reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
        text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
        text_end = text.VirtualAddress + text.Misc_VirtualSize
        off = reloc.VirtualAddress
        out = set()
        while True:
            page, size = struct.unpack_from('<II', self.image, off)
            if size == 0 or page >= text_end:
                break
            for k in range((size - 8) // 2):
                e = struct.unpack_from('<H', self.image, off + 8 + 2 * k)[0]
                if e >> 12 == 3:  # IMAGE_REL_BASED_HIGHLOW
                    out.add(self.base + page + (e & 0xFFF))
            off += size
        return out

    def read(self, va, n):
        off = va - self.base
        return self.image[off:off + n]


def write_if_changed(path, text):
    """Leave unchanged files alone so an incremental C build only recompiles what moved."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, 'w') as f:
        f.write(text)


def patch_kinds(prog, entries):
    """0 if a 5-byte jmp fits at the entry without touching another entry or live bytes, else 1."""
    all_entries = sorted(prog.entries)
    nxt = {a: b for a, b in zip(all_entries, all_entries[1:])}
    kinds = []
    for e in entries:
        ok = nxt.get(e, e + 5) - e >= 5
        if ok:
            end = max(hi for lo, hi in prog.functions[e] if lo <= e < hi) if any(lo <= e < hi for lo, hi in prog.functions[e]) else e
            if end < e + 5:
                tail = prog.read(end, e + 5 - end)
                ok = all(b in (0xCC, 0x90) for b in tail)
        kinds.append(0 if ok else 1)
    return kinds


def retail_dll():
    import winreg
    for view in (winreg.KEY_WOW64_32KEY, winreg.KEY_WOW64_64KEY):
        try:
            k = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'SOFTWARE\PlayOnlineUS\InstallFolder', 0, winreg.KEY_READ | view)
            return os.path.join(winreg.QueryValueEx(k, '0001')[0], 'FFXiMain.dll')
        except OSError:
            continue
    raise SystemExit('PlayOnline install not found in the registry; pass --retail')


def image_constants(prog, image_path):
    """What the loader needs to rebuild .text from the retail DLL and check it is the right build."""
    sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')))
    import pol1_unpack  # the static POL1 unpacker (tools/pol1_unpack.py)
    pe = pefile.PE(image_path, fast_load=True)
    text = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text')
    pol1 = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'POL1')
    src_len, dst_len, oep = pol1_unpack.parse_stub(pe)
    reloc = next(s for s in pe.sections if s.Name.rstrip(b'\0') == b'.reloc')
    return {
        'base': prog.base,
        'reloc_rva': reloc.VirtualAddress,
        'timestamp': pe.FILE_HEADER.TimeDateStamp,
        'size': pe.OPTIONAL_HEADER.SizeOfImage,
        'text_rva': text.VirtualAddress,
        'text_size': dst_len,
        'pol1_rva': pol1.VirtualAddress,
        'pol1_src_len': src_len,
        'oep': prog.base + oep,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--meta', required=True)
    ap.add_argument('--image', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--functions', default='')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--chunk', type=int, default=400)
    ap.add_argument('--retail', help='the retail (POL1-packed) DLL; default: FFXiMain.dll from the registry')
    ap.add_argument('--module', default='',
                    help='a second module (e.g. ffxi for FFXi.dll): functions are named <module>_XXXXXXXX, the '
                         'module has its own relocation delta, and table.c defines RtModule rt_module_<module> '
                         'instead of the rt_table/rt_image_* globals FFXiMain uses')
    args = ap.parse_args()

    meta = json.load(open(args.meta))
    prog = Program(meta, args.image)
    prog.prefix = args.module + '_' if args.module else 'f_'

    if args.all:
        todo = sorted(prog.entries)
        closure = False
    else:
        todo = [int(x, 16) for x in args.functions.split(',') if x]
        closure = True
        for e in todo:
            if e not in prog.entries:
                raise SystemExit('%#x is not a function entry in the metadata' % e)

    done = {}
    unimpl = collections.Counter()
    unimpl_funcs = set()
    queue = list(todo)
    while queue:
        e = queue.pop()
        if e in done:
            continue
        prog.referenced = set()
        t = FunctionTranslator(prog, e, prog.functions[e])
        done[e] = t.translate()
        for addr, mn, why in t.unimpl:
            unimpl[why if why.startswith('x87') else mn] += 1
            unimpl_funcs.add(e)
        if closure:
            queue.extend(r for r in prog.referenced if r not in done)

    os.makedirs(args.out, exist_ok=True)
    entries = sorted(done)
    # A second module relocates independently of FFXiMain: its translation reads its own delta.
    delta = ('\n#undef RD\n#define RD rt_delta_%s\nextern uint32_t rt_delta_%s;\n' % (args.module, args.module)
             if args.module else '')
    with open(os.path.join(args.out, 'funcs.h'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#pragma once\n#include "guest.h"\n%s\n' % delta)
        for e in entries:
            f.write('void %s%08x(Guest* g);\n' % (prog.prefix, e))
    chunks = set()
    for k in range(0, len(entries), args.chunk):
        name = 'funcs_%03d.c' % (k // args.chunk)
        chunks.add(name)
        text = ('/* generated by recomp.py - do not edit, do not commit */\n#include "funcs.h"\n\n'
                '#if defined(_MSC_VER)\n#pragma warning(disable: 4102 4189 4101 4702)\n'
                '#pragma code_seg(".xlat") /* translated code in its own section: the profiler tells it from the runtime */\n'
                '#endif\n\n')
        text += ''.join('\n'.join(done[e]) + '\n\n' for e in entries[k:k + args.chunk])
        write_if_changed(os.path.join(args.out, name), text)
    for old in os.listdir(args.out):
        if old.startswith('funcs_') and old.endswith('.c') and old not in chunks:
            os.remove(os.path.join(args.out, old))
    kinds = patch_kinds(prog, entries)
    img = image_constants(prog, args.retail or retail_dll())
    with open(os.path.join(args.out, 'table.c'), 'w') as f:
        f.write('/* generated by recomp.py - do not edit, do not commit */\n#include "runtime.h"\n#include "funcs.h"\n\n')
        if args.module:
            # A second module: one descriptor the runtime registers (rt_add_module), and its delta.
            m = args.module
            f.write('uint32_t rt_delta_%s;\n\nstatic const RtEntry table[] = {\n' % m)
            for e in entries:
                f.write('    { 0x%08Xu, %s%08x },\n' % (e, prog.prefix, e))
            f.write('};\n\n/* The pinned retail build this translation belongs to. */\n')
            f.write('const RtModule rt_module_%s = {\n    "%s", table, %d, &rt_delta_%s,\n' % (m, m, len(entries), m))
            f.write('    ' + ', '.join('0x%Xu' % img[k] for k in ('base', 'timestamp', 'size', 'text_rva', 'text_size',
                                                                     'pol1_rva', 'pol1_src_len', 'oep', 'reloc_rva')))
            f.write(',\n};\n')
            print('module %s: translated %d functions (%d with unimplemented instructions)' % (m, len(entries), len(unimpl_funcs)))
            if args.stats or unimpl:
                for k, v in unimpl.most_common(40):
                    print('  %7d  %s' % (v, k))
            return
        f.write('const RtEntry rt_table[] = {\n')
        for e in entries:
            f.write('    { 0x%08Xu, f_%08x },\n' % (e, e))
        f.write('};\nconst unsigned rt_table_count = %d;\n\n' % len(entries))
        f.write('/* How each entry is redirected into its translation: 0 = 5-byte jmp, 1 = int3 (too close\n'
                ' * to the next entry, or a body shorter than 5 bytes with no padding after it). */\n')
        f.write('const unsigned char rt_table_patch[] = {\n')
        for k in range(0, len(kinds), 32):
            f.write('    ' + ','.join(str(x) for x in kinds[k:k + 32]) + ',\n')
        f.write('};\n\n')
        f.write('/* The pinned retail build this translation belongs to. */\n')
        for name, value in img.items():
            f.write('const uint32_t rt_image_%s = 0x%Xu;\n' % (name, value))
    print('entry patches: %d jmp, %d int3' % (kinds.count(0), kinds.count(1)))

    print('translated %d functions (%d with unimplemented instructions)' % (len(entries), len(unimpl_funcs)))
    if args.stats or unimpl:
        total = sum(unimpl.values())
        print('unimplemented instructions: %d' % total)
        for k, v in unimpl.most_common(40):
            print('  %7d  %s' % (v, k))


if __name__ == '__main__':
    main()
