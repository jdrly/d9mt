#!/usr/bin/env python3
"""commit_guard_extract.py: cut MtlContextImpl::commitRecorded out of mtl_context_record.cpp by its
source anchors, for test/native/commit_guard_test.cpp (scripts/test-residency.sh).

    commit_guard_extract.py <mtl_context_record.cpp> <out dir>

Writes <out dir>/commit_recorded.inc: the method's definition as the driver has it.
Exit 2 if an anchor is missing or not unique.
"""
import pathlib
import sys

START = '  void MtlContextImpl::commitRecorded() {'
END = '  void MtlContextImpl::commitPrefix() {'


def main():
    if len(sys.argv) != 3:
        print('usage: commit_guard_extract.py <mtl_context_record.cpp> <out dir>', file=sys.stderr)
        sys.exit(2)
    src = pathlib.Path(sys.argv[1]).read_text()
    for anchor in (START, END):
        if src.count(anchor) != 1:
            print(f'commit_guard_extract: anchor {anchor.strip()!r} found {src.count(anchor)} times', file=sys.stderr)
            sys.exit(2)
    start = src.index(START)
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'commit_recorded.inc').write_text(src[start:src.index(END, start)])


if __name__ == '__main__':
    main()
