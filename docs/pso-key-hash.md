# The PSO key's hash: word-wise instead of byte-wise (design)

Status: design (n013-perf, 2026-10-21), for review before any code. Branch `perf/pso-key-hash`,
off `fix/pso-key-normalize` 64f3269, because both change `driver/backend/mtl_pso_key.h` and
pso-key-normalize merges first.

## 1. The cost

- `RenderPsoKey::hash()` (`mtl_pso_key.h:70`) is FNV-1a over the key's 320 bytes, one byte per
  step: 320 dependent xor + multiply steps, ~1300 cycles.
- Measured (n013-perf `bench/psokey_probe.cpp`, the real key and the PSO cache's container,
  mingw libstdc++, under Wine/Rosetta, window 6): **~305 ns per cache lookup**. The 320-byte
  memcmp of the found entry and the bucket walk are a small part of it.
- When it runs: every PSO resolve that misses the PSO MRU snapshot (pso-mru) builds the key
  and calls `RenderPsoCache::get`. That compares against the last key (memcmp), then
  `m_cache.find(key)` hashes it. A full miss (a new pipeline) hashes it again in
  `Registry::adoptOrInsert`.
- In psobench "blend" (3000 MRU misses per frame) that's ~0.9 ms of 1.72 ms per frame. In SWTOR
  it scales with the PSO changes that the MRU doesn't catch.

## 2. What the hash is (and isn't) part of

- **Only in memory:** `KeyHash` for `RenderPsoCache::m_cache` and `Registry::m_map`
  (`mtl_pso_cache.h:50-72`). That's its only user on main, on pso-key-normalize, on pso-mru
  and on the bridge bundle (25b6bf4). `MtlDeviceImpl::createSampler`'s `key.hash()` is
  `MtlSamplerKey`'s, not this one.
- **Not part of the state-cache file or its signature.** The file stores the key's state bytes
  (`stateBytes()`, from `StateOffset`). `RenderPsoKey::signature()` is built from field offsets,
  sizes and `kPsoKeyMappingVersion`, not from the hash. Entry identities use the state cache's
  own `identityHash` (FNV in `mtl_state_cache_core.cpp`, over the entry, not this function). So
  **no signature bump and no cache reset**. To confirm in the implementation:
  `state_cache_test`'s signature test is unchanged, and the hash suite with the state cache on
  loads an existing file with `status=ok`.
- Not used by the PSO archive or the AIR cache code either: no other caller of
  `RenderPsoKey::hash()` on any of the four branches (grep).

## 3. The replacement

`RenderPsoKey` is a fixed-size POD with no padding (`has_unique_object_representations`, 64f3269)
and 320 = 40 × 8 bytes. So it can be hashed as 40 little-endian `uint64_t` words (`memcpy`
loads, no alignment assumption):

- **4 independent lanes**, xxHash64's round: `acc = rotl(acc + w * P2, 31) * P1`, 32 bytes per
  iteration, 10 iterations. The lanes don't depend on each other, so the CPU overlaps them:
  ~10 multiply latencies deep instead of 320.
- Then xxHash64's merge of the 4 lanes and its final avalanche (`h ^= h >> 33; h *= P2; ...`).
- ~40–60 cycles instead of ~1300. Expected per lookup: ~305 → ~40 ns. Measured in the
  implementation with the probe.
- Why xxHash64's round and not a homemade one: a well-studied mix (SMHasher clean) at the same
  cost. It's a ~25-line function, not a dependency. wyhash's `mum` (128-bit multiply) would be a
  bit faster natively, but under Rosetta a 64×64→128 multiply is two ARM instructions plus moves,
  and it gains little at 320 bytes.
- `hash()` keeps its signature (`size_t`), so `KeyHash` and the containers are untouched. Only
  the function body changes.

## 4. Quality check (before the perf numbers)

In `test/native/pso_key_hash_test.cpp`, native:
1. **Avalanche**: for 10,000 random keys, flip each of the 2560 input bits. Every output bit
   must flip with probability within 0.5 ± 0.02 (xxHash's own criterion).
2. **Real keys, collisions and buckets**: the keys of the 33 hash-suite programs, plus
   sc_session's and psobench's, recorded by a hash-suite run with the state cache on (my own
   prefix). The cookies (vs/fs) are filled from the entries' shader SHA-1s, which is what
   distinguishes them in the cache. Pass:
   - 0 full 64-bit collisions;
   - bucket counts for `bucket_count()` of 13, 97, 1031 within a chi-square bound of the
     ideal (no worse than the current FNV's on the same keys).
3. **SWTOR's own keys** would be the best set. They're in Jan's state-cache file
   (`~/Library/Caches/d9mt/state/swtor.exe.d9mtstate`). I'd read only a copy, and only with
   Jan's go (the coordinator decides). Otherwise item 2 is the set.

## 5. pso-mru

Independent and additive:
- pso-mru cuts how often a resolve gets to `RenderPsoCache::get` (a snapshot hit skips the key
  build and the lookup);
- this cuts what each remaining lookup costs.
- With both, the misses that remain (state the 4-entry MRU doesn't hold, e.g. blend
  generations) get the cheaper hash.
- Nothing in pso-mru uses `RenderPsoKey::hash()`. Its snapshot compare is a memcmp of the
  snapshot struct.

## 6. Measurement (a short window, ABBA)

- psokey_probe: ns per lookup, old vs new (one binary, both functions), apart and shared bucket.
- psobench blend / blend2 / blend4 / topo and bindbench, 20 rounds:
  - `14-psokey` (base) vs `14-psokeyhash`, with `14-psokeycopy` as the control;
  - plus `14-psokeyhash-a64` vs `14-psokey-a64`, the layout control from window 6.
- With pso-mru: `14-psomru` vs `14-psomru+hash` on blend and bind.
- Correctness: the hash suite (33 programs, frame-identical to base), and state_cache_test
  plain + TSan.
- Expected: blend −30…45% (from ~0.9 ms of hashing per 3000 misses), topo/bind smaller (more MRU
  hits). Hard rule: no scenario slower. A case where the old hash was cheaper doesn't exist
  (the new one does strictly fewer dependent operations), but the window decides.
