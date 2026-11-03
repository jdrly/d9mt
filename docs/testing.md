# Testing

How d9mt is tested, and which script runs which test. The [README](../README.md#tests) has the
short version; the Definition of Done in [CONTRIBUTING.md](../CONTRIBUTING.md) says which of
these a pull request runs.

## Hash suite

The correctness gate of every pull request. It runs the D3D9 test programs `test/code_*.c` and
compares their frame hashes with the base commit's. Each program renders five frames, with these
exceptions:

- `code_sm3_ref` and `code_statecache` aren't part of the suite. Their own runners are
  `scripts/sm3_ref_test.py` and `scripts/run-statecache-bench.sh`.
- `code_readback` ends after one frame and `code_updatetex` after two, on every side.
- `code_mtlock` isn't deterministic. Its frames differ between two runs of the same commit
  because of thread timing, so a pull request compares it with an A/A run of base instead of
  requiring identical frames (#117, [gpu-4k.md](gpu-4k.md) §8). Its own runner is
  `scripts/run-hang-test.sh`.

The suite also runs with `MTL_DEBUG_LAYER=1`,
`D9MT_NO_ATTACHMENT_OPT=1` and `d3d9.reproducibleCommandStream=True`, with
`D9MT_EARLY_COMMIT=always` for a change to the submission path, and `D9MT_LIFETIME_CHECK=1`
must find no violation. Its runner, `scripts/run-hash-tests.sh`, is on the branch
`chore/test-runner` and not on `main` yet.

## Native unit tests

No Wine or GPU needed, from the repo root:

```bash
scripts/run-native-tests.sh   # the 15 host clang++ tests in test/native/; never starts Wine
scripts/test-upload-copy.sh   # UploadPerDrawData's own clamp and copy, under ASan/UBSan
```

`scripts/run-native-tests.sh` builds and runs every `test/native/*_test.cpp` that
needs only the host's clang++ (each source's first lines give its own command), and
names the runner of each of the other seven: `state_cache_test`
(`scripts/test-state-cache.sh`), `pso_key_hash_test` (`scripts/test-pso-key-hash.sh`),
`upload_copy_test` (`scripts/test-upload-copy.sh`), `fatal_log_test`
(`scripts/test-fatal-log.sh`, a mingw program it builds and runs under Wine), and `lifetime_check_test`,
`submission_retire_test` and `submission_tracker_test`, which are mingw programs run
by hand under Wine (the build line is in each source). A test in neither list fails
the run, and so does any failing or unbuildable test (exit status 1).

## Tests with their own runners

`scripts/test-state-cache.sh` runs the pipeline state cache tests (file
format, corruption, versions, cap, record → load → prewarm with a fake
backend), plain and under ThreadSanitizer, plus the render-thread cost of
recording; it needs DXVK's native headers like `test-compile-mt.sh`.
`scripts/run-statecache-bench.sh` runs `test/code_statecache.c`, a synthetic
SWTOR-like many-pipeline workload, under Wine with every cache in a scratch
directory.

`scripts/test-air-versions.sh` (after `scripts/build-bridge.sh`; needs Xcode's
metal toolchain and an LLVM 15 `llvm-dis`: `LLVM_DIS`, or
`ninja -C toolchains/llvm-build llvm-dis`) compiles the AIR corpus through
d9mtair for each AIR target (`D9MT_AIR_MACOS`, `D9MT_AIR_LEGACY`). Targets 26
and 27: the metallib container (file version, header OS, every VERS) and the
bitcode (triple, `air.version`, `air.language_version`, datalayout) must equal
what Apple's toolchain writes for that macOS (`xcrun metal
-mmacosx-version-min=<os>.0` and `metallib` on a probe shader; a newer VERS
makes Metal reject every game shader). Legacy 26 and 27: the fields 0.1.7
wrote, VERS 2.9 on 27 included. Unset gives this Mac's mode. With
`OLD_LOOPBACK=<build/airlib of a d9mt 73511be tree>` it also requires legacy
output byte-identical to that build's (the AIR target PR's acceptance check;
later codegen changes legacy bitcode on purpose).

`scripts/test-air-forms.sh` (same needs) checks the AIR built-in forms
contract: `air/tests/forms/inventory.txt` lists every intrinsic declaration
AirBuilder can emit and every `"air.…` literal in its source (a new emitter
without an entry fails); each form's fixture (`air/tests/forms/gen_ir.cpp`
IR through `airlib_loopback --ir`, or an SM3 corpus shader) must declare it,
with the same normalized signature Apple's compiler gives the
`forms.metal` group for it, for targets 26 and 27.

