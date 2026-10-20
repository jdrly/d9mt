# D3DSAMP_MIPMAPLODBIAS (and d3d9.samplerLodBias) do nothing: design

Status: design (2026-10-20), for its own review before any code. Branch `fix/sampler-lod-bias`
(base: `fix/sampler-dss-cache-keys`, which keys the sampler cache by the whole state, bias
included). Found by 0a2ecf2d in the review of that branch.

## 1. The bug

- The frontend builds the sampler key with the bias: `mipLodBias + d3d9.samplerLodBias`,
  clamped to ≥ 0 with `d3d9.clampNegativeLodBias` (`d3d9_device.cpp:7042-7048`, `BindSampler`).
- The backend never passes it on. `MtlDeviceImpl::createSampler` (`mtl_device_impl.cpp:485-499`)
  fills `WMTSamplerInfo`, which has no bias field (`bridge/winemetal.h:267-285`), and the bridge
  (`_MTLDevice_newSamplerState`, `winemetal_unix.c:1800`) doesn't set one.
- So `D3DSAMP_MIPMAPLODBIAS`, and the `d3d9.samplerLodBias` config option, have no effect,
  while the cap `D3DPRASTERCAPS_MIPMAPLODBIAS` is advertised (`d3d9_adapter.cpp:395`).
- Games use a negative bias for sharper distant textures, a positive one for softer or
  cheaper ones. Some use bias tricks on shadow maps or UI.
- What isn't affected: SM3 `texldb` (a per-pixel bias from the coordinate's w) already
  reaches Metal. The SM3 converter passes it as `lodBias` (`sm3_converter.cpp:1015`), and the
  AIR builder sends it as the sample's bias operand (`air_builder.cpp:1831-1833`). Every
  implicit-LOD sample already carries a bias operand, 0.0 by default.

## 2. First: does SWTOR set it? (measure before building)

No trace of SWTOR's sampler states exists (the session logs don't record them, and there's no
apitrace capture of the game).
- **Diagnostic, zero cost:** in `SetSamplerState`, inside the existing
  `if (unlikely(Type == D3DSAMP_MIPMAPLODBIAS))` branch (`d3d9_device.cpp:4474`): for a value
  that isn't a FETCH4 magic, log the first 4 distinct values once each, with the sampler index
  ("D9MT: the game sets D3DSAMP_MIPMAPLODBIAS = -0.5 on sampler 0"). Nothing else on any path.
- This goes into the next build Jan plays, alone or with this branch. Its log answers the
  question in one session (load + world + a warzone).

## 3. Options

