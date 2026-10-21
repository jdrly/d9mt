# perf/pso-key-hash: merge notes

From the review (review-014/pso-key-hash-design.md, 0a2ecf2d: b20ea6a MERGE).

## Order

On top of fix/pso-key-normalize (64f3269, its base): this branch needs its explicit padding and
its `has_unique_object_representations` assert.

## Against perf/pso-mru (ba400ac): one conflict, `driver/backend/mtl_pso_key.h`

pso-mru's 329b7d2 also changed `RenderPsoKey::hash()`: a single-lane word-wise hash (40
dependent multiply/xorshift steps). Keep **this branch's** body:

- `psokey::hashWords` (4 lanes) in `hash()`, and the `namespace psokey` block above the struct;
- both asserts after the struct: `has_unique_object_representations_v<RenderPsoKey>` and
  `sizeof(RenderPsoKey) % 8u == 0u`;
- drop 329b7d2's single-lane body and its in-function `static_assert`.

`mtl_context_transfer.cpp` merges without a conflict (pso-key-normalize's `snap.topoClass = 0u`
next to pso-mru's interning). Done that way in n013-perf `tmp/psomru-key-hash` 9d45bc9: 0
warnings, hash suite 33/33 = base.

Measured (n013-perf window 8, results/w8-summary.txt): under Wine/Rosetta, one lookup costs
313.9 ns with the byte-wise FNV, 76.8 ns single-lane, 49.0 ns 4-lane. psobench blend: −47%
against pso-key-normalize alone; nothing slower.

## After merging

`scripts/test-pso-key-hash.sh` (avalanche; with state files for real keys) and the hash suite.
