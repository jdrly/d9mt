#!/usr/bin/env python3
"""commit_guard_extract.py: cut MtlContextImpl's commit path out of mtl_context_record.cpp by its
source anchors, for test/native/commit_guard_test.cpp (scripts/test-residency.sh): everything
from commitRecorded to the end of submit (commitRecorded, abandonRecording if the tree has it,
commitPrefix, submit), and the destructor, as the driver has them.

    commit_guard_extract.py <mtl_context_record.cpp> <out dir>

Writes <out dir>/commit_path.inc, <out dir>/context_dtor.inc, and <out dir>/commit_path_sig.h, which says what
commitRecorded returns (void before #63, bool since) for the test's declarations.
Exit 2 if an anchor is missing or not unique.
"""
import pathlib
import sys

STARTS = ('  bool MtlContextImpl::commitRecorded() {', '  void MtlContextImpl::commitRecorded() {')
END = '  static bool canaryEnabled() {'
DTOR_START = '  MtlContextImpl::~MtlContextImpl() {'
DTOR_END = '  void MtlContextImpl::beginRecording() {'


def fail(msg):
    print(f'commit_guard_extract: {msg}', file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) != 3:
        fail('usage: commit_guard_extract.py <mtl_context_record.cpp> <out dir>')
    src = pathlib.Path(sys.argv[1]).read_text()
    found = [s for s in STARTS if src.count(s) == 1]
    if len(found) != 1 or any(src.count(s) > 1 for s in STARTS):
        fail('commitRecorded: no unique definition')
    if src.count(END) != 1:
        fail(f'anchor {END.strip()!r} found {src.count(END)} times')
    start = src.index(found[0])
    end = src.index(END, start)
    body = src[start:end]
    for needed in ('void MtlContextImpl::commitPrefix() {', 'void MtlContextImpl::submit(bool waitCompleted) {'):
        if body.count(needed) != 1:
            fail(f'{needed!r} not in the extracted range')
    for anchor in (DTOR_START, DTOR_END):
        if src.count(anchor) != 1:
            fail(f'anchor {anchor.strip()!r} found {src.count(anchor)} times')
    dstart = src.index(DTOR_START)
    dtor = src[dstart:src.index(DTOR_END, dstart)]
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'commit_path.inc').write_text(body)
    (out / 'context_dtor.inc').write_text(dtor)
    ret = 'bool' if found[0].startswith('  bool') else 'void'
    (out / 'commit_path_sig.h').write_text(
        f'#define COMMIT_RECORDED_RET {ret}\n'
        f'#define HAS_ABANDON_RECORDING {1 if "void MtlContextImpl::abandonRecording() {" in body else 0}\n')


if __name__ == '__main__':
    main()
