# d9mt agent guidance

Work one prepared issue at a time, within its scope and acceptance criteria, and take the next
one only after this one meets the Definition of Done in [`CONTRIBUTING.md`](CONTRIBUTING.md).

The documentation is the source of truth and should answer scope and design questions:
[`CONTEXT.md`](CONTEXT.md), the decisions in [`docs/adr/index.md`](docs/adr/index.md), the
[README](README.md) and the design documents in [`docs/`](docs/). Look the answer up there before
asking or choosing an approach of your own. `docs/dev-notes/` is historical, not a specification.

- Correctness on every supported Mac comes first (ADR-0005). Correctness is the hash suite
  against the base commit; speed is “nothing measurably slower”, measured as
  [PERFORMANCE.md](PERFORMANCE.md) describes. A fix merges unless a cost is established at the
  measurable resolution; a new speed-only change shows identical rendering on other Mac
  generations or goes in behind a switch that is off by default. Both need evidence in the PR,
  not an argument.
- Metal 4 doesn't retain resources: anything a command buffer uses lives until its serial retires
  (ADR-0001).
- Don't touch the AIR codegen (`air/`, `airlib/`, dxbc-spirv, LLVM, `bridge/unix/air_cache.c`,
  `bridge/unix/cache.c`) unless the issue needs it: it changes the AIR codegen id and costs every
  player a shader recompile, unless a lineage with byte-identity evidence carries it (ADR-0006).
  The driver and the bridge still ship from one commit.
- Run benchmarks only in a quiet window the maintainer has granted, never while a game runs, and
  never open a test window on the maintainer's screen.
- Work on a branch and open a pull request into `main`. Never push to `main`, and never create
  issues, labels or releases without the maintainer's approval: this repository is public.

## Agent skills

### Issue tracker

GitHub issues in `jdrly/d9mt`. See [`docs/agents/issue-tracker.md`](docs/agents/issue-tracker.md).

### Triage labels

Vocabulary: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, `wontfix`, plus
type and `area:` labels. See [`docs/agents/triage-labels.md`](docs/agents/triage-labels.md).

### Domain docs

Single context: root `CONTEXT.md` plus `docs/adr/`. See [`docs/agents/domain.md`](docs/agents/domain.md).
