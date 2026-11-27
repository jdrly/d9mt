#!/usr/bin/env python3
"""Set up driver/d3d9/d3d9_query.{h,cpp} for test/native/query_getdata_test.cpp.

    query_getdata_prepare.py <d3d9_query.cpp> <d3d9_query.h> <out dir> [<control>]

Copies both files unchanged into <out>/d3d9/ and writes the headers they include (the device
child, the device, mtl/mtl_context.h) as forwarders to test/native/query_fakes.h, so the test
compiles the driver's own query code. With <control>, plants one fault in the copied .cpp that
the test must catch:
  gpu-query      TIMESTAMPFREQ creates a GPU query, so its answer waits on the GPU
  endable        Issue(END) records an end for TIMESTAMPFREQ, so its answer waits on the render thread
  no-frequency   GetData answers 0 instead of the device's timestamp frequency
Each control's anchor must occur exactly once in the .cpp, or nothing is written and it exits 2.
"""
import os
import sys

CONTROLS = {
    "gpu-query": (
        "      case D3DQUERYTYPE_TIMESTAMPFREQ:\n        break;\n",
        "      case D3DQUERYTYPE_TIMESTAMPFREQ:\n"
        "        m_query[0] = dxvkDevice->createGpuQuery(MtlQueryType::Timestamp);\n"
        "        break;\n",
    ),
    "endable": (
        "        || QueryType == D3DQUERYTYPE_EVENT;\n",
        "        || QueryType == D3DQUERYTYPE_EVENT\n"
        "        || QueryType == D3DQUERYTYPE_TIMESTAMPFREQ;\n",
    ),
    "no-frequency": (
        "          m_dataCache.TimestampFreq = GetTimestampQueryFrequency();\n",
        "          m_dataCache.TimestampFreq = 0u;\n",
    ),
}

FORWARDERS = ("d3d9/d3d9_device_child.h", "d3d9/d3d9_device.h", "mtl/mtl_context.h")


def fail(msg):
    print(f"[query-getdata] {msg}", file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) not in (4, 5):
        fail("usage: query_getdata_prepare.py <d3d9_query.cpp> <d3d9_query.h> <out dir> [<control>]")
    cpp_path, h_path, out = sys.argv[1:4]
    control = sys.argv[4] if len(sys.argv) == 5 else None

    with open(cpp_path, encoding="utf-8") as f:
        cpp = f.read()
    with open(h_path, encoding="utf-8") as f:
        header = f.read()

    if control is not None:
        if control not in CONTROLS:
            fail(f"unknown control {control!r}; known: {', '.join(CONTROLS)}")
        anchor, planted = CONTROLS[control]
        hits = cpp.count(anchor)
        if hits != 1:
            fail(f"control {control}: anchor {anchor!r} found {hits} times in {cpp_path}, need exactly 1")
        cpp = cpp.replace(anchor, planted)

    for sub in ("d3d9", "mtl"):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    with open(os.path.join(out, "d3d9", "d3d9_query.cpp"), "w", encoding="utf-8") as f:
        f.write(f'#line 1 "{os.path.abspath(cpp_path)}"\n' + cpp)
    with open(os.path.join(out, "d3d9", "d3d9_query.h"), "w", encoding="utf-8") as f:
        f.write(f'#line 1 "{os.path.abspath(h_path)}"\n' + header)
    for name in FORWARDERS:
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            f.write('#pragma once\n#include "query_fakes.h"\n')
    print(f"[query-getdata] {os.path.basename(cpp_path)}: {control or 'as in the driver'}")


main()
