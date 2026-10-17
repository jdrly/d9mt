#!/usr/bin/env python3
"""Compare two clang -fdump-record-layouts-complete dumps (two targets) for
the records declared in the given header directories: field offsets, sizes,
alignment. Used by scripts/test-arm64-port.sh for the PE<->unix bridge ABI.
Usage: layout_cmp.py <header-dir>... -- a.raw b.raw"""
import re, sys, pathlib
args = sys.argv[1:]
sep = args.index('--')
hdrs, (a, b) = args[:sep], args[sep+1:sep+3]
names = set()
for h in hdrs:
    for p in pathlib.Path(h).glob('*.h'):
        t = p.read_text(errors='replace')
        names |= set(re.findall(r'\b(?:struct|union)\s+([A-Za-z_]\w*)', t))
        names |= set(re.findall(r'}\s*([A-Za-z_]\w*)\s*;', t))
def parse(fn):
    recs = {}
    for blk in open(fn).read().split('*** Dumping AST Record Layout')[1:]:
        lines = [l.rstrip() for l in blk.strip('\n').splitlines() if l.strip()]
        m = re.match(r'\s*0\s*\|\s*(?:struct|union|class)\s+(\S+)', lines[0])
        if not m: continue
        n = m.group(1)
        recs[n] = '\n'.join(lines)
    return recs
ra, rb = parse(a), parse(b)
keys = sorted(k for k in (set(ra) | set(rb)) if k in names)
diff = 0; same = 0
for k in keys:
    if k not in ra or k not in rb:
        print(f'ONLY-IN-{"A" if k in ra else "B"}: {k}'); diff += 1; continue
    if ra[k] != rb[k]:
        diff += 1
        print(f'=== DIFF {k}\n--- {a}\n{ra[k]}\n--- {b}\n{rb[k]}')
    else:
        same += 1
print(f'{same} identical, {diff} different ({len(keys)} records from our headers)')
sys.exit(1 if diff else 0)
