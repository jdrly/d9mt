---
status: accepted
date: 2026-10-21
deciders: Jan Drlý
---

# Changes to winemetal.so are batched, because its LC_UUID keys every player's AIR cache

## Context

The AIR cache's key includes `winemetal.so`'s LC_UUID, which the linker derives from the linked
contents, so every rebuild of the bridge changes it (`air_build_identity` in
`bridge/unix/winemetal_unix.c`). After any bridge change, every player recompiles all shaders
once. The driver and the bridge also have no version handshake: mixing them from different
builds can crash (README, *Known issues*).

## Decision

- A change that doesn't need the bridge doesn't touch it, and its PR states that `winemetal.so`
  is byte-identical to `main`'s.
- Bridge changes are collected into one bundle per release, so players recompile once per release
  at most (`docs/state-cache.md` §9 lists the follow-ups “worth bundling into the next
  winemetal.so change”).
- The same applies to the pipeline state cache's key signature: a bump resets every player's
  state cache and ships with the bridge bundle.
- The driver and the bridge are always built, tested and shipped together.

## Consequences

- Features are designed to stay in `d3d9.dll` where they can (the state cache and its journal,
  ADR-0003).
- A fix that needs the bridge may wait for the next bundle.
- Apps that ship d9mt carry `winemetal.so` and `d3d9.dll` from the same commit.

## Alternatives considered

- **Change the bridge whenever a PR needs it**: every such release would cost players a full
  shader recompile.
- **A sqlite journal for the state cache through the bridge**: rejected for this reason
  (`docs/state-cache-journal.md` §8).

## Open questions

- A key for the AIR cache that doesn't depend on the LC_UUID (“S1”) is on the unmerged branch
  `bundle/bridge-s1-pso`; it would change this decision.
- A version handshake between driver and bridge doesn't exist.
