## What changes

<!-- Two to five sentences: what this PR does and why, not a list of files. -->

Closes #

- **ADR:** none <!-- or docs/adr/NNNN-… -->

## How to verify

<!-- Steps for the reviewer: the test programs, native tests and benches to run, the game scene for the UAT. -->

## Evidence

<!-- Hash suite, the regression test on base and on this branch, the perf rounds (paired, with the control), winemetal.so's identity. -->

## Definition of Done

- [ ] `scripts/build-bridge.sh` and `scripts/build-driver.sh` for `ARCH=x64` and `ARCH=x86` build clean from scratch, without new warnings.
- [ ] The hash suite is frame-identical to the base commit (also with `MTL_DEBUG_LAYER=1`, `D9MT_NO_ATTACHMENT_OPT=1` and `d3d9.reproducibleCommandStream=True`), with no new Metal API validation messages.
- [ ] A bug fix has a regression test (a `test/` program or a `test/native/` test) that fails on the base commit and passes here.
- [ ] The native tests pass; a change to threading runs them under ThreadSanitizer, a change to memory handling under ASan/UBSan.
- [ ] Nothing measurably slower: paired rounds against the base commit in a quiet window, uncapped, with a control; a single commit's small delta is checked with the a64 layout control (or an identical-size placebo). A fix: no cost established at the measurable resolution. A speed-only change: identical rendering on the Mac generations named in *Evidence*, or behind a switch that is off by default.
- [ ] `winemetal.so` is byte-identical to `main`, or the PR says that its LC_UUID changes (every player's AIR cache recompiles once) and why it can't wait for the next bridge bundle. A state cache key signature bump is called out the same way.
- [ ] Every new environment variable or `dxvk.conf` option is in the README's tables.
- [ ] Terms follow `CONTEXT.md`, and a new term is added in this PR; a changed decision has an ADR in this PR, or links an existing `accepted` ADR.
- [ ] The independent agent review (preferably GPT-6.1-Sol) has no open `Blocker` or `High`, and its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

## Review

<!-- An independent agent (preferably GPT-6.1-Sol) reviews this PR and puts its verdict here or in a comment. Squash-merge (subject = PR title) after a clean verdict (no open Blocker or High) and green checks. -->

- **Verdict:** pending

## Risks and what I didn't do

<!-- Known limits, deferred work, what a player could notice. An empty field is an answer too: write "none". -->
