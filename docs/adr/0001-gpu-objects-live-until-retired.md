---
status: accepted
date: 2026-10-21
deciders: Jan Drlý
---

# Metal objects live until the GPU has retired the last command buffer that uses them

## Context

Metal 4 command buffers don't retain the resources they use. d9mt released a texture's old
sampled view on a rebind while a recording or committed command buffer still referred to its
resource ID. The ID was reused (the GPU read the wrong texture) or the GPU page-faulted and the
queue hung: pink and black-dotted artifacts through SWTOR's Cartel Market overlay, and GPU
freezes (#23, #24). The present path also let go early of the drawable's texture, d9mt's
reference to the drawable and the layer's residency set, and relied on Core Animation to keep
them: a player's pink textures at loading screens and a GPU fault at the first presents after a
display power event point there (#26).

## Decision

- Nothing a recorded or committed command buffer refers to is released before the GPU is past
  that command buffer's serial. Releases are deferred with `retireAfter(nextSerial())` and run in
  serial order on the `d9mt-retire` thread or at the end of a frame on the CS thread, never both at
  once (#24; `driver/backend/mtl_submission.h`).
- Texture views follow that pattern (#24); so do the drawable, d9mt's reference to it, the layer's
  residency set and MetalFX scalers at teardown (#26).
- `D9MT_LIFETIME_CHECK` checks the rule deterministically, on any Mac, without needing the GPU to
  fault (#25; `driver/backend/mtl_lifetime.h`). Metal's own
  `MTL_DEBUG_LAYER_VALIDATE_UNRETAINED_RESOURCES` doesn't catch this class of bug (#25).

## Consequences

- Objects live until their serial retires instead of until the next rebind. _Open:_ the effect
  on peak memory has not been measured.
- Every new owner of a Metal object needs the deferral; a review finding that something can be
  released early is a `Blocker` (CONTRIBUTING).
- Evidence for #26: GPU faults in 8 of 150 starts on main against 0 of 150 with the change
  (Fisher p = 0.007); 115 in-flight drawable releases in the hash suite under the lifetime check
  against 0. #24: `code_view_lifetime` fails 6/6 on base and passes 6/6, with no bench slower.

## Alternatives considered

- **Keep views alive with a push per bind**: rejected, about 6.6 % slower on the bind path, which
  violates ADR-0002.
- **Release the views on the CS thread** (an earlier variant, `ac70a8c`, through the residency
  path): rejected, 1.3–1.5 % slower in churnbench; #24 hands them to the retire queue instead and
  was neutral.
- **Rely on Metal's validation layer to find the bug**: it doesn't catch it (#25), hence the
  lifetime check.

## Open questions

- #26 measured −2.0 % fps with vsync on, not shown to be significant; a narrower variant is being
  looked at.
- One `GpuLifetime` owner that replaces the separate retire queues is not done.
