#!/usr/bin/env python3
"""air-target-tables.py <work dir> <codegen id>: test-air-cache-lineage.sh's AIR target check.

Reads <work>/t{26,27,L26,L27}.log (one corpus run per target, each on its own cache) and prints
"1 <tables>" when the four targets log the terms T 0x2828, 0x2929, 0x12727 and 0x12729, and four
pairwise distinct tables, each (without a lineage) cache_<identity's first 8 bytes, little-endian
^ (ABI 2 << 56) ^ (T << 24)>, else "0 <what differs>".
"""
import re
import sys

WANT = {'26': 0x2828, '27': 0x2929, 'L26': 0x12727, 'L27': 0x12729}
ABI = 2


def main(work, codegen_id):
    identity = int.from_bytes(bytes.fromhex(codegen_id[:16]), 'little')
    errs, tables = [], {}
    for mode, t in WANT.items():
        log = open('%s/t%s.log' % (work, mode)).read()
        m = re.search(r'AIR cache .* \(table cache_([0-9]+), target (0x[0-9a-f]+),', log)
        if not m:
            errs.append('%s: no AIR cache line' % mode)
            continue
        table, got = int(m.group(1)), int(m.group(2), 16)
        if got != t:
            errs.append('%s: T %#x, not %#x' % (mode, got, t))
        if ', lineage ' not in log and table != (identity ^ (ABI << 56) ^ (t << 24)) & 0x7fffffffffffffff:
            errs.append('%s: table cache_%d is not identity ^ ABI ^ T' % (mode, table))
        tables[mode] = table
    if len(set(WANT.values())) != len(WANT) or len(set(tables.values())) != len(WANT):
        errs.append('not four distinct tables: %s' % tables)
    print(0 if errs else 1, '; '.join(errs) or ' '.join('%s=cache_%d' % kv for kv in tables.items()))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
