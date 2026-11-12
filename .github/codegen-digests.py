#!/usr/bin/env python3
"""codegen-digests.py: the AIR codegen id of the bridge build in build/, file by file.

    python3 -I .github/codegen-digests.py <the files of CODEGEN_OBJS, in the build's order>

Prints one line per file: the normalized digest scripts/air-codegen-id.py gives it (the same
function, imported from that script), then the path. Two builds whose ids differ differ in the
files whose lines differ, or in the order of the lines: the id is a hash over the list in the
order scripts/build-bridge.sh puts it, and that order comes from the shell's globs, which sort by
the locale's collation.

Then two ids:
- of the files in the order given. It must be the id in build/bridge/air-codegen-id.txt; if it
  isn't, CODEGEN_OBJS changed and the caller's list has to follow (exit 1);
- of the same files with each of CODEGEN_OBJS's globs sorted by byte value, which is what the
  build computes under LC_ALL=C.
"""
import glob
import hashlib
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def byte_order():
    obj = os.path.join(ROOT, 'build', 'bridge', 'obj')
    airlib = os.path.join(ROOT, 'build', 'airlib', 'obj')
    llvm = os.path.join(ROOT, 'toolchains', 'llvm-x86_64', 'lib')
    return ([os.path.join(obj, n) for n in ('airlib_unix_renamed.o', 'air_cache.c.o', 'cache.c.o')]
            + sorted(glob.glob(os.path.join(airlib, 'third_party_dxbc-spirv_ir_*.o')))
            + sorted(glob.glob(os.path.join(airlib, 'third_party_dxbc-spirv_util_*.o')))
            + [os.path.join(airlib, n) for n in ('air_air_builder.cpp.o', 'air_air_metallib.cpp.o')]
            + sorted(glob.glob(os.path.join(llvm, 'libLLVM*.a'))))


def main(argv):
    if not argv:
        sys.exit('usage: codegen-digests.py <object or archive>...')
    spec = importlib.util.spec_from_file_location(
        'air_codegen_id', os.path.join(ROOT, 'scripts', 'air-codegen-id.py'))
    acid = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(acid)

    with open(os.path.join(ROOT, 'build', 'bridge', 'air-codegen-id.txt')) as f:
        m = re.match(r'AIR codegen ([0-9a-f]{64}) ', f.read())
    if not m:
        sys.exit('codegen-digests: no id in build/bridge/air-codegen-id.txt')
    built = m.group(1)

    given = [os.path.abspath(p) for p in argv]
    digest = {p: acid.file_digest(p) for p in given}

    def codegen_id(paths):
        h = hashlib.sha256()
        acid.field(h, 'D9MT-AIR-CODEGEN v1')
        for p in paths:
            acid.field(h, os.path.basename(p), digest[p])
        return h.hexdigest()

    for p in given:
        print('%s  %s' % (digest[p].hex(), os.path.relpath(p, ROOT)))
    as_given = codegen_id(given)
    print('%s  AIR codegen id of the %d files in this order' % (as_given, len(given)))
    if as_given != built:
        sys.exit('codegen-digests: these files give %s, the build says %s: CODEGEN_OBJS in '
                 'scripts/build-bridge.sh changed, and the list in the workflow has to follow'
                 % (as_given, built))
    sorted_list = byte_order()
    if sorted(sorted_list) != sorted(given):
        sys.exit('codegen-digests: the files given are not the files of CODEGEN_OBJS that '
                 'byte_order() lists: the list in this script has to follow')
    in_bytes = codegen_id(sorted_list)
    moved = [os.path.basename(a) for a, b in zip(given, sorted_list) if a != b]
    print('%s  AIR codegen id of the same files with the globs in byte order (LC_ALL=C)%s'
          % (in_bytes, ': the same order' if not moved else
             ': %d files sit elsewhere, the first is %s' % (len(moved), moved[0])))


if __name__ == '__main__':
    main(sys.argv[1:])
