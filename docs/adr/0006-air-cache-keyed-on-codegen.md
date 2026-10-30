---
status: accepted
date: 2026-10-30
deciders: Jan Drlý
---

# The AIR cache is keyed on the AIR codegen, not on `winemetal.so`, and keeps 0.1.5.1's metallibs

## Context

Until now the AIR cache's identity was `winemetal.so`'s LC_UUID (ADR-0004). The linker derives it
from the whole library, so any bridge change (a Metal thunk, a log line) and even a new Xcode with
no code change at all gave every player a cold AIR cache, and the table of the previous build was
dropped on the first start:

- 0.1.5.1's bridge (d9mt f6cfa71, LC_UUID 2BBB585C-5076-3157-9A6A-BAE494B00ACC) was linked with
  Xcode 27.0. Rebuilt from the same commit with Xcode 27.2, every section is byte-identical
  (`__text` 8,976,800 bytes, `__const`, `__cstring`, …) and the LC_UUID isn't: only the SDK
  version stamps and the symbol table differ.
- Between 0.1.5.1 and 0.1.6 only #110 (the VERS tag per macOS) and #114 (the PSO archive flush
  thread) touched the bridge. On macOS 27 the metallibs and reflection blobs of 0.1.6's codegen are
  byte-identical to 0.1.5.1's, but the new LC_UUID would still have reset every player's cache.

Jan's requirement for 0.1.6: players keep their shader caches; a release must not come with
“your shaders recompile once”.

## Decision

- **The AIR codegen id is the cache's identity.** `scripts/build-bridge.sh` links the AIR codegen
  from one array, `CODEGEN_OBJS`: the airlib objects (AirBuilder, the metallib writer, dxbc-spirv's
  IR and util code), `airlib/unix.cpp` (the compile entry and the reflection blob), the AIR cache
  in front of it (`bridge/unix/air_cache.c`: the identity, the key and the blob layout; and the
  sqlite store `bridge/unix/cache.c`), and the LLVM archives. `scripts/air-codegen-id.py` hashes
  exactly that array, each file normalized: sections, relocations, symbols and the deployment
  target, without the SDK and tool version stamps and the ar headers. The id is compiled into its
  own object; the build reads it back out of `winemetal.so` and fails if it differs.
- **What is outside it can't change a cached result:** `winemetal_unix.c` (every Metal thunk), the
  PE side, the lineage table and the id's own object. A change there keeps every player's AIR
  cache. Any byte of code inside it gives a new id and a cold cache, never a stale one; so does
  any file `air-codegen-id.py` doesn't recognize.
- **Table and key.** The cache identity is the id's first 16 bytes, or the lineage the id is mapped
  to (below). The table is `cache_<(identity's first 8 bytes) ^ (D9MT_AIR_ABI_VERSION << 56) ^
  (VERS AIR version << 24)>`, the entry key SHA-256(identity, ABI, flags, spec data, entry, IR),
  as before with the identity in the LC_UUID's place. On open, every other `cache_<n>` table is
  dropped.
- **Lineages.** `bridge/unix/air_cache_lineage.c` maps an exact codegen id to an earlier build's
  cache identity, so that build's entries keep hitting under their keys. An entry needs evidence
  that the id's codegen writes byte-identical metallibs and reflection to the build it continues,
  for every input; it holds only for that id. It may also name a predecessor table to adopt once,
  with the VERS tag under which the two builds agree.
- **0.1.5.1's cache is adopted on macOS 27 and later.** The one entry maps 0.1.6's codegen id to
  0.1.5.1's LC_UUID bytes and adopts 0.1.5.1's table (`cache_6138817854601476907`) when this
  process tags VERS AIR 2.9. In one sqlite write transaction, before the reader and writer open,
  that table is renamed to the new one, or merged into it if the new one already holds rows (the
  rows there win). Before macOS 27 the tag is 2.7 (#110): 0.1.5.1's metallibs say 2.9, which is
  the macOS 26 black screen, so its table is dropped there and the cache starts cold.
- **No cache for a session whose open fails.** If that transaction can't run (another process
  holds the write lock past the 2 s busy timeout, a damaged file), the session compiles without
  the AIR cache and writes no table, so a predecessor's table stays whole for the next start.
- **Carried over from ADR-0004:** the driver and the bridge are built, tested and shipped from the
  same commit (there is no version handshake); a change to the pipeline state cache's key
  signature resets every player's state cache and is batched into one release.

## Consequences

- A bridge change no longer costs players a recompile unless it changes the AIR codegen; batching
  bridge changes per release (ADR-0004) is no longer needed for the AIR cache.
- A PR that changes the codegen says so: its id changes and every player recompiles once, unless
  the PR adds a lineage entry with the byte-identity evidence. `build/bridge/air-codegen-id.txt`
  and the build's last line name the id and its lineage. A release whose build ends up with a
  different id (another Xcode or LLVM that changes the code) starts cold: release builds check
  that line.
- A new Xcode that changes the generated code of the codegen or of LLVM gives a cold cache, even
  when the metallibs would be identical. That's the price of failing closed; a lineage entry
  backed by the corpus comparison can carry such a build over.
- A lineage entry is a claim checked by evidence, not by the build: a wrong entry would serve
  another codegen's metallibs. Each one is reviewed with its evidence and names it.
- The cache glue moved from `winemetal_unix.c` to `air_cache.c`, so this change itself gives
  0.1.6 a new LC_UUID; the lineage keeps the caches anyway.
- The id script reads about 100 MB of LLVM archives on every bridge build (under a second here).

## Alternatives considered

- **Keep the LC_UUID (ADR-0004).** Every release with any bridge change, and any rebuild with a new
  Xcode, resets every player's cache, including 0.1.6.
- **S1 (branch `bundle/bridge-s1-pso`): the raw bytes of the codegen objects.** The same object
  list, but object files carry the SDK version, so every Xcode update would still reset the cache
  with not one byte of code changed (the 27.0 → 27.2 case above).
- **A hash of the sources plus the toolchain's version strings.** Path-independent, but it needs a
  hand-kept list of every header and flag that matters, and leaving one out serves stale
  metallibs; it also resets on every Xcode update.
- **Migrate 0.1.5.1's entries lazily** (look up a miss in the old table under the old key, copy it
  over). It costs a second lookup on every miss, writes every entry again and keeps two tables;
  a rename does it once, in one transaction.

## Open questions

- The app's release build (swtor-on-mac `scripts/build-d9mt.sh`) doesn't check
  `air-codegen-id.txt` yet.

## Implementation status as of 2026-10-30

`scripts/air-codegen-id.py`, `scripts/build-bridge.sh` (`CODEGEN_OBJS`, the id's read-back
check), `bridge/unix/air_cache.c`, `bridge/unix/air_cache_lineage.c`, `bridge/unix/cache.c`
(`d9mt_cache_prepare`), `scripts/test-air-cache-lineage.sh`. The byte-identity evidence for
the 0.1.5.1 entry is in the PR that added it.
