---
status: accepted
date: 2026-10-21
deciders: Jan Drlý
---

# Nothing merges that is measurably slower

## Context

d9mt's users run it to make games faster; a correctness fix or a feature that costs frame time on
the per-draw or per-bind path defeats that. Microbenchmarks are noisy: a rebuild alone moves one
by up to ±3 % through code and data layout, so a single measurement can't tell a regression from
layout.

## Decision

- No change merges that is measurably slower in any scenario. The per-draw and per-bind paths are
  the most sensitive; a gain in one scenario doesn't offset a loss in another.
- The method (CONTRIBUTING, *Definition of Done*): uncapped runs (`D9MT_BG_FPS=0
  D9MT_STALL_PACE=0`), each benchmark alone in a quiet window, never while a game runs; paired
  rounds against the base commit with the order alternated; a byte-identical copy of base as a
  control; the paired difference and the count of slower rounds in the PR.
- Every median slowdown over 0.25 % (ms up, fps down; the median or the paired median against
  base) is flagged and re-verified before a merge, even when it lies inside the control's range
  (the maintainer, 2026-10-23). `scripts/perf-compare.py` marks it `FLAG >0.25%`. A flag is
  first the author's: re-verify; if it holds, find a cost-free solution; only if there is none,
  take it to the maintainer with a solid explanation and a second opinion from the independent
  agent review (GPT-6.1-Sol).
- A small delta of a single commit is checked with the a64 layout control (both sides built with
  `-falign-functions=64`). When the change alters the size of its translation unit's code, the
  control is a placebo of identical size instead.
- The binding check is the release candidate as shipped (default alignment) against the previous
  release. A slowdown there that is layout gets a layout fix (alignment, function order), each
  measured on its own, not a revert of the commit that happened to move it.

## Consequences

- Every PR needs a benchmark run, and benchmark windows are a scarce resource: nothing else may
  run on the Mac meanwhile.
- A fix whose cost can't be removed waits until it can; “just measure it” isn't enough.
- Layout effects are judged per release, not per commit.

## Alternatives considered

- **Judge each commit by its raw delta**: rejected; layout moves microbenchmarks by up to ±3 %, so
  it would block changes for effects that aren't theirs and flip with the next merge.
- **Accept small regressions for correctness or features**: rejected by the maintainer (“we're
  aiming to improve, not regress”). The PSO MRU change (#40) is no exception: the cause of its
  synthetic worst case (psobench topo) was gone with #33, merged before it, and what remained
  flipped sign with alignment, so it was layout (#40).

## Open questions

- Most benchmarks (bindbench, churnbench, psobench and others) are not in this repository yet;
  only `test/bench_4k.c` is.
- Building releases with `-falign-functions=64` would make layout steadier; it is proven for one
  benchmark only and not decided.
