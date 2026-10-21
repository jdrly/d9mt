# Triage labels

The skills speak in terms of five canonical triage roles. This file maps those roles to the
label strings of this repository's issue tracker, and lists the type and area labels next to
them.

> **Proposed vocabulary.** The labels marked _new_ do not exist on GitHub yet; the maintainer
> creates them. Until then GitHub ignores them in the issue forms.

## Triage state (exactly one on an open issue)

| Label in mattpocock/skills | Label in our tracker | Meaning                                          | Exists |
| -------------------------- | -------------------- | ------------------------------------------------ | ------ |
| `needs-triage`             | `needs-triage`       | The maintainer needs to evaluate this issue      | _new_  |
| `needs-info`               | `needs-info`         | Waiting on the reporter for more information     | _new_  |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified, ready for an AFK agent          | _new_  |
| `ready-for-human`          | `ready-for-human`    | Needs a human: a decision, hardware or the game  | _new_  |
| `wontfix`                  | `wontfix`            | Will not be actioned                             | yes    |

When a skill mentions a role (for example "apply the AFK-ready triage label"), use the label
string from this table. An issue is `ready-for-agent` only when its verification can be done
without a person at the Mac; anything that needs the game played, a real GPU run or a
maintainer's decision is `ready-for-human`.

## Type (exactly one)

| Label           | Meaning                                                      | Exists |
| --------------- | ------------------------------------------------------------ | ------ |
| `bug`           | Wrong rendering, a hang, a crash or a build failure          | yes    |
| `enhancement`   | New or better behaviour                                      | yes    |
| `performance`   | Frame time, stutter, pacing, memory                          | yes    |
| `investigation` | Research or measurement; the findings go into the issue      | yes    |
| `documentation` | Only the docs change                                         | yes    |

The existing labels `rosetta` (running without Rosetta 2: arm64, ARM64EC) and `d9mt` (an issue
moved here from an app's repository) stay as they are.

## Area (one or more)

| Label             | Meaning                                                                   | Exists |
| ----------------- | ------------------------------------------------------------------------- | ------ |
| `area:frontend`   | DXVK's D3D9 front-end (`driver/d3d9`)                                     | _new_  |
| `area:backend`    | The seam and the Metal 4 backend, submission, presentation (`driver/mtl`, `driver/backend`) | _new_  |
| `area:shaders`    | The AIR compiler and dxbc-spirv (`air/`, `airlib/`, `patches/dxbc-spirv`) | _new_  |
| `area:bridge`     | `winemetal` (`bridge/`); changes the LC_UUID (ADR-0004)                   | _new_  |
| `area:caches`     | AIR cache, PSO archives, pipeline state cache                             | _new_  |
| `area:build-test` | Build scripts, the hash suite, native tests, benchmarks (`scripts/`, `test/`) | _new_  |
