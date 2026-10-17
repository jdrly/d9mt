# Draft: upstream report for dxbc-spirv (not sent)

Two bugs in dxbc-spirv's `sm3` front end, found in d9mt (GitHub issue #8),
present at the pin d9mt uses (c5c1a5b9) and at upstream HEAD (64f078d,
2026-10-16). Patches: `patches/dxbc-spirv/0001-*.patch` and `0002-*.patch`;
both apply to 64f078d with `git am`.

---

**Title:** sm3: crs/setp with partial write masks read uninitialized
memory; packed I/O declarations assert in dclIoVar

### 1. `crs` and `setp` with a write mask that isn't a prefix of .xyzw

```
ps_3_0
dcl_texcoord0 v0
mov r0, v0
mov r1, v0.yzxw
mov r2, v0
crs r2.xz, r0, r1        ; also .y, .z, .yz (legal SM3 masks)
mov oC0, r2
```

`Converter::handleCrs` and `Converter::handleSetP` push one value per
written component (packed, in write mask order) into a
`util::small_vector<ir::SsaDef, 4>`, then call
`composite(builder, type, components.data(), Swizzle::identity(), writeMask)`,
which indexes the array **by component**. For `.y` it reads `components[1]`
of a one-element list; for `.xz` it reads `components[2]` of a
two-element list. `small_vector`'s inline storage is `aligned_storage` and
uninitialized, except that the constructor's `u.m_ptr = nullptr` happens
to zero the first 8 bytes (elements 0 and 1). So, depending on stack
contents:

- `.y`: a null def: `Assertion failed: (scalar), function composite`
- `.z`, `.xz`, `.yz`: a stale def, often of a non-arithmetic op:
  `Assertion failed: (false), function normalizeTypeForConsume`, or
  **silently** any other SSA value. In one case the store of `r1.z` used
  the entry point function's def (`CompositeConstruct %364 %main`).
- `setp` the same for masks such as `.y`, `.xz`, `.yw`; with `.w` in a
  non-prefix mask element 3 is read uninitialized.

In a generated corpus of 3000 shaders, 820 use `crs r.yz`: 204 abort, 37
compile to wrong code that reaches an output, and 579 compile with the
wrong value in dead code (removed by legalization). With asserts compiled
out (DXVK release builds), the aborting cases also turn into undefined
behaviour.

**Fix (0001):** build the result with `buildVector()` from the packed list,
as `handleDst`, `handleLit`, `handleSinCos` etc. already do. For the masks
that worked (.x, .xy, .xyz and .xyzw for setp) the emitted IR is identical.
`crs` has no `.w` result; a mask containing `.w` is invalid D3D9, the patch
leaves `.w` unwritten instead of reading past the list.

### 2. Several semantics in one I/O register

```
ps_3_0
dcl_texcoord0 v0.xy
dcl_texcoord1 v0.zw      ; legal: "The same output register may be declared
mov oC0, v0              ;  multiple times, each time with a unique write mask"
```

`IoMap::dclIoVar` asserts `!foundExisting` on the second declaration of a
register (and on a second declaration of a semantic), for PS inputs, VS
inputs and VS outputs. The declaration's write mask is ignored entirely.
The ps_3_0 `dcl_usage` documentation has the packed example
(`dcl_normal v0.xyz` + `dcl_blendweight v0.w`, `dcl_texcoord1 v1.y` +
`dcl_texcoord0 v1.zw`) and states that a mask selects components of the
semantic at the same positions.

**Fix (0002):**
- Each semantic of a packed register gets its own variable (location from
  its semantic, as before) and keeps the declaration's component mask;
  `emitLoad`, `emitStore` and the relative-addressing switch functions pick
  the variable per component. A component no mask covers belongs to the
  nearest declaration below it, else above it, so a read past a declared
  mask still gets data (DXVK's dxso reads all four components of a PS
  input for the same reason).
- A semantic declared again (another mask of the same register, or another
  register), a special register declared twice, and an undeclared register
  whose implicit semantic (`determineSemanticForRegister`) is already
  declared map to the variable declared first instead of asserting.
- A point size output packed into a register's `.w` is flushed from that
  component.
- Registers with one declaration are converted exactly as before (IR
  byte-identical).

For comparison: wined3d links SM3 VS outputs to PS inputs per component
with the masks (`ps_in[r].zw = vs_out[o].zw`), which is the model the
patch follows. DXVK 2.x's dxso stored every element's full vec4 into the
register, so with packing the last declaration overwrote the others, and
it moved masked VS outputs to the front of the varying (`o1.zw` became
`out.xy`) while PS inputs kept their positions.

### Testing

In d9mt, on an Apple GPU through the whole driver (d3d9.dll + AIR backend):
2041 rendered cases compared against a scalar model of the D3D9 semantics
(every crs/setp write mask, VS and PS, source modifiers and swizzles,
saturate, `_pp`, predication incl. `!p0` and replicate swizzles, IEEE and
legacy multiplies; packed PS inputs, VS outputs and VS inputs incl.
relative addressing): all match, and the 419 of them that use masks the
unpatched converter handles match on the unpatched build as well. Every
crs of 4 generated corpora (8290 shaders) gives bit-identical results to
the same shader with the crs expanded to mul/mul/add. 13156 generated and
test shaders: no aborts (464 before), and the 11487 without these
patterns give byte-identical IR, AIR and reflection.

---

Note for whoever sends it: the d9mt side (vertex input signature: one
attribute per semantic) is in `driver/d3d9/d3d9_shader_analysis.cpp`. d9mt's front end is extracted
from DXVK's d3d9 (3.0), so DXVK most likely needs the same one-line change
if it adopts 0002: check its D3D9ShaderAnalysis::HandleDcl first.
