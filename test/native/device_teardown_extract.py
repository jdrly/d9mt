#!/usr/bin/env python3
"""device_teardown_extract.py: cut MtlDeviceImpl's destructor out of mtl_device_impl.cpp by its source
anchors, for test/native/device_teardown_test.cpp (scripts/test-residency.sh), as the driver has it.

    device_teardown_extract.py <mtl_device_impl.cpp> <out dir>

Writes <out dir>/device_dtor.inc. Exit 2 if an anchor is missing or not unique.
"""
import pathlib
import sys

START = '  MtlDeviceImpl::~MtlDeviceImpl() {'
END = '  void* MtlDeviceImpl::hostAlloc(uint64_t size) {'


def main():
    if len(sys.argv) != 3:
        print('usage: device_teardown_extract.py <mtl_device_impl.cpp> <out dir>', file=sys.stderr)
        sys.exit(2)
    src = pathlib.Path(sys.argv[1]).read_text()
    for anchor in (START, END):
        if src.count(anchor) != 1:
            print(f'device_teardown_extract: anchor {anchor.strip()!r} found {src.count(anchor)} times', file=sys.stderr)
            sys.exit(2)
    start = src.index(START)
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'device_dtor.inc').write_text(src[start:src.index(END, start)])


if __name__ == '__main__':
    main()
