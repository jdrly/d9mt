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
scripts/run-native-tests.sh   # the 13 host clang++ tests in test/native/; never starts Wine
scripts/test-upload-copy.sh   # UploadPerDrawData's own clamp and copy, under ASan/UBSan
```

`scripts/run-native-tests.sh` builds and runs every `test/native/*_test.cpp` that
needs only the host's clang++ (each source's first lines give its own command), and
names the runner of each of the other six: `state_cache_test`
(`scripts/test-state-cache.sh`), `pso_key_hash_test` (`scripts/test-pso-key-hash.sh`),
`upload_copy_test` (`scripts/test-upload-copy.sh`), and `lifetime_check_test`,
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
metal toolchain) compiles the AIR corpus through d9mtair for macOS 26, for
macOS 27 and for this Mac (`D9MT_AIR_MACOS`). For macOS 26 each runtime
metallib's OS version and VERS tags must equal what Apple's `metallib` writes
for the same bitcode (a newer VERS makes Metal reject every game shader); for
macOS 27 they keep AIR 2.9, and nothing else differs.

`scripts/test-air-cache-lineage.sh` (after `scripts/build-bridge.sh`) checks the
AIR cache's identity (ADR-0006) on scratch caches, without Wine: the AIR codegen
id ignores an object's SDK version stamp but not a byte of its code, a warm run
hits every entry, a foreign table is dropped, and, when the build's id has a
lineage that adopts a predecessor table, that table is adopted on macOS 27 with
every row kept and every compile a hit, and dropped under `D9MT_AIR_MACOS=26`.
`OLD_BRIDGE=<the predecessor's winemetal.so>` makes the predecessor table with
that bridge itself and compares it row by row first.

`scripts/sm3_hazard_test.py` checks the `D3D9 SM3 hazard` warning (shader
patterns dxbc-spirv's SM3 converter mishandles, GitHub issue #8) under Wine:
`ENGINE=<engine with this build's bridge> [FRAMEWORKS=<dir>] [WORK=<scratch dir>]
scripts/sm3_hazard_test.py build/driver-x64/d3d9.dll [<dir of .asm/.bin>...]`
(corpora from `scripts/sm3_corpus.py gen`). `scripts/sm3_ref_test.py` (same
environment) renders `crs`/`setp` with every write mask, modifier and
predicate and packed I/O declarations on the GPU and compares the results
with a scalar model of the D3D9 semantics; `--strict` uses
`d3d9.floatEmulation = Strict`, `--corpus <dir>` adds a differential test of
every corpus `crs` against a `mul`/`mul`/`add` expansion.

`scripts/test-compile-mt.sh` runs the compile-path concurrency tests (with
ThreadSanitizer) against the built bridge. The scripts under `scripts/run-*`
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
