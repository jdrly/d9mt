# Triage labels

The skills speak in terms of five canonical triage roles. This file maps those roles to the
label strings of this repository's issue tracker, and lists the type and area labels next to
them.

## Triage state (exactly one on an open issue)

| Label in mattpocock/skills | Label in our tracker | Meaning                                          |
| -------------------------- | -------------------- | ------------------------------------------------ |
| `needs-triage`             | `needs-triage`       | The maintainer needs to evaluate this issue      |
| `needs-info`               | `needs-info`         | Waiting on the reporter for more information     |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified, ready for an AFK agent          |
| `ready-for-human`          | `ready-for-human`    | Needs a human: a decision, hardware or the game  |
| `wontfix`                  | `wontfix`            | Will not be actioned                             |

When a skill mentions a role (for example "apply the AFK-ready triage label"), use the label
string from this table. An issue is `ready-for-agent` only when its verification can be done
without a person at the Mac; anything that needs the game played, a real GPU run or a
maintainer's decision is `ready-for-human`.

## Type (exactly one)

| Label           | Meaning                                                      |
| --------------- | ------------------------------------------------------------ |
| `bug`           | Wrong rendering, a hang, a crash or a build failure          |
| `enhancement`   | New or better behaviour                                      |
| `performance`   | Frame time, stutter, pacing, memory                          |
| `investigation` | Research or measurement; the findings go into the issue      |
| `documentation` | Only the docs change                                         |

The existing labels `rosetta` (running without Rosetta 2: arm64, ARM64EC) and `d9mt` (an issue
moved here from an app's repository) stay as they are.

## Area (one or more)

| Label             | Meaning                                                                   |
| ----------------- | ------------------------------------------------------------------------- |
| `area:frontend`   | DXVK's D3D9 front-end (`driver/d3d9`)                                     |
| `area:backend`    | The seam and the Metal 4 backend, submission, presentation (`driver/mtl`, `driver/backend`) |
| `area:shaders`    | The AIR compiler and dxbc-spirv (`air/`, `airlib/`, `patches/dxbc-spirv`) |
| `area:bridge`     | `winemetal` (`bridge/`); changes the LC_UUID (ADR-0004)                   |
| `area:caches`     | AIR cache, PSO archives, pipeline state cache                             |
| `area:build-test` | Build scripts, the hash suite, native tests, benchmarks (`scripts/`, `test/`) |
