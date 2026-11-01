---
status: accepted
date: 2026-11-01
deciders: Jan Drlý
---

# Correctness fixes merge on correctness evidence with a representative performance guard

## Context

ADR-0005 prioritizes correctness but ties a fix to microbenchmark resolution and retains
a prohibition on any established slowdown. Small synthetic workloads are sensitive to
binary and process placement. More rounds of one binary pair can make that pair's
interval narrow without establishing the effect in a game. Issue #142 investigated
randomized layouts. Its proposed methods exposed substantial measurement and
certification costs without producing a calibrated hardware result. A safety fix must
not wait for an unsupported precision claim.

On the only test Mac, an M5 Max, a byte-identical d3d9.dll ran 6% faster under a
variant path one character longer, recorded in [#65](https://github.com/jdrly/d9mt/issues/65).
One pair of builds read +5.31%, -2.42% and -5.28% in three blocks while identical
copies agreed within +0.2%, as recorded in PERFORMANCE.md §2.5. The unbound-resource
fix [#133](https://github.com/jdrly/d9mt/pull/133) read +4.5% and +5.1% on bindbench,
every round slower, although bindbench does not execute the corrected
argument-buffer path. Fixes intended to prevent GPU faults, which can take the
whole Mac down, were held on such readings. Only slowdowns were flagged, so the
reports read one-sided.


## Decision

This decision supersedes ADR-0005. Correctness on supported hardware remains the first
requirement. A correctness or stability fix needs review, relevant tests, image
comparisons, appropriate Metal validation and the specified game UAT. Prefer a safe test
that fails on base and passes on the fix. State when the failure cannot be reproduced or
the available test proves only an API contract.

Representative workloads guard against substantial regressions. Their thresholds come
from a recorded calibration of the actual comparison, with uncertainty and detection
power, and require maintainer acceptance. PERFORMANCE.md describes confirmation and
escalation. Until calibration exists, timing remains explicitly unverified; correctness
fixes, diagnostics and refactors may merge on current-head correctness, a bounded review
of added work, independent review and specified game UAT. Record timing as pending. A
credible material loss goes promptly to the maintainer. Speed-only changes remain off by
default during this transition.

A confirmed material representative regression requires investigation and a narrower
implementation where practical, regardless of the static review result. The maintainer
may accept a necessary cost of a correctness fix. Record the cost, uncertainty, benefit
and independent review's opinion. A supported cost below the accepted guard does not
independently hold a correctness fix. Record it in the PR and performance ledger and
list it for the maintainer in the release candidate record. Adoption of this policy
delegates those per-PR decisions under the accepted guard; the maintainer records
acceptance of the cumulative tradeoff before release. A credible material loss still
goes promptly to the maintainer.

Speed-only changes enabled by default need a representative gain, a comparable
before/after game session and rendering checks on other generations, including slower
and fanless Macs. Otherwise they may merge as reviewed experiments behind documented
switches that are off by default. An experimental switch does not waive correctness.
Existing speed optimizations stay as they are; this applies to new speed-only changes.
Refactors need behavioral proof and the regression guard, not a speedup. Diagnostics
separate their disabled cost from enabled overhead. Shader codegen changes add semantic,
target-OS and cache checks under ADR-0006; report one-time recompilation separately from
warm performance.

Microbenchmarks, alignment controls, placebos and randomized layouts are optional
diagnostics. They do not independently gate a merge or release. A static review records
inspected paths and added work, not a zero-cost certificate. Its tooling is optional;
unchanged instructions do not prove unchanged performance. Report gains and losses
symmetrically with their conditions and uncertainty. An unresolved measurement is not
zero cost. Results from one GPU generation are scoped to it.

The combined release candidate is checked against the previous stable release for
correctness, representative performance, smoothness and game behavior. Release the
tested executable artifacts. Investigate confirmed regressions and record any maintainer
acceptance before release.

## Consequences

Safety fixes no longer wait for microbenchmarks to resolve small placement effects.
Small costs may accumulate and poorly represented workloads may regress; the combined
candidate and affected-hardware testing remain necessary. The policy requires useful
workloads and field evidence, so it cannot promise zero regressions. New speed-only
defaults can remain experimental while tester coverage is missing.
The representative game traces contain game data and stay private. Outside
contributors cannot run that trace-based guard; the maintainer runs it. The
measurement tooling and synthetic workload checks can remain public. All timing
currently comes from one Mac; costs and gains on other GPU generations remain
unknown unless testers measure them.


## Alternatives considered

- Keep fixed sub-percent microbenchmark thresholds. Rejected because they do
  not establish equivalent precision for different binaries or game workloads.
- Gate every fix on randomized layouts. Retained as a research option, rejected
  as a universal requirement without calibrated precision and affordable power.
- Use subjective play alone. Rejected because it misses controlled comparisons,
  cold-start behavior and reproducible correctness tests.

- Amend ADR-0005 in place. Rejected because its decision and carried-over method
  would contradict this record; an accepted ADR must describe what applies.

## Open questions

The representative suite, its per-metric calibration and accepted regression guards are
pending. No numerical threshold is established by this decision.

## Implementation status as of 2026-11-01

Policy adopted on 2026-11-01. Documentation records the accepted policy; representative
instruments and calibration remain pending. Existing checks do not imply that the new guard has passed.
