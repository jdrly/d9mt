#!/usr/bin/env python3
"""Cut createSampler's fill of WMTSamplerInfo out of mtl_device_impl.cpp for
test/native/sampler_lod_bias_test.cpp.

    sampler_fill_extract.py <mtl_device_impl.cpp> <out dir>

Writes <out>/sampler_fill.inc: the driver's own lines, unchanged, from the struct's declaration
to the MTLDevice_newSamplerState call, with a #line back to the source. Each anchor must occur
exactly once in MtlDeviceImpl::createSampler, or nothing is written and it exits 2.
"""
import os
import sys

FUNCTION = "Rc<MtlSamplerState> MtlDeviceImpl::createSampler(const MtlSamplerKey& key) {"
FIRST = "WMTSamplerInfo samplerInfo = {};"
LAST = "obj_handle_t handle = MTLDevice_newSamplerState(m_device.handle(), &samplerInfo);"


def fail(msg):
    print(f"[sampler-fill] {msg}", file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) != 3:
        fail("usage: sampler_fill_extract.py <mtl_device_impl.cpp> <out dir>")
    src, out = sys.argv[1], sys.argv[2]
    with open(src, encoding="utf-8") as f:
        lines = f.read().split("\n")

    heads = [n for n, l in enumerate(lines) if l.strip() == FUNCTION]
    if len(heads) != 1:
        fail(f"{FUNCTION!r}: {len(heads)} matches, need exactly 1")
    depth, end = 0, None
    for n in range(heads[0], len(lines)):
        code = lines[n].split("//", 1)[0]
        depth += code.count("{") - code.count("}")
        if depth == 0:
            end = n
            break
    if end is None:
        fail("createSampler: no closing brace")

    def only(anchor):
        hits = [n for n in range(heads[0], end + 1) if lines[n].strip() == anchor]
        if len(hits) != 1:
            fail(f"anchor {anchor!r}: {len(hits)} matches in createSampler, need exactly 1")
        return hits[0]

    a, b = only(FIRST), only(LAST)
    if b < a:
        fail("the call comes before the struct")
    with open(os.path.join(out, "sampler_fill.inc"), "w", encoding="utf-8") as f:
        f.write(f'#line {a + 1} "{os.path.abspath(src)}"\n')
        f.write("\n".join(lines[a:b + 1]) + "\n")
    print(f"[sampler-fill] sampler_fill.inc: {os.path.basename(src)} lines {a + 1}-{b + 1}")


main()
