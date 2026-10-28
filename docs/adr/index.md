# Architecture Decision Records

An ADR records a decision or a proposed direction. **Its existence does not prove that the
behaviour is implemented.** When the code and an ADR differ, the code and its checks describe
what runs; the ADR stays the target and the difference is recorded as a gap.

## Statuses

| Status | Meaning |
|---|---|
| `proposed` | Awaiting discussion; **not an implementation specification**. |
| `accepted` | The valid target contract. |
| `superseded by ADR-NNNN` | Historical context; follow its successor. |
| `deprecated` | Don't use this approach in new work. |

## When to write an ADR

Only when there were real alternatives with consequences that matter. Ask: “Will someone want
to know a year from now why this wasn't done differently?” Tool choices and style rules belong in
`CONTRIBUTING.md`, not in an ADR.

## Format

`NNNN-english-slug.md`, in English, started from [0000-template.md](0000-template.md). Never reuse
a number; keep superseded records and change their status. Write only what a source shows, and
put what isn't decided under *Open questions*.

## Records

This table is the status source of truth. Update both it and the ADR's frontmatter.

| # | Decision | Status |
|---|---|---|
| [0001](0001-gpu-objects-live-until-retired.md) | Metal objects live until the GPU has retired the last command buffer that uses them | `accepted` |
| [0002](0002-nothing-measurably-slower.md) | Nothing merges that is measurably slower | `superseded by ADR-0005` |
| [0003](0003-pipeline-state-cache-in-d3d9.md) | A pipeline state cache in `d3d9.dll`, with a journal for the newest records | `accepted` |
| [0004](0004-batch-bridge-changes.md) | Changes to `winemetal.so` are batched, because its LC_UUID keys every player's AIR cache | `accepted` |
| [0005](0005-correctness-first-speed-proven.md) | Correctness on every supported Mac comes first, and a speed-only change proves itself or waits behind a switch | `accepted` |

## Candidates

Recorded elsewhere and worth an ADR once someone needs the reasoning: Metal 4 only, no Metal 3 path
(README, *Status*); d9mt's own AIR compiler instead of MSL at runtime (README). Their rationale is
only partly written down (`docs/dev-notes/`, historical).
