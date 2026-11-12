# Contributing to d9mt

d9mt is a Direct3D 9 → Metal 4 driver for Wine on Apple Silicon (see the [README](README.md)).
Its main users are [SWTOR on Mac](https://github.com/jdrly/swtor-on-mac) and
[SWGL on Mac](https://github.com/jdrly/swgl-on-mac), which ship it at a pinned commit, so a
change here reaches players through their next release.

By contributing you agree that your contribution is licensed like the code it changes: the
zlib/libpng license for the driver and d9mt's own code, the LGPL 2.1 or later for the bridge in
`bridge/` (README, *License*).

## Before you start

```bash
gh auth status                       # GitHub authentication
export PATH="/opt/homebrew/bin:$PATH" # the scripts need bash ≥ 4.3
scripts/fetch-deps.sh                 # dxbc-spirv at its pin, with d9mt's patches
```

Read [CONTEXT.md](CONTEXT.md). Without it you are likely to introduce synonyms that come back in
review.

## Sources of truth and reading order

1. **[CONTEXT.md](CONTEXT.md)**: the terms to use.
2. **[docs/adr/](docs/adr/index.md)**: accepted decisions. An ADR with status `accepted` is the
   target contract even when the code differs. Record the difference as a gap; **don't weaken an
   ADR to match the current code**.
3. **[README.md](README.md)**: what d9mt does today, how to build, test, install and configure
   it, its known issues. [docs/configuration.md](docs/configuration.md) lists every option and
   switch, [docs/building.md](docs/building.md) and [docs/testing.md](docs/testing.md) the build
   and test details.
4. **The design documents in [docs/](docs/)** (`state-cache.md`, `state-cache-journal.md`,
   `g1-early-commit.md`, `gpu-4k.md`, `pso-key-hash.md`, …): the design and the measurements of
   a feature. `docs/dev-notes/` is neo773's historical material from v2's development, not
   maintained and not a specification.
5. **[PERFORMANCE.md](PERFORMANCE.md)**: the binding performance guide: the rule, how to
   measure, the benchmarks, the diagnostics, the hot-path design rules and the release
   candidate's health pass.
6. **The code and its tests**: `test/` (D3D9 test programs) and `test/native/` (unit tests).

If two sources conflict, report the conflict instead of silently choosing one.

## Issues

- Every change that isn't trivial starts from an issue: a **bug report** or a **task**, a
  precise specification.
- Titles use `Context: Actionable result`, for example
  `Present path: Keep the drawable until the GPU has retired it`. The issue, the pull request
  and the squash commit carry the same title; a measured result may follow in parentheses
  (`PSO key hash: … (psobench blend -47%)`).
- The area is a label, never a title prefix (see
  [docs/agents/triage-labels.md](docs/agents/triage-labels.md)).
- *Affected areas* lists only paths you have verified in the repository.
- *Verification (UAT)* is a scenario a non-technical player can play through: each step is an
  action in a game and, after the arrow, the expected result. d9mt has no window of its own;
  when no game shows the change, the scenario uses a test program from `test/`.

## Branches

- Never push directly to `main`. Every change goes through a pull request into `main`.
- Name the branch `<type>/<issue>-<short-description>`, in English `lower-case-kebab-case`, for
  example `fix/23-present-queue-hang`. Types: `feat`, `fix`, `perf`, `port` (a port from DXVK or
  dxbc-spirv upstream), `diag`, `test`, `chore`, `docs`. A change without an issue leaves out the
  number.
- One branch is one issue. Measurement variants (`tmp/…`) are never merged.

## Commits

- Write commit messages in **English**, in the imperative (`keep`, `fix`, `remove`; not `kept`
  or `keeping`).
- The subject follows the title form, `Context: what the commit does`, for example
  `Present path: keep the drawable and the layer's residency set until the GPU is done with
  them`. No emoji, no scope tags.
- The body says why, with the evidence: what failed before, what was measured, what was not
  done. Several focused commits are fine; they are squashed on merge.

## Pull requests

- The PR title is **identical to the issue title**.
- Fill in the [template](.github/PULL_REQUEST_TEMPLATE.md), with `Closes #<issue>`. The
  *Evidence* section carries the hash suite, the regression test on base and on the branch, the
  perf rounds and `winemetal.so`'s identity.
- Keep the PR a **draft** until it meets the Definition of Done.
- One change per PR: don't mix a fix with unrelated refactoring, and don't bundle independent
  optimizations; each is measured on its own.
- **Review and merge.** Every PR gets an **independent agent review** (preferably GPT-6.1-Sol)
  by an agent that didn't write the change; its verdict goes into the PR, as a comment or in the
  description. The PR is **squash-merged**, with the PR title as the commit subject and the PR's
  description as its body, once that verdict has no open `Blocker` or `High` **and** the
  repository's checks are green: the `Checks` workflow, which builds d9mt and runs the tests that
  need no GPU ([docs/testing.md](docs/testing.md#ci)), and for everything it can't
  run, the Definition of Done's evidence in the PR. The maintainer doesn't review code by hand. A
  rebase onto a changed `main` says how conflicts were resolved.

## Code review

- Read the issue and its UAT before the diff.
- The independent agent review (see *Pull requests*) ends in the verdict `MERGE` or `FIX FIRST`,
  often with a condition (“MERGE after the ABBA”).
- Classify every finding by severity. `Blocker` and `High` prevent the merge; `Medium` and
  `Low` may merge with a follow-up issue.

  | Severity | Meaning |
  |---|---|
  | `Blocker` | The pull request must not merge until it is resolved. |
  | `High` | Materially wrong behaviour, or a hard project-rule violation. |
  | `Medium` | Significant duplication, overengineering or maintainability risk. |
  | `Low` | Local cleanup that can go with related work. |

- In d9mt a finding is a Blocker by default when it can change a frame against
  base unintentionally, release a Metal object before the GPU has retired it,
  or leave a confirmed material representative regression without a resolution
  or recorded maintainer decision under PERFORMANCE.md. A microbenchmark delta
  alone is a diagnostic observation, not a performance Blocker.
- A finding describes **one problem with one fix boundary**, not one affected file.
- Findings stay in the pull request; never commit a review ledger. Decisions that should outlive
  the review go into the PR description.

## Definition of Done

A change is done only when all of these hold:

- [ ] **Clean builds.** `scripts/build-bridge.sh` and `scripts/build-driver.sh` for `ARCH=x64` and
      `ARCH=x86` build from scratch without new warnings. The driver and the bridge are built,
      tested and shipped together: there is no version handshake between them.
- [ ] **Hash suite equals base.** Every program of the hash suite (the D3D9 test programs
      `test/code_*.c`, except `code_sm3_ref` and `code_statecache`, which have harnesses of their
      own; five frames each, with the exceptions in docs/testing.md § Hash suite) renders frames
      identical to the base commit's. It
      also runs with `MTL_DEBUG_LAYER=1` (no new Metal API validation messages),
      `D9MT_NO_ATTACHMENT_OPT=1` and `d3d9.reproducibleCommandStream=True`, and, for a change to
      the submission path, with `D9MT_EARLY_COMMIT=always`. `D9MT_LIFETIME_CHECK=1` finds no
      violation.
- [ ] A bug fix adds or extends a regression test and shows it failing on base
      and passing on the branch where safe and feasible. When the hardware fault
      cannot reproduce locally or safely, state the violated contract, provide
      the strongest safe production/API witness available, and document what it
      proves and which hardware/runtime checks remain. Do not provoke a GPU fault
      merely to obtain a negative control.
- [ ] **Native tests.** `scripts/run-native-tests.sh` passes, and a change that a test it skips
      covers runs that test's own runner ([docs/testing.md](docs/testing.md)); a change to threading runs the
      affected tests under ThreadSanitizer (`scripts/test-state-cache.sh`,
      `scripts/test-compile-mt.sh`), a change to memory handling under ASan/UBSan.
- [ ] Performance evidence follows the change's kind in PERFORMANCE.md §1.
      Name the kind and record a bounded review of added work, inspected paths,
      architectures and omissions. Provide the accepted representative guard's
      result when valid. During the explicit transition record timing as pending,
      never passed, and preserve correctness, review and UAT requirements.
      Record supported below-guard costs for the cumulative release decision.
      Speed-only defaults need positive representative and game evidence plus
      cross-generation rendering coverage; otherwise use a documented default-off
      experiment. Shader codegen adds semantic, OS and cache checks. Diagnostics
      separate default-path effects from enabled overhead. Report optional
      microbenchmark observations in both directions with their limits.
- [ ] **AIR codegen id.** A PR that changes the bridge or the AIR codegen states its AIR codegen
      id (`build/bridge/air-codegen-id.txt`) next to `main`'s. If it changes, every player's AIR
      cache recompiles once: the PR says so, or adds a lineage entry with the evidence that the
      new codegen writes byte-identical metallibs and reflection (ADR-0006). A change to the
      pipeline state cache's key signature resets every player's state cache and is called out
      and batched into one release.
- [ ] **Docs.** Every new environment variable or `dxvk.conf` option is in the tables of
      [docs/configuration.md](docs/configuration.md);
      a design that needs explaining has its document in `docs/`.
- [ ] Terms follow `CONTEXT.md`; a new term is added in the same PR.
- [ ] A changed decision has an ADR in the same PR, or links an existing `accepted` ADR.
- [ ] The independent agent review (preferably GPT-6.1-Sol) has no open `Blocker` or `High`, and
      its verdict is in the PR.
- [ ] The issue's UAT scenario has been played through and ticked off in the issue.

_Open:_ the hash suite's runner (`scripts/run-hash-tests.sh`, `test/expected-failures.txt`) is on
the branch `chore/test-runner` and not on `main` yet; until it lands, the suite runs from the
maintainer's workspace. The native tests' runner (`scripts/run-native-tests.sh`) and the
benchmarks (`test/perf/`, `test/bench_4k.c`, PERFORMANCE.md §3) are on `main`.

## Never do these things

- Push directly to `main`.
- Merge a change that renders a frame differently from base without the issue asking for it.
- Bypass the performance decisions or required evidence in PERFORMANCE.md.
- Measure while a game or unrelated workload runs.
- Merge a speed-only change enabled by default without the evidence PERFORMANCE.md §1 requires.
- Release a Metal object the GPU may still use (ADR-0001).
- Change the AIR codegen in passing: it costs every player a shader recompile (ADR-0006).
- Mix a driver and a bridge from different builds.
- Commit game files, captures or logs with a player's account data.
- Add a dependency without the maintainer's approval.
- Change `CONTEXT.md` or an ADR incidentally during unrelated work.