| | how | cost when bias = 0 | cost when ≠ 0 | shaders/PSOs/state cache | bridge |
|---|---|---|---|---|---|
| **A (proposed)** | `MTLSamplerDescriptor.lodBias` (macOS 26+, d9mt's minimum; S4.6 fixed point, range [-16, 15.999]): the sampler applies it | none | none (a different sampler object) | unchanged | **yes**: `WMTSamplerInfo` grows |
| B | IR pass: every implicit-LOD sample adds a per-sampler bias from the SpecData uniform buffer, gated by a spec mask | a load and a select per sample in every shader; the IR of every shader changes, so every AIR cache entry rebuilds once | as when 0 | every shader changes once | no |
| C | PS variants: a bias mask/values in the PS key, the bias baked in as an immediate | none (the variant = today's function) | a new PS function per (shader, bias set): AIR compile + PSOs | new variant identity; the state cache's shader identity needs a variant field (format change) | no |

**Option A** is the only one with no cost and no shader-side change. It is the Metal-native
equivalent of D3D9's sampler-state bias, which D3D9 also defines per sampler.
- Semantics: D3D9 adds the sampler bias to the shader's bias (`texldb`) and to the computed
  LOD. Metal's sampler bias adds to the LOD the same way, and a sample with an explicit
  `bias()` still gets the sampler's on top. To confirm on hardware: §5 test 3.
- Precision: S4.6 is 1/64 LOD steps. D3D9 doesn't specify the precision; DXVK passes the float
  to Vulkan's `mipLodBias`, whose precision is implementation-defined (typically 1/256). Values
  outside [-16, 15.999] are clamped (D3D9 has no range; games use −3…+3).
- `d3d9.clampNegativeLodBias` and `d3d9.samplerLodBias` keep working as today: they shape the
  value in the key.

## 4. Option A, concretely

- **Bridge** (part of the bridge bundle, #11):
  - `struct WMTSamplerInfo` gets `float lod_bias`. The struct has a single free byte
    (offset 23), so it grows to 40 bytes, with `lod_bias` at 32 and padding after it; the
    STATIC_ASSERT becomes 40.
  - `_MTLDevice_newSamplerState` sets `sampler_desc.lodBias = info->lod_bias` (macOS 26 API,
    within d9mt's minimum, no `@available` needed; guard it anyway if the bridge still builds
    against older SDKs).
  - It lands in the bridge bundle after S1 (the AIR cache key decoupled from `winemetal.so`'s
    LC_UUID), so it costs no shader-cache rebuild.
  - The PE side (`winemetal.dll` thunk) takes the struct by pointer, so only the size changes.
  - **Version skew (M-1, decided):**
    - the former padding byte at offset 23 becomes `lod_bias_present`;
    - `lod_bias` sits at 32, the struct is 40 bytes;
    - callers must zero the struct (the only caller does: `WMTSamplerInfo samplerInfo = {}`).

    So an old d3d9.dll sends 0 at offset 23 and the bridge never reads past its 32 bytes. A new
    d3d9.dll with an old bridge: the old bridge ignores the flag; `gpu_resource_id` stays at
    offset 24 either way.
- **Backend:** `createSampler`: `samplerInfo.lod_bias = key.mipLodBias;` (clamped to
  [-16, 15.999]).
- **Frontend:** unchanged, the key already holds the bias.
- **Sampler cache:** fix/sampler-dss-cache-keys keys it by the whole state (`mipLodBias`
  included, compared field by field), so samplers that differ only in bias are separate
  objects. In base, the hash covered the bias bytes too.
- **Per-draw cost:** none. The bias is part of the sampler state, which is created once per
  distinct key and cached.
- **The sampler limit (M-2):**
  - Every distinct bias is its own sampler, the cache never evicts, and argument buffers
    reference at most `maxArgumentBufferSamplerCount` samplers.
  - So the frontend quantises the bias in the key to Metal's S4.6: round(b × 64) / 64, clamped
    to [-16, 1023/64]. That also folds -0 into 0 and turns NaN into 0.
  - DXVK keys its bias at 1/256 (`encodeFixed<int32_t, 6, 8>`), finer than Metal can apply,
    so there's no coarser upstream rule to match.
  - `createSampler` logs at 256/512/768/1000 distinct samplers. If a biased sampler fails, it
    falls back to bias 0 with a log line; never a silent null.
- Prior art: dxmt (D3D11 on Metal) applies `MipLODBias` shader-side through the argument
  buffer (`dxmt_context.cpp:285`), because it predates `lodBias`.
- **Version skew:** d3d9.dll and the bridge always ship together in the app bundle. A
  development d3d9.dll with an old bridge would pass a 40-byte struct to a 32-byte reader; the
  unix side reads by pointer, so it would ignore the bias and not crash. The reverse (an old
  d3d9.dll, a new bridge) would read 8 bytes past a 32-byte struct. So the bridge side must
  check a struct version or size: `WMTSamplerInfo.struct_size`, or keep the old layout and
  append. **Decide with the bridge lane.**

## 5. Tests

1. **`test/code_lodbias.c`** (a new hash test): a 64×64 texture with 7 mips, each mip a
   different solid colour, drawn on 32×32 and 8×8 quads:
   - with `MIPMAPLODBIAS` 0, +1, +2, −1 (as float bits): the centre colour must be the
     expected mip's (a check line, not only the hash);
   - with `d3d9.samplerLodBias` via `DXVK_CONFIG` once (+1).

   Base fails the non-zero cases (they show mip 0's level); the fix passes.
2. Hash suite: the 33 programs identical to base (none sets a bias).
3. Review additions (in code_lodbias):
   - T-2: `texldl` LOD 1 + sampler bias +1. Reported (`INFO … -> mip N`), not checked. Vulkan
     (DXVK) adds the bias, giving mip 2; if Metal gives mip 1, that's documented, no IR fix;
   - T-4: MAXMIPLEVEL 2 + bias −2 must stay at mip 2;
   - T-5: 200 bias values in 1/64 steps, so 200 sampler states. Every quad must be drawn, and
     no "createSampler failed".
4. `texldb` + sampler bias together (SM3 `ps_3_0`: texldb with w = 1 and sampler bias = 1 → 2
   levels): confirms Metal adds them. If Metal's sampler bias *replaced* the shader's, option A
   would need an IR-side add for texldb shaders only (still zero cost elsewhere).
5. `MTL_DEBUG_LAYER=1` on test 1.

## 6. Order
1. The diagnostic (§2) now, in any build Jan plays next.
2. If SWTOR sets a bias, or for correctness regardless: option A in the bridge bundle, after
   S1, together with the backend line and the tests.
3. If the bridge bundle can't take it soon: option C as a stopgap only for shaders sampled
   with bias ≠ 0. Not B, because B costs every shader.
