# d9mt agent guidance

Work one prepared issue at a time, within its scope and acceptance criteria, and take the next
one only after this one meets the Definition of Done in [`CONTRIBUTING.md`](CONTRIBUTING.md).

The documentation is the source of truth and should answer scope and design questions:
[`CONTEXT.md`](CONTEXT.md), the decisions in [`docs/adr/index.md`](docs/adr/index.md), the
[README](README.md) and the design documents in [`docs/`](docs/). Look the answer up there before
asking or choosing an approach of your own. `docs/dev-notes/` is historical, not a specification.

- Correctness is the hash suite against the base commit; speed is “nothing measurably slower”
  (ADR-0002), measured as [PERFORMANCE.md](PERFORMANCE.md) describes. Both need evidence in the
  PR, not an argument.
- Metal 4 doesn't retain resources: anything a command buffer uses lives until its serial retires
  (ADR-0001).
- Don't touch `bridge/` unless the issue needs it: it changes `winemetal.so`'s LC_UUID and costs
  every player a shader recompile (ADR-0004).
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
