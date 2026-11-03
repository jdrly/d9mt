#!/usr/bin/env python3
"""air-fields.py: the AIR target fields of metallibs (air/air_metallib.h AirTarget).

  air-fields.py [--llvm-dis P] FILE...                 one JSON line of fields per FILE
  air-fields.py [--llvm-dis P] --expect MODE FILE...   check every FILE against MODE's fields
  air-fields.py [--llvm-dis P] --want WANT.json FILE...  check every FILE against WANT.json

A FILE is a metallib (d9mt's or Apple's) or LLVM text (.ll). A metallib gives the container fields
"container" (header file version), "os" (header OS version) and "vers" (every function's VERS:
AIR major, minor, Metal major, minor); with --llvm-dis also its bitcode's, disassembled. LLVM text
gives the bitcode fields "triple", "air" (air.version), "metal" (air.language_version) and
"datalayout". A check compares only the keys the wanted fields have ("vers" wants one value for
every function), prints "<file>: <differences>" per mismatch and exits 1 on any.
MODE is 26, 27 (the targets), L26 or L27 (legacy, D9MT_AIR_LEGACY=1).
"""
import json
import re
import struct
import subprocess
import sys
import tempfile

MODES = {
    '26': {'triple': 'air64_v28-apple-macosx26.0.0', 'air': [2, 8], 'metal': [4, 0],
           'container': [2, 9], 'os': [26, 0], 'vers': [2, 8, 4, 0]},
    '27': {'triple': 'air64_v29-apple-macosx27.0.0', 'air': [2, 9], 'metal': [4, 1],
           'container': [2, 9], 'os': [27, 0], 'vers': [2, 9, 4, 1]},
    'L26': {'triple': 'air64-apple-macosx15.0.0', 'air': [2, 7], 'metal': [3, 0],
            'container': [2, 7], 'os': [15, 0], 'vers': [2, 7, 3, 0]},
    'L27': {'triple': 'air64-apple-macosx15.0.0', 'air': [2, 7], 'metal': [3, 0],
            'container': [2, 7], 'os': [15, 0], 'vers': [2, 9, 3, 0]},
}


def metallib_fields(d):
    if d[:4] != b'MTLB':
        raise ValueError('not a metallib')
    header_size, list_size = struct.unpack_from('<QQ', d, 24)
    pos, end, vers = header_size + 4, header_size + list_size, []
    while pos < end:
        p, fn_end = pos + 4, pos + struct.unpack_from('<I', d, pos)[0]
        while p < fn_end and d[p:p + 4] != b'ENDT':
            n = struct.unpack_from('<H', d, p + 4)[0]
            if d[p:p + 4] == b'VERS':
                vers.append(list(struct.unpack_from('<4H', d, p + 6)))
            p += 6 + n
        pos = fn_end
    bc_off, bc_size = struct.unpack_from('<QQ', d, 0x48)
    return {'container': list(struct.unpack_from('<HH', d, 6)),
            'os': list(struct.unpack_from('<HH', d, 12)), 'vers': vers}, d[bc_off:bc_off + bc_size]


def ll_fields(t):
    md = dict(re.findall(r'^(![0-9]+) = (?:distinct )?!\{(.*)\}$', t, re.M))

    def named(n):
        m = re.search(r'^!%s = !\{(![0-9]+)\}$' % re.escape(n), t, re.M)
        return md.get(m.group(1), '') if m else ''

    def ints(s):
        return [int(x) for x in re.findall(r'i32 (-?[0-9]+)', s)]

    trip = re.search(r'^target triple = "([^"]*)"', t, re.M)
    dl = re.search(r'^target datalayout = "([^"]*)"', t, re.M)
    return {'triple': trip.group(1) if trip else None, 'air': ints(named('air.version'))[:2],
            'metal': ints(named('air.language_version'))[:2], 'datalayout': dl.group(1) if dl else None}


def fields(path, llvm_dis):
    data = open(path, 'rb').read()
    if path.endswith('.ll'):
        return ll_fields(data.decode())
    f, bitcode = metallib_fields(data)
    if llvm_dis:
        with tempfile.NamedTemporaryFile(suffix='.bc') as bc:
            bc.write(bitcode)
            bc.flush()
            text = subprocess.run([llvm_dis, bc.name, '-o', '-'], capture_output=True, check=True).stdout
        f.update(ll_fields(text.decode()))
    return f


def differences(got, want):
    errs = []
    for k, v in sorted(want.items()):
        if k == 'vers':
            if not got.get('vers') or any(x != v for x in got['vers']):
                errs.append('VERS %s, not %s' % (got.get('vers'), v))
        elif got.get(k) != v:
            errs.append('%s %s, not %s' % (k, got.get(k), v))
    return errs


def main(argv):
    llvm_dis, want, files = None, None, []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--llvm-dis':
            llvm_dis = argv[i + 1]; i += 2
        elif a == '--expect':
            want = MODES[argv[i + 1]]; i += 2
        elif a == '--want':
            want = json.load(open(argv[i + 1])); i += 2
        else:
            files.append(a); i += 1
    if not files:
        sys.exit(__doc__)
    if want is None:
        for f in files:
            print(json.dumps(fields(f, llvm_dis), sort_keys=True))
        return 0
    bad = 0
    for f in files:
        try:
            errs = differences(fields(f, llvm_dis), want)
        except (OSError, ValueError, struct.error, subprocess.CalledProcessError) as e:
            errs = ['unreadable: %s' % e]
        if errs:
            print('%s: %s' % (f, '; '.join(errs)))
            bad = 1
    return bad


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
