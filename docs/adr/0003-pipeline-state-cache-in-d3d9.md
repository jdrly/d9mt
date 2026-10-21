---
status: accepted
date: 2026-10-21
deciders: Jan Drlý
---

# A pipeline state cache in d3d9.dll rebuilds the game's pipelines in the background, with a journal for the newest records

## Context

A Metal pipeline that isn't in a PSO archive is built at the draw that first needs it, 20–220 ms
each. After a bridge update that changes the AIR bytes, a macOS update (its archive directory is
wiped) or archive eviction (content last seen more than eight sessions ago), every such pipeline
is cold again: an estimated 5–15 s of render-thread waits spread over the first minutes
(`docs/state-cache.md` §1.3; issue #10). DXVK dropped its own state cache in 2.7 because
`VK_EXT_graphics_pipeline_library` compiles each shader on its own; Metal 4's flexible pipelines
only cover the fragment output, so the vertex and pixel shader pair, the vertex descriptor and
other state are still fixed at pipeline creation, which in D3D9 is only known at the draw
(`docs/state-cache.md` §10).

## Decision

- d9mt records the pipelines a game uses (stable shader identities plus the derived
  `RenderPsoKey` bytes) and rebuilds them on low-priority prewarm threads in later sessions, as
  soon as the game has created their shaders, through the same compile path as a draw
  (`docs/state-cache.md`; README).
- It lives entirely in `d3d9.dll`: `winemetal.so` stays byte-identical (`docs/state-cache.md` §8;
  ADR-0004).
- The file is written atomically (temp file and rename), capped (1024 pipelines per game by
  default), has fixed-size entries, and a key signature drops stale entries after a mapping
  change (`docs/state-cache.md` §2, §10).
- Records made since the last write go to an append-only journal, `<file>.journal`, so they
  survive a kill or `ExitProcess`; a write compacts it (`docs/state-cache-journal.md`).

## Consequences

- The cold-pipeline cost after an update moves from the game's frames into the background during
  loading; in a warm session the gain is small (about 0.1–0.2 s) (`docs/state-cache.md` §1.3).
- A stale entry is never a correctness risk: the draw always looks a pipeline up by its live key,
  so a stale entry only costs a wasted background build (`docs/state-cache.md` §2).
- A key signature bump resets every player's state cache, so bumps are called out and batched
  (CONTRIBUTING).
- Every prewarmed pipeline is also captured into each session's PSO archive (about 45 KB each)
  (README, `D9MT_STATE_CACHE_MAX`).
- A crash during prewarm must not repeat every session: the load drops the entries that were in
  flight (crash-loop guard, `docs/state-cache.md` §12).

## Alternatives considered

- **Store the raw D3D9 state and derive the key again**: rejected; it duplicates the CS thread's
  derivation outside it for little gain (`docs/state-cache.md` §2).
- **No state cache, as DXVK 2.7+**: its reason (GPL-style separate compilation) doesn't hold on
  Metal 4 (§10).
- For the journal (`docs/state-cache-journal.md` §8): **writing the main file more often**
  (cost grows with the cache, the window only shrinks); **an exit hook** (after `ExitProcess` the
  other threads are dead, possibly holding Wine's locks); **sqlite** (a second database on the PE
  side or a bridge call, which changes `winemetal.so`'s LC_UUID).
