#!/usr/bin/env python3
"""air-codegen-id.py: the AIR codegen id, the identity of the code that produces the AIR cache's
metallibs (ADR-0006). A SHA-256 over the objects and archives linked as the codegen, in the order
given, each by its file name and normalized content:

- Mach-O objects: every section (names, address, size, alignment, flags, bytes, relocations), the
  symbol table (name, type, section, desc, value), the indirect symbol table, the deployment
  target (platform + minos) and every other load command as it is.
- Archives: every member by name and the same digest, without the ar headers (dates, owners) and
  the ranlib table.
- Left out: the SDK version and the build-tool versions in LC_BUILD_VERSION, which change with
  every Xcode update even when not one byte of code does (0.1.5.1, Xcode 27.0, rebuilt with 27.2:
  every section identical, the LC_UUID not).

Anything this script doesn't recognize is hashed as raw bytes, so an unknown format gives a new id
(a cold cache), never an old one.

    air-codegen-id.py <file>...            prints the hex id
    air-codegen-id.py --c <out.c> <file>... also writes the C file that embeds it
"""
import hashlib
import os
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB = 0x19, 0x2, 0xB
LC_VERSION_MIN_MACOSX, LC_BUILD_VERSION = 0x24, 0x32
LC_DATA_IN_CODE, LC_LINKER_OPTIMIZATION_HINT = 0x29, 0x2E
ZEROFILL = (0x1, 0xC, 0x12)  # S_ZEROFILL, S_GB_ZEROFILL, S_THREAD_LOCAL_ZEROFILL
PREFIX = 'D9MT-AIR-CODEGEN-ID '


def field(h, *parts):
    for p in parts:
        if isinstance(p, int):
            p = struct.pack('<q', p) if p < 0 else struct.pack('<Q', p)
        elif isinstance(p, str):
            p = p.encode()
        h.update(struct.pack('<Q', len(p)))
        h.update(p)


def macho_digest(d):
    """Normalized digest of one MH_OBJECT/MH_DYLIB image; None if d isn't a 64-bit Mach-O."""
    if len(d) < 32 or struct.unpack_from('<I', d, 0)[0] != MH_MAGIC_64:
        return None
    h = hashlib.sha256()
    _, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
    field(h, 'macho', cpu & 0xFFFFFFFF, sub & 0xFFFFFFFF, ftype, ncmds, flags)
    off = 32
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, off)
        body = d[off:off + cs]
        if cmd == LC_SEGMENT_64:
            seg = body[8:24].rstrip(b'\0')
            vmaddr, vmsize, _, _, maxp, initp, nsects, sflags = struct.unpack_from('<QQQQiiII', body, 24)
            field(h, 'segment', seg, vmaddr, vmsize, maxp & 0xFFFFFFFF, initp & 0xFFFFFFFF, nsects, sflags)
            for i in range(nsects):
                s = 72 + i * 80
                name, sseg = body[s:s + 16].rstrip(b'\0'), body[s + 16:s + 32].rstrip(b'\0')
                addr, size, foff, align, reloff, nreloc, sfl, r1, r2, _ = struct.unpack_from('<QQIIIIIIII', body, s + 32)
                content = b'' if (sfl & 0xFF) in ZEROFILL else d[foff:foff + size]
                field(h, 'section', sseg, name, addr, size, align, sfl, r1, r2, content, d[reloff:reloff + 8 * nreloc])
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', body, 8)
            field(h, 'symtab', nsyms)
            strtab = d[stroff:stroff + strsize]
            for i in range(nsyms):
                strx, ntype, nsect, ndesc, nvalue = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
                name = strtab[strx:strtab.index(b'\0', strx)] if strx < len(strtab) else b''
                field(h, name, ntype, nsect, ndesc, nvalue)
        elif cmd == LC_DYSYMTAB:
            v = struct.unpack_from('<18I', body, 8)
            # index ranges and counts; file offsets left out, the indirect table by content
            field(h, 'dysymtab', *[v[i] for i in (0, 1, 2, 3, 4, 5, 7, 9, 11, 13, 15, 17)],
                  d[v[12]:v[12] + 4 * v[13]])
        elif cmd == LC_BUILD_VERSION:
            platform, minos = struct.unpack_from('<II', body, 8)
            field(h, 'build', platform, minos)  # not sdk, not the tools
        elif cmd == LC_VERSION_MIN_MACOSX:
            field(h, 'vmin', struct.unpack_from('<I', body, 8)[0])  # not sdk
        elif cmd in (LC_DATA_IN_CODE, LC_LINKER_OPTIMIZATION_HINT):
            doff, dsize = struct.unpack_from('<II', body, 8)
            field(h, 'linkedit', cmd, d[doff:doff + dsize])
        else:
            field(h, 'cmd', cmd, body)
        off += cs
    if off - 32 != sizeofcmds:
        return None
    return h.digest()


def file_digest(path):
    d = open(path, 'rb').read()
    h = hashlib.sha256()
    m = macho_digest(d)
    if m is not None:
        field(h, 'object', m)
        return h.digest()
    if d.startswith(b'!<arch>\n'):
        field(h, 'archive')
        off = 8
        while off + 60 <= len(d):
            hdr = d[off:off + 60]
            name, size = hdr[:16].decode('ascii', 'replace').rstrip(), int(hdr[48:58])
            data = d[off + 60:off + 60 + size]
            if name.startswith('#1/'):  # BSD long name, stored in front of the data
                n = int(name[3:])
                name, data = data[:n].rstrip(b'\0').decode('utf-8', 'replace'), data[n:]
            if not name.startswith('__.SYMDEF'):
                md = macho_digest(data)
                field(h, name, md if md is not None else b'raw' + data)
            off += 60 + size + (size & 1)
        if off != len(d):
            field(h, 'trailing', d[off:])
        return h.digest()
    field(h, 'raw', d)
    return h.digest()


def main(argv):
    out_c = None
    if argv[:1] == ['--c']:
        out_c, argv = argv[1], argv[2:]
    if not argv:
        sys.exit('usage: air-codegen-id.py [--c out.c] <object or archive>...')
    h = hashlib.sha256()
    field(h, 'D9MT-AIR-CODEGEN v1')
    for p in argv:
        field(h, os.path.basename(p), file_digest(p))
    hexid = h.hexdigest()
    if out_c:
        with open(out_c, 'w') as f:
            f.write('/* Generated by scripts/air-codegen-id.py: the AIR codegen id (ADR-0006). */\n'
                    'const char d9mt_air_codegen_id[] = "%s%s";\n' % (PREFIX, hexid))
    print(hexid)


if __name__ == '__main__':
    main(sys.argv[1:])