`scripts/test-air-cache-lineage.sh` (after `scripts/build-bridge.sh`) checks the
AIR cache's identity (ADR-0006) on scratch caches, without Wine: the AIR codegen
id ignores an object's SDK version stamp but not a byte of its code, a warm run
hits every entry, a foreign table is dropped, and, when the build's id has a
lineage that adopts a predecessor table, that table is adopted on macOS 27 with
every row kept and every compile a hit (also next to a stray table), merged into
a new table that already holds rows, left whole while another process holds the
write lock (that session runs without the cache) and adopted by the next start,
and dropped under `D9MT_AIR_MACOS=26`. The four AIR targets (26, 27, legacy
26, legacy 27) get four distinct cache terms and four tables, and 0.1.5.1's
table is dropped, not adopted.
`OLD_BRIDGE=<the predecessor's winemetal.so>` makes the predecessor table with
that bridge itself and compares it row by row first.

`scripts/test-pso-partition.sh` (after `scripts/build-bridge.sh`; no Wine) checks the
PSO archives' partition, `os-<build>-<gpu>-c<identity>-t<T>`: this build's
identity and AIR target, 0.1.7's and other identities' directories removed,
two starts keeping the directory, the same directory with `D9MT_AIR_CACHE=0`,
four targets in four directories, two live processes on one base keeping each
other's partitions (leases in `<base>/.locks`), a lineaged codegen id (a copy
of the bridge carrying 0.1.5.1's) keeping its predecessor's directory, a
malformed id removing nothing, and links (a linked partition refused, a linked
stale one unlinked itself, nothing behind a link touched). Every start's
harness must exit 0.

`scripts/run-air-diff.sh` follows `D9MT_AIR_MACOS` and `D9MT_AIR_LEGACY`: its
artifacts live under `build/air-compiler/diff/<target, math, invariance,
samples>`, and every AIR artifact's target fields are checked before it renders.

`scripts/sm3_hazard_test.py` checks the `D3D9 SM3 hazard` warning (shader
patterns dxbc-spirv's SM3 converter mishandles, GitHub issue #8) under Wine:
`ENGINE=<engine with this build's bridge> [FRAMEWORKS=<dir>] [WORK=<scratch dir>]
scripts/sm3_hazard_test.py build/driver-x64/d3d9.dll [<dir of .asm/.bin>...]`
(corpora from `scripts/sm3_corpus.py gen`). `scripts/sm3_ref_test.py` (same
environment) renders `crs`/`setp` with every write mask, modifier and
predicate and packed I/O declarations on the GPU and compares the results
with a scalar model of the D3D9 semantics; `--strict` uses
`d3d9.floatEmulation = Strict`, `--corpus <dir>` adds a differential test of
every corpus `crs` against a `mul`/`mul`/`add` expansion. Its `pin_*` cases
(`--only pin_`) cover the dxbc-spirv pin: unpacked relative input reads, the
assert paths of unreachable loops, the `if`/`breakp` predicate truth table
(with and without NOT) and the signed-zero witnesses of ConditionPass.
`conditions_signed_zero_test` (in `run-native-tests.sh`) checks the same
signed-zero case on the IR after `legalizeIr`.

`test/code_flat_select.c` draws with `D3DSHADE_FLAT` across the topologies
d9mt draws flat and the ones it keeps Gouraud (triangle fans), a pixel shader
without COLOR inputs and a VS/PS pair that needs semantic reconciliation. Its
frames must hash as base until the AIR compiler honours flat; with
`D9MT_FLAT_CHECK=1` its log shows each draw's flat choice and the final-IR
check of every pixel shader function (expected values in the file's header).
Flat shading is partial: triangle fans stay Gouraud (D3D9 takes fan triangle
t's flat values from vertex t+1, Metal would take the hub of the expanded
`(0, t+1, t+2)`; non-indexed fans follow once the fan index rotation lands,
#170), and points have nothing to interpolate.

`scripts/test-compile-mt.sh` runs the compile-path concurrency tests (with
ThreadSanitizer) against the built bridge.
`scripts/test-present-ring.sh` runs `present_ring_test` (the bridge's presented-time ring
behind the `SCREEN` line, `bridge/unix/present_ring.h`) under ThreadSanitizer; its plain build
runs in `scripts/run-native-tests.sh`. The scripts under `scripts/run-*`
and `scripts/dump-verify.sh` drive the D3D9 test programs in `test/` under a
CrossOver bottle (`CX` and `BOTTLE` must be set).

## Hang test

`scripts/run-hang-test.sh` is the profiling/debug regression test: it runs
`test/code_mtlock.c` (a render thread presenting frames while a second thread
locks managed textures, on a device without `D3DCREATE_MULTITHREADED`, as
SWTOR does) with `D9MT_PROFILE`, `D9MT_MEMSTATS`, `DXVK_LOG_LEVEL=info` and a
`D9MT_DEBUG_FILE` flags file, in a throwaway prefix, and fails if a run doesn't
finish within the timeout:

```bash
ENGINE=<Wine engine root> scripts/run-hang-test.sh --arch x64   # or --arch x86
```

## Benchmarks

The benchmarks are in `test/perf/` (bindbench, perdrawbench, churnbench,
psobench, bench_draws, bench_upload, bench_getdata, bench_fps_cap) and
`test/bench_4k.c`; `scripts/build-benches.sh` builds them, and
[PERFORMANCE.md](../PERFORMANCE.md) §3 describes each.
