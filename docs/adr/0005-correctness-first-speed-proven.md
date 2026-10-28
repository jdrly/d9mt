---
status: accepted
date: 2026-10-28
deciders: Jan Drlý
---

# Correctness on every supported Mac comes first, and a speed-only change proves itself or waits behind a switch

## Context

ADR-0002 (2026-10-21, amended by #59 and #95) made “nothing measurably slower” the merge rule:
every median slowdown over +0.25 % is flagged and re-verified, and a comparison the method can't
resolve goes to a council. Two things came up that it doesn't weigh:

- **The method doesn't reach 0.25 %.** On bindbench it resolves about ±1 % today: that is how
  far the A/A intervals spanned in the maintainer's runs of 2026-10-28 (not public). Under
  ADR-0002 a fix that prevents real bugs or GPU faults could wait on a question the method can't
  answer; its own *Consequences* said so.
- **Speed was measured on one Mac, correctness wasn't checked on others.** #18 (2026-10-18) let
  textures that aren't block-compressed be losslessly compressed, for the GPU cost at 4K. A
  player's report of magenta icons on a MacBook Air M4, a fanless Mac, is suspected to come from
  it: a texture that is created but not written yet may read as magenta there, and as zero on the
  maintainer's Mac. That isn't confirmed yet; a field test with `D9MT_TEX_PFV=1` (no lossless
  compression) is pending. `test/code_icon_churn.c` (#99) covers the pattern and doesn't reproduce
  it on an M5 Max. #18 had that off switch, and the player had no way to know it existed.

The maintainer revised the rule on 2026-10-28 after that report, and added a guard for the
player's smoothness at release: small costs that no single PR resolves can add up.

## Decision

- **Correctness on every supported Mac comes first.** A **fix**, a change that prevents a bug or a
  GPU fault, merges when no cost is established at the resolution the calibrated method actually
  reaches (the **measurable resolution**, about ±1 % on bindbench today). It doesn't need a bound
  below +0.25 %. A flag larger than that resolution is re-verified before the fix merges; a cost
  that the re-verification establishes takes the established-cost path below.
- **A speed-only change carries the burden of proof.** A change whose purpose is speed, not
  preventing a bug, needs evidence that it renders identically on Mac generations other than the
  one it was measured on, a slower and a fanless Mac among them, not only a faster benchmark.
  Without that coverage it goes in only behind a switch that is **off by default**, documented in
  the README's tables; a later PR that brings the coverage turns it on. Existing speed
  optimisations stay as they are; this applies to new speed-only changes.
- **+0.25 % stays a flag, not a hard stop.** Every median slowdown over +0.25 % (ms up, fps down;
  the median or the paired median against base) is flagged, as before. A real, established cost
  of a fix still goes to the maintainer. A small flag at the edge of the measurable resolution
  goes to a council (#95's decision 1) and doesn't block a fix.
- **The smoothness guard.** Before each stable release, the release candidate is compared with
  the last stable release on what the player feels: 1 % lows, hitches (frames over 33 and 50 ms),
  frame-time p99 and the present interval's spread, not only average fps or ms. Any measurable
  regression there goes to the maintainer before the release, with the numbers (PERFORMANCE.md
  §6). That is where small flags that add up are caught.
- **Carried over from ADR-0002 unchanged:**
  - Nothing merges that is measurably slower in any scenario; a gain in one scenario doesn't
    offset a loss in another.
  - The method: uncapped paired rounds against base in a quiet window, a byte-identical copy of
    base as a control, the paired difference and the count of slower rounds in the PR.
  - The flag's steps (re-verify; if the cost is established, find a solution that costs nothing;
    only if there is none, the maintainer, with a second opinion from the independent agent
    review) and the three outcomes: cost established, cost bounded below +0.25 % under stated
    conditions, unresolved (PERFORMANCE.md §2.4).
  - The layout controls (a64, the identical-size placebo; neither controls data layout; one block
    is never a pass), and the per-PR versus release split: per PR, layout-matched evidence; mode
    and layout shifts at the release candidate check.
  - The binding check is the release candidate as shipped against the previous release; a
    slowdown there that is layout gets a layout fix, not a revert. Only the maintainer grants an
    exception.

## Consequences

- A fix no longer waits for a method that resolves 0.25 %. The price: a fix can merge with a real
  cost below the measurable resolution. Its flags stay on record and go to a council, and the
  release candidate's checks are where their sum shows.
- A new speed-only change costs more: it needs testers' Macs of other generations, or it ships off
  by default, so players don't get its gain until the coverage exists. While a switch exists there
  are two paths to keep working.
- With the switch off by default, the player on an untested Mac never needs to know it exists.
- The smoothness guard costs time at every release candidate, and part of it isn't covered by a
  tool yet (PERFORMANCE.md §6, _Open_).
- “Fix” and “speed-only” are a classification the PR makes, and a review can dispute it.

## Alternatives considered

- **Keep ADR-0002 as it was**, fixes waiting for a method that resolves 0.25 %: rejected. It holds
  fixes for real bugs and GPU faults on a question the method can't answer today, while the bugs
  are real.
- **A speed-only change on by default, with an off switch documented**: rejected. The player who
  needs the switch is the one on a Mac nobody tested, and the one least likely to know it exists;
  #18 had `D9MT_TEX_PFV=1`. `D9MT_AIR_FASTMATH` is already off by default, with `1` for the faster
  path.
- **Drop the +0.25 % flag**: rejected by the maintainer. It stays as a flag, so small costs stay
  visible and on record.
- **Revert or switch off existing speed optimisations until they are proven on every Mac**:
  rejected by the maintainer. Today's experience is smooth, and the rule applies to new changes.

## Open questions

- How much hardware coverage is enough (how many generations, which Macs) isn't set beyond “a
  slower and a fanless Mac among them”.
- Whether #18 causes the M4 Air's magenta icons (the field test is pending).
- No benchmark prints the 1 % lows directly, and there is no scripted game scene in this
  repository for the smoothness guard (PERFORMANCE.md §6).
- The measurable resolution is bindbench's; the other benchmarks' aren't calibrated yet.
