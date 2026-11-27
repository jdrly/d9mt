---
status: accepted
date: 2026-11-26
deciders: Jan Drlý
---

# d9mt is tuned for SWTOR behind an app profile, and every other game keeps the generic path

## Context

d9mt has two main users. [SWTOR on Mac](https://github.com/jdrly/swtor-on-mac) ships it for
SWTOR, and [SWGL on Mac](https://github.com/jdrly/swgl-on-mac) ships it for Star Wars Galaxies
Legends, a 32-bit game. Any other D3D9 game can load it too. Most of my work on d9mt is for
SWTOR: its frame times, its hitches and its rendering are what I measure and what players report.

Until now every change applied to every game. A change tuned on SWTOR captures either waited
until I could show it didn't hurt SWGL, or it reached SWGL untested. Neither is good: SWTOR
tuning stalls, or SWGL players carry a cost nobody measured for them.

DXVK has the same problem across thousands of games and answers it with app profiles: a table
of exe names with the options each game gets (`driver/util/config/config.cpp`). d9mt's front-end
comes from DXVK, and its bridge from DXMT (`driver/UPSTREAM`, `bridge/UPSTREAM`). Both upstreams
target other GPUs and other games than SWTOR on Apple's GPUs.

## Decision

- **I tune d9mt for SWTOR.** A change meant for SWTOR alone goes behind the `swtor` app profile.
  d9mt selects it when the game's exe is named `swtor.exe`, in any case and in any folder whose
  path Windows' `MAX_PATH` holds;
  `D9MT_APP_PROFILE=generic|swtor` overrides that for tests and A/Bs
  ([configuration](../configuration.md#environment-variables)). The app profile is a struct the
  code reads (`driver/mtl/mtl_app_profile.h`). A SWTOR-only change adds its field there, with the
  generic value as the default.
- **Every other game runs the `generic` app profile**, SWGL included. That is the path d9mt has
  today, and it changes only under the existing rules. The hash suite guards it: its programs
  aren't `swtor.exe`, so they run generic, and a SWTOR-only change leaves them frame-equal to
  base. The suite's `appswtor` variant runs the same programs under the `swtor` app profile.
- **I judge a SWTOR-only change on SWTOR captures under ADR-0007**: its correctness evidence, the
  representative guard on SWTOR workloads, and a UAT in SWTOR. A synthetic program alone doesn't
  show what such a change does.
- **A fix that every game needs goes on the generic path**, not behind the app profile. The
  `swtor` app profile holds choices that are right for SWTOR and unproven or wrong elsewhere.
- **I read upstream and don't follow it.** I read DXVK, DXMT and dxbc-spirv and port what helps
  SWTOR or every game (`port/` branches, CONTRIBUTING). d9mt doesn't track their releases, and an
  upstream default isn't a reason to change d9mt's without d9mt's own evidence.

## Consequences

- Two paths to keep correct. Every field of the app profile is a branch that only SWTOR takes in
  practice, and the hash suite's programs don't draw what SWTOR draws. The `appswtor` variant
  shows that the override reached d9mt (the runner requires its log line) and compares the
  programs' frames with base; it can't show what a change does in SWTOR. A SWTOR session can.
- A SWTOR-only change that would also help SWGL doesn't reach SWGL players until someone moves it
  to the generic path, with the evidence that needs.
- The captures that judge SWTOR-only changes contain game data and stay private (ADR-0007).
  Outside contributors can't run that check. I run it.
- The match is on the file name only. A renamed `swtor.exe` runs generic, and any program named
  `swtor.exe` gets SWTOR's tuning. `D9MT_APP_PROFILE` covers both cases by hand.
- d9mt drifts from DXVK and DXMT. Each port gets harder the further the code has moved.

## Alternatives considered

- **Tune the one path for every game.** Rejected: SWGL and other games would carry choices
  measured only on SWTOR, and a SWTOR change would wait for evidence from games I don't measure.
- **A `dxvk.conf` option per tuning, set by SWTOR on Mac.** Rejected: each tuning becomes a
  player-visible option to document and keep, the app has to ship and maintain the config, and
  a player's own `dxvk.conf` can drop it by accident.
- **DXVK's built-in table in `config.cpp`.** Rejected for now: it sets string options that the
  code reads through the config, so every tuning would still need an option of its own, as
  above. A field of the app profile can later read an option where one makes sense.
- **Match the exe by hash or version.** Rejected: every game patch would drop SWTOR's tuning until
  d9mt learned the new hash.

## Open questions

- Which tuning goes behind the `swtor` app profile first. This record adds none: today both app
  profiles hold the same values, so no game behaves differently.
- Whether SWGL gets an app profile of its own once something is tuned for it.

## Implementation status as of 2026-11-27

`driver/mtl/mtl_app_profile.h` (the struct, the exe match, the override), read once by
`MtlInstance` at Direct3DCreate9 (`driver/mtl/mtl_device_stub.cpp`), which logs
`d9mt: app profile <name> (exe <file name>)`. Tested by `test/native/app_profile_test.cpp` and the
hash suite's `appswtor` variant (`scripts/run-hash-tests.sh`). The fields:
- `staticBuffersDirect`: static buffers mapped directly on every memory profile; its override
  `D9MT_STATIC_BUFFERS_DIRECT`, the `swtor-unlimited` variant;
- `inpassClear` (#129): a part-surface depth/stencil clear of the open pass's own depth-stencil
  is drawn inside that pass; its override `D9MT_INPASS_CLEAR`. The device carries the app profile
  (`MtlDevice::appProfile`) for the backend;
- `keepPassOverClearVisit`: an open pass stays open across a visit to other targets that only
  clears them; its override `D9MT_KEEP_PASS_OVER_CLEAR_VISIT`.
