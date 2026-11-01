## What changes

<!-- Two to five sentences: what this PR does and why, not a list of files. -->

Closes #

- **ADR:** none <!-- or docs/adr/NNNN-… -->

## How to verify

<!-- Steps for the reviewer: the test programs, native tests and benches to run, the game scene for the UAT. -->

## Evidence

<!-- Hash suite, the regression test on base and on this branch, the performance evidence for the change's kind (PERFORMANCE.md §1), winemetal.so's identity. -->

## Definition of Done

- [ ] `scripts/build-bridge.sh` and `scripts/build-driver.sh` for `ARCH=x64` and `ARCH=x86` build clean from scratch, without new warnings.
- [ ] The hash suite is frame-identical to the base commit (also with `MTL_DEBUG_LAYER=1`, `D9MT_NO_ATTACHMENT_OPT=1` and `d3d9.reproducibleCommandStream=True`), with no new Metal API validation messages.
- [ ] A bug fix has a regression test (a `test/` program or a `test/native/` test) that fails on the base commit and passes here where safe and feasible; otherwise state the violated contract and provide the strongest safe witness, with its limits (CONTRIBUTING).
- [ ] The native tests pass; a change to threading runs them under ThreadSanitizer, a change to memory handling under ASan/UBSan.
- [ ] Performance evidence follows the change's kind (PERFORMANCE.md §1): bounded review of added work; the representative guard's result when valid, otherwise timing recorded as pending, never passed; supported below-guard costs recorded for the cumulative release decision. Speed-only defaults need positive representative and game evidence plus cross-generation rendering coverage, or a default-off switch. Shader codegen adds semantic, OS and cache checks; diagnostics separate default-path effects from enabled overhead. Microbenchmark observations are reported in both directions.
- [ ] The AIR codegen id (`build/bridge/air-codegen-id.txt`) is `main`'s, or the PR says that it changes (every player's AIR cache recompiles once) and why, or adds a lineage entry with byte-identity evidence (ADR-0006). A state cache key signature bump is called out the same way.
- [ ] Every new environment variable or `dxvk.conf` option is in the tables of `docs/configuration.md`.
- [ ] Terms follow `CONTEXT.md`, and a new term is added in this PR; a changed decision has an ADR in this PR, or links an existing `accepted` ADR.
- [ ] The independent agent review (preferably GPT-6.1-Sol) has no open `Blocker` or `High`, and its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

## Review

<!-- An independent agent (preferably GPT-6.1-Sol) reviews this PR and puts its verdict here or in a comment. Squash-merge (subject = PR title) after a clean verdict (no open Blocker or High) and green checks. -->

- **Verdict:** pending

## Risks and what I didn't do

<!-- Known limits, deferred work, what a player could notice. An empty field is an answer too: write "none". -->
