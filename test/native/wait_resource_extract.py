#!/usr/bin/env python3
"""Cut D3D9DeviceEx::WaitForResource out of d3d9_device.cpp for test/native/wait_resource_test.cpp.

    wait_resource_extract.py <d3d9_device.cpp> <out dir>

Writes wait_resource.inc: the whole definition, from its signature to its closing brace, unchanged,
with a #line back to the source. The signature must occur exactly once, or nothing is written and
it exits 2: the test then can't silently compile something other than the driver's code.
"""
import os
import sys

FUNCTION = "bool D3D9DeviceEx::WaitForResource("


def fail(msg):
    print(f"[wait-resource] {msg}", file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) != 3:
        fail("usage: wait_resource_extract.py <d3d9_device.cpp> <out dir>")
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.read().split("\n")
    heads = [n for n, l in enumerate(lines) if l.strip().startswith(FUNCTION)]
    if len(heads) != 1:
        fail(f"{FUNCTION!r}: {len(heads)} matches, need exactly 1")
    start = heads[0]
    body = next((n for n in range(start, len(lines)) if lines[n].split("//", 1)[0].rstrip().endswith("{")), None)
    if body is None:
        fail("no opening brace after the signature")
    depth = 0
    for end in range(body, len(lines)):
        code = lines[end].split("//", 1)[0]
        depth += code.count("{") - code.count("}")
        if depth == 0:
            break
    else:
        fail("no closing brace")
    with open(os.path.join(out, "wait_resource.inc"), "w", encoding="utf-8") as f:
        f.write(f'#line {start + 1} "{os.path.abspath(src)}"\n')
        f.write("\n".join(lines[start:end + 1]) + "\n")


if __name__ == "__main__":
    main()
