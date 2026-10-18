#!/usr/bin/env python3
"""forge-statecache.py - test tool: write state cache files with one forged entry.

Takes a .d9mtstate file recorded by d9mt and writes one copy per invalid case
(docs/state-cache.md §3), with entry <index> changed and every checksum
recomputed, so only d9mt's content validation (backend/mtl_pso_key.h) stands
between the entry and Metal. Used with scripts/run-statecache-bench.sh under
exc_guard: each forged file must load with invalid=1 and no crash.

  forge-statecache.py <recorded.d9mtstate> <out dir> [entry index, default 0]

Writes <out dir>/<case>/<same file name>. Reads and writes only those paths.
"""
import os
import struct
import sys

HEADER, ENTRY, KEY_AT = 64, 384, 8 + 56          # key = state bytes, after check + entry head
ATTRS, LAYOUTS, ATTR_COUNT, LAYOUT_COUNT = 0, 160, 240, 241
COLORS, COLOR_COUNT, SAMPLES, TOPOLOGY, A2C, DEPTH = 244, 292, 293, 294, 295, 296


def fnv1a(data, h=0xcbf29ce484222325):
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


def u8(k, at, v):
    k[at] = v


def u16(k, at, v):
    k[at:at + 2] = struct.pack('<H', v)


def u32(k, at, v):
    k[at:at + 4] = struct.pack('<I', v)


CASES = {
    'zero_stride':           lambda k: u16(k, LAYOUTS + 0, 0),
    'bad_vertex_format':     lambda k: u32(k, ATTRS + 0, 1),            # UChar2: never produced
    'bad_pixel_format':      lambda k: u32(k, COLORS + 0, 999),
    'depth_pixel_format':    lambda k: u32(k, COLORS + 0, 252),         # Depth32Float as a color target
    'blend_non_blendable':   lambda k: (u32(k, COLORS + 0, 53), u8(k, COLORS + 4, 1)),  # R32Uint, blending
    'buffer_without_layout': lambda k: u8(k, ATTRS + 6, 7),             # no layout at buffer 7
    'color_count_overflow':  lambda k: u8(k, COLOR_COUNT, 9),           # colors[] has 4
    'attr_count_overflow':   lambda k: u8(k, ATTR_COUNT, 40),           # attrs[] has 20
    'sample_count_3':        lambda k: u8(k, SAMPLES, 3),
    'depth_format':          lambda k: u32(k, DEPTH, 252),
}


def main():
    src, out = sys.argv[1], sys.argv[2]
    index = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    data = bytearray(open(src, 'rb').read())
    count = struct.unpack_from('<I', data, 20)[0]
    if data[:8] != b'D9MTSTC\0' or index >= count or len(data) < HEADER + count * ENTRY:
        sys.exit('not a d9mt state cache file, or no entry %d' % index)

    for name, mutate in CASES.items():
        f = bytearray(data)
        at = HEADER + index * ENTRY
        key = bytearray(f[at + KEY_AT:at + KEY_AT + 304])
        mutate(key)
        f[at + KEY_AT:at + KEY_AT + 304] = key
        struct.pack_into('<Q', f, at, fnv1a(f[at + 8:at + ENTRY]))   # the entry's check
        d = os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, os.path.basename(src)), 'wb') as o:
            o.write(f)
        print(name)


if __name__ == '__main__':
    main()
