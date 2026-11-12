#!/usr/bin/env python3
"""codegen-digests.py: the AIR codegen id of the bridge build in build/, file by file.

Prints one line per file of scripts/build-bridge.sh's CODEGEN_OBJS, in its order: the normalized
digest scripts/air-codegen-id.py gives the file (the same function, imported from that script),
then the path. Two builds whose ids differ differ in the files whose lines differ.

The list here repeats CODEGEN_OBJS. To prove it is the same list, the script computes the id
from its own lines and compares it with build/bridge/air-codegen-id.txt; it exits 1 when they
differ, which means CODEGEN_OBJS changed and this list has to follow.

Run from the repository root after scripts/build-bridge.sh:  python3 -I .github/codegen-digests.py
"""
import glob
import hashlib
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def codegen_objs():
    obj = os.path.join(ROOT, 'build', 'bridge', 'obj')
    airlib = os.path.join(ROOT, 'build', 'airlib', 'obj')
    llvm = os.path.join(ROOT, 'toolchains', 'llvm-x86_64', 'lib')
    return ([os.path.join(obj, n) for n in ('airlib_unix_renamed.o', 'air_cache.c.o', 'cache.c.o')]
            + sorted(glob.glob(os.path.join(airlib, 'third_party_dxbc-spirv_ir_*.o')))
            + sorted(glob.glob(os.path.join(airlib, 'third_party_dxbc-spirv_util_*.o')))
            + [os.path.join(airlib, n) for n in ('air_air_builder.cpp.o', 'air_air_metallib.cpp.o')]
            + sorted(glob.glob(os.path.join(llvm, 'libLLVM*.a'))))


def main():
    spec = importlib.util.spec_from_file_location(
        'air_codegen_id', os.path.join(ROOT, 'scripts', 'air-codegen-id.py'))
    acid = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(acid)

    with open(os.path.join(ROOT, 'build', 'bridge', 'air-codegen-id.txt')) as f:
        m = re.match(r'AIR codegen ([0-9a-f]{64}) ', f.read())
    if not m:
        sys.exit('codegen-digests: no id in build/bridge/air-codegen-id.txt')
    built = m.group(1)

    h = hashlib.sha256()
    acid.field(h, 'D9MT-AIR-CODEGEN v1')
    lines = []
    for p in codegen_objs():
        d = acid.file_digest(p)
        acid.field(h, os.path.basename(p), d)
        lines.append('%s  %s' % (d.hex(), os.path.relpath(p, ROOT)))
    print('\n'.join(lines))
    print('%s  AIR codegen id of the %d files above' % (h.hexdigest(), len(lines)))
    if h.hexdigest() != built:
        sys.exit('codegen-digests: these files give %s, the build says %s: CODEGEN_OBJS in '
                 'scripts/build-bridge.sh changed, and the list in this script has to follow'
                 % (h.hexdigest(), built))


if __name__ == '__main__':
    main()
