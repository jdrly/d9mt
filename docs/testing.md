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
must find no violation.

Its runner is `scripts/run-hash-tests.sh`. It needs a Mac with Metal 4 and an x86_64 Wine
engine (the one the app installs, `cx-26.3.0-8` from `jdrly/wine-engine`):

```bash
ENGINE=<engine> SRC=<a built checkout of base> WORK=<scratch>/base scripts/run-hash-tests.sh
ENGINE=<engine> WORK=<scratch>/head scripts/run-hash-tests.sh    # this tree, after its builds
scripts/cmp-hash-runs.py <scratch>/base/hashes.txt <scratch>/head/hashes.txt \
  --stats <scratch>/base/stats.txt <scratch>/head/stats.txt
```

Both runs go through this tree's runner and harness; `SRC` names the tree whose programs and
builds are tested, so a base from before the runner existed works too.

- `VARIANT=debug|noattach|repro|lifetime|shaderval|early|profile` sets one of the variants above;
  `profile` runs with `D9MT_PROFILE=1`, for a change to the profiling counters, and is invalid
  unless every program logs `D9MT: profiler clock:` (a base from #141 on). `ci/gpu/run.sh` keeps
  the logs of every profile run that finishes (not of one the queue ended). Five frames reach no `PROF` window (256 frames) and rarely a `STATS`
  second;
  `D9MT_AIR_MACOS=26` or `27` reaches the programs for the AIR target.
- `CACHE=<dir>` puts d9mt's three caches (AIR cache, PSO archives, state cache) into a folder
  of the caller's, used as found and left behind (neither it nor `WORK` may lie inside the other). Without it every run starts
  on empty caches inside `WORK`. Two runs on one `CACHE` are how a change to a cache's key or
  folder is tested: base fills it, head runs on it, and `WORK/cache.txt` of each run lists the
  PSO folders and the AIR cache's tables with their row counts.
- The runner clones the engine into `WORK`, installs this tree's bridge and `d3d9.dll`, makes a
  new prefix in which no crash dialog can open (and checks that before any program runs), and
  runs each program under `test/harness/exc_guard.c`, which turns a crash into a log file.
  `test/harness/d9h.h` is force-included into every program and prints a hash of each frame.
- `hashes.txt` has one line per program, `stats.txt` the counts of Metal validation lines,
  lifetime violations and programs whose log doesn't prove that d9mt drew them. The runner's
  exit status is 1 when the run itself is unusable and 3 when the GPU was lost or hung
  (`D9MT-GPU-LOST`, `D9MT-GPU-HUNG`): no verdict, run it again. A program's own exit status
  and `FAIL` lines are data for the comparison.
- A program that Wine kills at exit (`rc=137`, issue #164) after it printed every frame runs
  once more, and the second run counts. `stats.txt` names it under `exit-kill-reruns`. If the
  second run draws other frames than the first, the program isn't deterministic: its line
  gets a `FAIL`, `stats.txt` names it under `rerun-differs`, and the runner exits 1.
- `scripts/cmp-hash-runs.py` fails when a common program's frames differ (`code_mtlock` aside),
  a program is gone, head isn't clean, or head has a program not named in `--new`.
  `--allow-notclean` excuses a program only where base is unclean in the same way. With
  `--stats` it also fails on a lifetime violation on head and on more validation or
  "Invalid Metal usage" lines than base has. Its own checks are
  `scripts/test-cmp-hash-runs.py`.
- `--build-only` stops before the first program: it is what CI runs, since the hosted runners
  have no Metal 4.

`ci/gpu/run.sh <set> <out dir>` is the entry point for a Mac with a real GPU: `probe` (the Metal
device must be an Apple GPU with Metal 4, else exit status 3, no verdict), `build`, `gpu-tests`
(the native tests plus `test-commit-feedback.sh`, `test-compile-mt.sh` and
`test-pso-partition.sh`), `hash` (builds `BASE`
in a worktree, runs both for the spec's `variants=` and `targets=`, compares each pair's frames
and diagnostics) and `merged-main` (`gpu-tests`, then `hash` against the commit's first parent
for AIR targets 26 and 27). Every set builds the checkout first. It writes one line per step to
`<out dir>/summary.md`, with the swap and memory pressure before and after and the count of
`IOGPUMetalError` lines in the system log of the run's interval. Exit status 3 is "no verdict"
(no usable Metal 4, or the GPU was lost or hung); a gate that failed stays a failure.

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

`scripts/test-air-invariance.sh` (same needs) checks position invariance:
every vertex shader of the AIR corpus carries exactly the form Apple's
compiler writes for `[[position, invariant]]` with `-fpreserve-invariance`
(the floating-point function attributes, the instructions' fast-math flags,
`air.invariant`, `fast_math_disable` even under `D9MT_AIR_FASTMATH=1`), and
with `D9MT_AIR_INVARIANT=0` Apple's form without it; pixel shaders keep the
form without invariance in every mode. `test/code_invariant.c` draws one
mesh twice with two vertex shaders that share the position expression (the
second with unrelated outputs and scheduling), the second pass with
`D3DCMP_EQUAL`, and counts the pixels that fail it (must be 0).
`test/perf/bench_vs.c` is the vertex-bound bench for its cost: about 2 M
vertices a frame through a 4-bone skinning vertex shader into 640x360.

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
table is dropped, not adopted. Position invariance off is in the key: on one
cache, the driver's flags with invariance on, then off, then on give a forced
compile's outputs each time, the third run hits every entry, and invariance
off changes every vertex shader's metallib and no pixel shader's.
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
stale one unlinked itself, nothing behind a link touched), a session killed
at exit after the call of the bridge's `DLL_PROCESS_DETACH` keeping its
archive, and Metal's own temporaries (`<UUID>.metallib`) removed once older
than 600 s (#191). Every start's harness must exit 0.

`scripts/run-air-diff.sh` follows `D9MT_AIR_MACOS`, `D9MT_AIR_LEGACY` and
`D9MT_AIR_INVARIANT` (`0`: `air_compiler --no-invariance`): its
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
signed-zero case on the IR after `legalizeIr`, and
`conditions_duplicate_operand_test` that a boolean op of two equal conditions
stays that one condition (`patches/dxbc-spirv/0004`).

`test/code_flat_select.c` draws with `D3DSHADE_FLAT` across the topologies
d9mt draws flat and the ones it keeps Gouraud (triangle fans), a pixel shader
without COLOR inputs and a VS/PS pair that needs semantic reconciliation. Its
frames must hash as base until the AIR compiler honours flat; with
`D9MT_FLAT_CHECK=1` its log shows each draw's flat choice and the final-IR
check of every pixel shader function (expected values in the file's header).
The fixed-function pipeline draws `D3DSHADE_FLAT` with its own flat fragment
shader (`ffFsMainFlat`: colour inputs flat; `test/code_ff_flat.c` checks every
primitive against D3D9's provoking vertex).
The fixed-function batch's tests read back their own pixels and print
`PASS`/`FAIL` (each file's header gives the oracle): `code_ff_fog.c` (linear
fog with start == end: no fog in front of the end, full fog at and behind it, fixed
function and SM1/2 emulated fog, an SM3 control),
`code_ff_cliprhw.c` (no user clip planes for pre-transformed vertices),
`code_ff_dot3.c` (a DOTPRODUCT3 alpha op on a depth-only target),
`code_ff_cube3d.c` (cube and volume textures in fixed-function stages, #152;
run it under `MTL_SHADER_VALIDATION=1` too) and `code_ff_invariant.c` (a
fixed-function depth pass and an SM3 EQUAL pass: reported, with an asserted
depth-offset control).
Flat shading is partial: triangle fans stay Gouraud (D3D9 takes fan triangle
t's flat values from vertex t+1, Metal would take the hub of the expanded
`(0, t+1, t+2)`; non-indexed fans follow once the fan index rotation lands,
#170), and points have nothing to interpolate.

`test/code_depth_bias.c` checks `D9MT_DEPTH_BIAS_VS` (the constant depth bias in
the vertex shader, #163): one frame renders about 150 cells into a 2048x1536
target, 1x and then 4x MSAA, and prints a `D9MT-DEPTHBIAS` line per checked
pixel and a summary per pass. The cells cover:
- the bias at several depths, both signs, mixed w, slope plus constant;
- reduced and degenerate viewports, the k guard;
- the depth range contract: the biased depth isn't clamped to a reduced viewport
  range, before or after the slope term, per sample or per vertex;
- coverage at the near and far planes, the six user clip planes, wireframe,
  the guard band, negative and near-zero w;
- the shader's arithmetic domain;
- lines and points, pixel shaders writing depth, fixed function, transitions.

The file's header explains the expected values and the runs: switch on, every gating
line ok; switch off, equal to the base build; `D9MT_DEPTH_BIAS_TEST_UNHOOK=1`;
and the `no_depthbias` run. Every line carries the raw resolved pixel (`px=`), so
the cross-run comparisons compare bytes, not only the colour class. Fixture checks
(the 4x depth margin, asserted at start-up, and the automatic coverage samples)
report `fixture-FAIL`, never a driver result. The domain rows (Q, J) draw triangles
with a vertex at w = 2^126 or 2^127, which the M5 Max doesn't rasterize at all: each
has a witness cell, the same geometry without a bias, and where the witness stays
empty its lines read `norast` and count as neither ok nor FAIL (red in such a cell
is still a FAIL: the bias added coverage).

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

## CI

The `Checks` workflow (`.github/workflows/checks.yml`) runs on every pull request and every push
to `main`, on GitHub's hosted runners, with a read-only token and no secrets. It also runs every
night on `main` (a changed runner image then shows as a red run of an unchanged commit), and by
hand on any branch: `gh workflow run checks.yml --ref <branch>`, with `-f full=true` for the
nightly-only step. A pull request keeps one run per branch; every commit on `main` gets a run of
its own that a later push doesn't cancel. Its jobs:

- `lint` runs actionlint over the workflow. `shellcheck` over `scripts/` isn't part of it yet:
  the tree has 17 warnings under `shellcheck -S warning`.
- `build` runs on `xcode-27`, GitHub's macOS 27 image with Xcode 27. It builds LLVM 15.0.7 for
  x86_64 with `scripts/build-llvm-unixside.sh` (kept in the Actions cache, keyed by the recipe's
  hash), then `scripts/fetch-deps.sh`, `scripts/build-bridge.sh` with `SMOKE=0` and
  `scripts/build-driver.sh` for x64 and x86, and fails if a build changes a tracked file or keeps
  a metallib header that is older than its shader sources. Then it runs
  `scripts/run-native-tests.sh` and these scripts: `test-air-versions.sh`, `test-air-forms.sh`,
  `test-air-invariance.sh`, `test-air-cache-lineage.sh`, `test-build-deps.sh` (without
  `BUILD=1`), `test-fetch-deps.sh`, `test-null-kind.sh`, `test-pass-barrier.sh`,
  `test-present-ring.sh`, `test-pso-key-hash.sh`, `test-residency.sh`, `test-state-cache.sh`,
  `test-upload-copy.sh`, `test-perf-compare.py` and `test-cmp-hash-runs.py`. DXVK's native headers
  come from DXVK v3.0.2.
  - The two Wine scripts, `test-fatal-log.sh` and `test-cs-sync.sh`, run console programs with no
    window and no GPU. Their Wine is the engine the app installs (`cx-26.3.0-8` from
    `jdrly/wine-engine`, fetched with `gh release download` under the run's read-only token,
    three tries, then checked against its SHA-256), in a prefix `test-fatal-log.sh` makes.
  - `scripts/run-hash-tests.sh --build-only` builds every hash suite program and makes the
    runner's prefix; no program runs. `ci/gpu/run.sh probe` must end with exit status 3 there:
    the hosted runner has no Metal 4.
  - `test-build-jobs.sh` runs as a step of its own, against `main`'s build scripts.
  - `test-perf-abba.sh` runs too, as a step that doesn't block yet: its cases depend on the
    caller's environment, and 5 fail on a hosted runner. Two are the "two callers" cases (the
    caller's `OLDPWD` or `SHLVL` reaches the run). Three are the `RUN_WRAPPER` cases with
    `nice -n 3`: the runner's shell runs at nice -10, and the test expects a caller at nice 0.
  - `test-build-deps.sh` with `BUILD=1` (five x64 driver builds) runs only at night and in a run
    by hand with `full=true`.
- `metallib-load` runs on `macos-26`, a real macOS 26. It takes the AIR corpus metallibs the
  build job compiled for AIR targets 26 and 27 through Metal's classic API
  (`.github/metallib-load.sh`, `.github/metallib-load.m`). Target 26 must load, build its render
  pipelines, and get a command buffer that uses each through to completion. Target 27 must be
  refused at `newLibraryWithData`, which is what a metallib with too new a language version
  does to a macOS 26 player. On the build runner (macOS 27) both targets must load. No pixel
  is read, so a pass doesn't say that anything was drawn.
- `metal-probe` prints what the runner's Metal device can do into the job summary, on `macos-26`
  and `xcode-27`.

The hosted runners' GPU is the Apple Paravirtual device, which doesn't support Metal 4
(`supportsFamily` is true for Mac2 only, and `newMTL4CommandQueue` returns nil). So nothing that
drives the bridge's Metal 4 objects runs in CI: `test-commit-feedback.sh`, `test-compile-mt.sh`,
`test-pso-partition.sh`, the bridge's smoke test, and the hash suite. Those need a Mac with a real GPU
(`ci/gpu/run.sh`), as the Definition of Done's evidence in the pull request.

`scripts/run-air-diff.sh` isn't in CI either. It was tried on the hosted runner (pull request
#185, 2026-11-12): it reported 46 of 46 cases in all four modes with `maxdiff=0`, and it reported
the same `maxdiff=0` for nine comparisons of deliberately different shaders (`ps_show` against
the reference of `ps_sincos`, twice, and of `ps_alu_mix`, `ps_cmp`, `ps_pow`, `ps_nrm`,
`ps_if_else` and `ps_modifiers`, and `vs_base` against `vs_mvp`). A comparison that can't tell
those apart is no gate, so air-diff stays with a Mac with a real GPU. The cause wasn't looked
for: these runs don't say whether the device drew nothing or `air_diff_test`'s readback missed
it. What the hosted device does show is whether a macOS version loads a metallib and builds a
pipeline from it.

The job summary shows the build's AIR codegen id. The runner builds its own LLVM with
`scripts/build-llvm-unixside.sh`. The id hashes `CODEGEN_OBJS` in the array's order, and
`scripts/build-bridge.sh` lists the array's globs in byte order, whatever the locale of the shell
that starts it. (A plain glob sorts by the locale's collation: under macOS 27's `en_US.UTF-8`,
the runner's default, the same files in the glob's order gave `ea71911c…`.) The runner's id of
`83abcf1`'s code is `25fc2491…`, the id the 0.1.8 release build recorded on another Mac with
another Xcode. No step compares the id with a recorded one.

The summary and the `codegen-digests` artifact also list the id's parts: the digest
`scripts/air-codegen-id.py` gives each object and each LLVM archive of `CODEGEN_OBJS`
(`.github/codegen-digests.py`), in byte order. Comparing that list between two builds says which
files moved the id. The step fails when its list doesn't give the build's id: the build listed
the files in another order, or `CODEGEN_OBJS` in `scripts/build-bridge.sh` changed and the list
in the workflow has to follow.

## Benchmarks

The benchmarks are in `test/perf/` (bindbench, perdrawbench, churnbench,
psobench, bench_draws, bench_upload, bench_getdata, bench_fps_cap) and
`test/bench_4k.c`; `scripts/build-benches.sh` builds them, and
[PERFORMANCE.md](../PERFORMANCE.md) §3 describes each.
