#!/usr/bin/env python3
"""pass_barrier_extract.py: cut MtlContextImpl::emitProducerBarrier out of mtl_context_transfer.cpp
by its source anchors, for test/native/pass_barrier_test.cpp (scripts/test-pass-barrier.sh).

    pass_barrier_extract.py <mtl_context_transfer.cpp> <out dir> [<mutant>]

Writes <out dir>/emit_producer_barrier.inc: the method's definition as the driver has it, or with
one mutation applied (the witnesses of scripts/test-pass-barrier.sh --witness):
    no_vertex      the #64 Vertex bit is dropped again
    all_consumers  beforeStages widened to All
    all_producers  Vertex added to every afterStages
    no_epoch       the vertex epoch never advances
Exit 2 if an anchor is missing or not unique, or a mutation doesn't apply.
"""
import pathlib
import sys

START = '  void MtlContextImpl::emitProducerBarrier('
END = '  void MtlContextImpl::emitGraphicsBarrier('

MUTANTS = {
    'no_vertex': ('? afterStages | uint64_t(WMTStageVertex) : afterStages', '? afterStages : afterStages'),
    'all_consumers': ('MTL4CommandEncoder_barrier(encoder, waitStages, beforeStages,',
                      'MTL4CommandEncoder_barrier(encoder, waitStages, uint64_t(WMTStageAll),'),
    'all_producers': ('MTL4CommandEncoder_barrier(encoder, waitStages,',
                      'MTL4CommandEncoder_barrier(encoder, waitStages | uint64_t(WMTStageVertex),'),
    'no_epoch': ('m_vtxEpoch++;', ';'),
}


def fail(msg):
    print(f'pass_barrier_extract: {msg}', file=sys.stderr)
    sys.exit(2)


def main():
    if len(sys.argv) not in (3, 4):
        fail('usage: pass_barrier_extract.py <mtl_context_transfer.cpp> <out dir> [<mutant>]')
    src = pathlib.Path(sys.argv[1]).read_text()
    for anchor in (START, END):
        if src.count(anchor) != 1:
            fail(f'anchor {anchor.strip()!r} found {src.count(anchor)} times')
    start = src.index(START)
    end = src.index(END, start)
    body = src[start:end]
    if len(sys.argv) == 4:
        name = sys.argv[3]
        if name not in MUTANTS:
            fail(f'unknown mutant {name}')
        a, b = MUTANTS[name]
        if body.count(a) != 1:
            fail(f'mutant {name}: its anchor is in the method {body.count(a)} times')
        body = body.replace(a, b)
    out = pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'emit_producer_barrier.inc').write_text(body)


if __name__ == '__main__':
    main()
