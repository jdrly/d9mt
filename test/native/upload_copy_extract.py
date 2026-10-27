#!/usr/bin/env python3
"""Cut UploadPerDrawData's vertex copy out of d3d9_device.cpp for test/native/upload_copy_test.cpp.

    upload_copy_extract.py <d3d9_device.cpp> <out dir>

Writes three fragments of the driver's own source, unchanged, each with a #line back to it:
  upload_vbocopy.inc  the VBOCopy struct
  upload_clamp.inc    one stream's clamp: from its vertexSize to its totalUpBufferSize update
  upload_copy.inc     one stream's copy: the block under `copy.copyBufferLength != 0`
Each anchor must occur exactly once in UploadPerDrawData, or nothing is written and it exits 2:
the test then can't silently compile something other than the driver's code.
"""
import os
import sys

FUNCTION = "void D3D9DeviceEx::UploadPerDrawData("
STRUCT = "struct VBOCopy {"
CLAMP_FIRST = "const uint32_t vertexSize = m_state.vertexDecl->GetSize(i);"
CLAMP_LAST = "totalUpBufferSize += vboCopies[i].copyBufferLength;"
COPY = "if (likely(copy.copyBufferLength != 0)) {"


def fail(msg):
    print(f"[upload-copy] {msg}", file=sys.stderr)
    sys.exit(2)


def block_end(lines, start):
    """The line that closes the brace opened on lines[start] (no braces in strings or comments)."""
    depth = 0
    for n in range(start, len(lines)):
        code = lines[n].split("//", 1)[0]
        depth += code.count("{") - code.count("}")
        if depth == 0:
            return n
    fail(f"no closing brace for line {start + 1}")


def only(lines, lo, hi, anchor):
    hits = [n for n in range(lo, hi + 1) if lines[n].strip() == anchor]
    if len(hits) != 1:
        fail(f"anchor {anchor!r}: {len(hits)} matches in UploadPerDrawData, need exactly 1")
    return hits[0]


def main():
    if len(sys.argv) != 3:
        fail("usage: upload_copy_extract.py <d3d9_device.cpp> <out dir>")
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.read().split("\n")

    heads = [n for n, l in enumerate(lines) if l.strip().startswith(FUNCTION)]
    if len(heads) != 1:
        fail(f"{FUNCTION!r}: {len(heads)} matches, need exactly 1")
    body = next(n for n in range(heads[0], len(lines)) if "{" in lines[n])
    lo, hi = body, block_end(lines, body)

    s = only(lines, lo, hi, STRUCT)
    first, last = only(lines, lo, hi, CLAMP_FIRST), only(lines, lo, hi, CLAMP_LAST)
    if last < first:
        fail("the clamp's last anchor comes before its first")
    c = only(lines, lo, hi, COPY)

    path = os.path.abspath(src)
    frags = {
        "upload_vbocopy.inc": (s, block_end(lines, s)),
        "upload_clamp.inc": (first, last),
        "upload_copy.inc": (c, block_end(lines, c)),
    }
    for name, (a, b) in frags.items():
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            f.write(f'#line {a + 1} "{path}"\n')
            f.write("\n".join(lines[a:b + 1]) + "\n")
        print(f"[upload-copy] {name}: {os.path.basename(src)} lines {a + 1}-{b + 1}")


main()
