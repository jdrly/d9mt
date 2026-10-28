# Performance guide

d9mt exists to make games faster. This guide is **binding**: the Definition of Done in
[CONTRIBUTING.md](CONTRIBUTING.md) refers to it, and the decision behind it is
[ADR-0002](docs/adr/0002-nothing-measurably-slower.md). It says what the rule is, how to measure
against it, which benchmarks and diagnostics exist, which design rules keep the hot path fast,
and what a release candidate is checked for. What isn't in this repository yet is marked _Open_.

## 1. The rule

- **Nothing measurably slower merges**, in any scenario. The per-draw and per-bind paths are the
  most sensitive; a gain in one scenario doesn't offset a loss in another (ADR-0002).
- **A cost that can't be removed waits until it can.** Measuring a regression is not enough; it
  has to be solved (ADR-0002).
- **A layout effect is not a cost of the change.** Under Rosetta, a rebuild alone moves a
  microbenchmark by up to ±3 % because the linker moves hot code; every later merge moves it
  again, and its sign is as likely to flip as to stay. A delta that disappears under the layout
  controls, in more than one block (§2.4), isn't attributed to the commit. A delta the method
  can't resolve isn't attributed to layout either: it is unresolved (§2.4, *Outcomes*).
- **The binding check is the release candidate as shipped**: the whole build at default
  alignment against the previous release (§6). A slowdown there that is layout gets a layout fix
  (an alignment flag, function order), each measured on its own, **not a revert** of the commit
  that happened to move it.
- No author or reviewer grants an exception; only the maintainer can (§2.4, step 3). The PSO
  MRU change (#40), for example, was no exception: it merged once the cause of its synthetic
  worst case (psobench `topo`) was gone with #33, merged before it, and what remained flipped
  sign with alignment, so it was layout (#40).

## 2. How to measure

### 2.1 Conditions

- **A quiet window**: the benchmark runs alone on the Mac. No game, no other Wine process, no
  build, no other benchmark. A run that another Wine process overlaps is **DIRTY** and doesn't
  count. Never measure while a game runs.
- **Uncapped**: `D9MT_BG_FPS=0 D9MT_STALL_PACE=0 D9MT_FPS_CAP=0`, so neither the background frame
  cap, the stall pacing nor the frame cap (#47) limits the frame rate. A cap inherited from the
  environment can flatten base and branch to the same frame time and hide a regression.
- **Normal priority**: don't run benchmarks under `taskpolicy -b` (background QoS): it stretches
  timers (a 1 ms sleep takes about 3 ms), which distorts anything that waits or retries (#43).
- **`D9MT_PROFILE` off for rates**: it changes the regime (frames several times slower, and G1's
  backlog gate then never carries), so use its counters, not the frame times measured with it
  (#43).
- **The display asleep** for benchmarks that don't need it awake, so no other window or
  compositor work competes with them (`scripts/perf-abba.sh`).
- **The window's client area equals the back buffer**: otherwise every present of a visible
  window takes the presenter's scaling blit (an extra pipeline and a GPU pass) instead of the
  copy. bindbench, churnbench, psobench, bench_getdata and bench_upload size their windows with
  `bench_window()` (`test/perf/bench_window.h`); bench_draws, bench_4k and bench_fps_cap with
  `AdjustWindowRect` of their own. Every bench prints `CLIENT <bench> WxH`;
  `scripts/perf-abba.sh` records it as `client=`, and `scripts/perf-compare.py` warns on a
  mismatch (#52).
- **Every variant under the same path and environment**: a process's speed can depend on the
  length of its path and environment (its memory layout). A byte-identical copy of a DLL ran 6 %
  faster than the original in every round of a bindbench run, under a variant name one character
  longer (#65). `scripts/perf-abba.sh` therefore runs every variant from one directory and, under
  Wine, with its caches under a fixed-length id, and gives each run's stderr log a name of fixed
  length (#71: `logs/<tag id>-<pid, 5 digits>-<id>-<round, zero-padded>.err`; the results header
  names each id). The same holds between two calls (#96): each run starts with `env -i` and an
  explicit environment after the `cd`. The user's `HOME`, `USER`, `LOGNAME` and `TMPDIR` come from
  the system, `PATH` and `LANG` are fixed, and `OLDPWD` is `WORK`, never the caller's cwd. A job
  runner's `JOB_ID`/`JOB_ROOT` and a virtual display's `VD_*` pass at a fixed width, and so do
  the script's Wine and d9mt variables, the caller's `D9MT_*` switches and `--env`; nothing else of
  the caller's gets through. The umask is 022, every descriptor above 2 is closed, and the tag
  reaches the log names only as an 8-hex id. The results header gives each variant's launch
  digest (names and value lengths) and the umask, nice and limits the runs inherit, so two calls
  can be compared. Keep `WORK` the same between calls that are compared: its path is the run
  directory's. `scripts/test-perf-abba.sh` checks all of this without Wine. That covers the
  harness, not every layout effect. Switches set per variant (`--env`) should be set on every
  variant, with values of the same length (`D9MT_GPU_CANARY=0` on one, `=1` on the others).
- **Clean builds only**: every measured `d3d9.dll` comes from a build from scratch.
  `build-driver.sh` tracks header dependencies since #36, but a measured variant must not depend
  on that. Record each variant's source commit next to its binary.

### 2.2 Rounds and what to report

- Run **paired rounds**: each round runs every variant once, with the order rotated by one per
  round (so each variant is in each position equally often; for two variants that is ABBA). Every
  round pairs each variant with base.
- Report per variant the **median** of its runs, the **paired difference** to base (per round,
  then the median), and **how many rounds it was slower**, for example
  `paired -0.3250 ms (-3.2%)  slower 0/10`.
- A difference within the ±3 % a rebuild can cause counts only if it repeats across windows,
  with a control next to it (§2.4).

### 2.3 The harness

```bash
scripts/build-benches.sh                  # every bench into build/bench/x64/ (ARCH=i686 for 32-bit)
D9MT_FPS_CAP=0 ENGINE=<Wine engine root> WORK=<scratch dir> scripts/perf-abba.sh \
  --variant base=<dir>/base/d3d9.dll \
  --variant basecopy=<dir>/basecopy/d3d9.dll \
  --variant fix=<dir>/fix/d3d9.dll \
  --control basecopy --rounds 10 bindbench 1060
```

- `scripts/perf-abba.sh` runs the rounds rotated (§2.2), with `D9MT_BG_FPS=0 D9MT_STALL_PACE=0`,
  the state cache off, and the rest of the caller's environment. It doesn't reset
  `D9MT_FPS_CAP`, so set `D9MT_FPS_CAP=0` for the call as above; an experiment with the cap sets
  it per variant with `--env <name>:D9MT_FPS_CAP=<fps>`. Each variant gets an AIR cache and PSO
  archives of its own (warm after its first run). The first variant is the reference unless `--ref`; `--env <name>:<VAR>=<value>`
  gives one variant extra environment (the same DLL with a switch off); `--native` compares
  native programs without Wine (one `state_cache_test` per tree). The bench is looked up in
  `BENCH_DIR` (default `build/bench/x64`) or given as a path. Results go to
  `$WORK/results/<tag>-<date>-<pid>.txt`. The harness writes nothing of its own outside `WORK`;
  a bench may create temporary files of its own (the native state-cache bench uses
  `/tmp/d9mt-sct.*`).
- A run is **DIRTY** when another Wine server is running, and **FAIL** when the program doesn't
  exit 0 with a result line (a bench given bad arguments prints `RESULT FAIL usage` and exits 2);
  both are left out of the comparison, and the script exits 1 if a run failed. `DIRTY_CHECK=0`
  turns the Wine server check off, for functional smoke runs only, never for measurements.
- `scripts/perf-compare.py results.txt …` prints per variant the runs, the median, min..max, the
  paired median against the reference and **slower n/N**. With `--control`, a variant whose paired
  median lies within the control's range is marked **`~ noise`**; a variant whose runs fall into
  two clusters (at least six runs, a gap wider than 2.5 % of the median, and at least 20 % of
  the runs on each side) is marked **`bimodal?`** (§2.5). `--metric` picks another result field (for example
  `p99` or `sd` for pacing).
- For a virtual display, build the benches with `BENCH_INCLUDE=<header>` and wrap the whole
  `perf-abba.sh` call in the display tool (`scripts/build-benches.sh`, `scripts/perf-abba.sh`).
  A virtual display can vanish when the physical display is forced to sleep during a run (#43).

#### Through the maintainer's job runner (`job Q`)

On the maintainer's Mac, benchmarks don't start by hand: each `perf-abba.sh` call is one job of
class **Q** of the workspace's test runner (not part of this repository), so the machine checks the
conditions of §2.1 instead of a person:

```bash
D9MT=<this checkout> ENGINE=<Wine engine root> WORK=<scratch dir> \
  <workspace>/tools/job/runner/adapters/timing.sh abba <new run dir> -- \
  --variant base=… --variant basecopy=… --variant fix=… --control basecopy --rounds 10 bindbench 1060
```

- **One call is one block of paired rounds:** the whole rotation runs under one admission and
  one observer, the same for every variant. Rounds are never cut to meet a deadline: a block that
  runs out of time ends without a result.
- **Before it starts**, every build and correctness job has finished, and the window is verified
  quiet for 20 s: the display **asleep** for CPU benchmarks (class Q) or **awake and unlocked** for
  present benchmarks (class Q+D, `--present`), no Wine, virtual display or compiler that the job
  doesn't own, the 1-minute load below 4, and below half a core of CPU in the user's other
  processes. That CPU is sampled from the processes the runner can read (the user's own, not the
  system's daemons, which only the load shows). A benchmark submitted while the window isn't
  quiet waits and says why.
- **During the block**, foreign Wine, a compiler, the user's other processes above half a core
  (sampled every 5 s), or a change of the class's display state makes the **whole block
  invalid** (it stops, and nothing of it counts), where `perf-abba.sh`'s own DIRTY mark only drops
  one run. The display state is per class: for Q the display waking, for Q+D the display sleeping
  or the session locking. A game launch stops it too.
- **The runner adds nothing of variable length** to a run's arguments, environment or paths: two
  variables of fixed width (a job id of 8 digits and the queue's root), plus the class. The
  workspace's fixtures run this script through it and check that every variant sees the same
  working directory, arguments, environment shape and stderr path length.
- **Cleanup** is the runner's: the job's processes and, under Wine, only `WORK`'s prefix
  (`wineserver -k`/`-w`), by exact identity, never by name.
- _Open:_ whether the runner's own sampling (its process table every 0.5 s, a memory sampler)
  biases an A/A is measured by a quiet A/A and a reverse-order window with and without it; until
  then that observer is an unmeasured condition of runs made through it.

### 2.4 Controls

- **basecopy (A/A)**: a byte-identical copy of base in its own directory, run as one more variant
  (`--control basecopy`).
  It shows the harness's bias and the window's noise floor. A delta smaller than the A/A spread
  of that window can't be told from noise in that window. Inside one block a byte-identical
  copy lands in base's mode (§2.5), so the A/A doesn't show how far two different binaries can
  move between blocks.
- **A median slowdown over 0.25 % is flagged anyway** (ms up, fps down; the median or the paired
  median against base), even inside the A/A spread, and re-verified before a merge:
  `scripts/perf-compare.py` marks it `FLAG >0.25%` and names it in a closing line (ADR-0002).
  A flag is first the author's to clear, in this order:
  1. **Re-verify** in another block (on bindbench more rounds in the same block don't help,
     §2.5), with the layout controls below and, on a bimodal bench, the mode report
     (*Per PR and at the release check*). The re-verification ends in one of the three
     outcomes below.
  2. **If the cost is established, find a solution that costs nothing.**
  3. **Only if there is none**, it goes to the maintainer, with a solid explanation of the cost
     and a second opinion from the independent agent review (GPT-6.1-Sol).

  An **unresolved** flag goes to a council: independent agent reviewers on different models
  (GPT-6.1-Sol among them) work on it alone, debate it and write one result for the maintainer.
  It isn't held without a word, and it isn't read as free (the maintainer, 2026-10-27).
- **a64 pair**: for a single commit's small delta, build base and the branch both with
  `-falign-functions=64`. If the gap disappears there, that points at code layout, not the
  change. It is a diagnosis for that block, and it counts only when it repeats across blocks.
  Check that the flag alone doesn't move base (base-a64 against base).
  a64 controls less than it seems to:
  - It aligns where functions start. **It doesn't neutralise a change of data layout:** a member
    added to or moved inside a struct shifts every later field's offset, and with it which hot
    fields share a cache line, at any code alignment. #55's branch, for example, adds three
    members in the middle of the device context, before its per-draw state, so every later
    field moves by 0x18 bytes.
  - It keeps each function's offset within a cache line, but not its address or its position
    within a page.
  - So a gap that survives a64 can still be layout, and a gap that disappears under a64 in one
    block isn't a pass yet (*Outcomes*).
- **Identical-size placebo**: a64 can't neutralise a change that alters the size of its
  translation unit's code, because everything after it moves. Then build a placebo with exactly the
  change's code size and no behaviour change, and compare the change against the placebo. If the
  change reads like the placebo, it is layout. A change that alters data layout needs a placebo
  with the same data-layout change too (the same members at the same offsets); otherwise the
  comparison mixes layout with the change. Check both against the disassembly: the function
  spans, and the field offsets the hot path uses.
- **A near-identical pair is a control, not a verdict.** Two binaries that match in the
  disassembly (an a64 pair, a change and its placebo) still place their code and data at
  different addresses. Whether such a pair reads the same in every block is measured for that
  pair, across blocks. It never passes on one block.

#### Outcomes

Every comparison of two different binaries ends in one of three outcomes, and the PR names it:

1. **Cost established**: the declared uncertainty, across blocks, supports a positive slowdown.
   It doesn't merge (§1), whatever its size, and it takes steps 2 and 3 above. +0.25 % stays
   the threshold for the flag, not for the cost.
2. **Cost bounded below +0.25 %, under stated conditions**: a one-sided upper bound, declared
   before the data, lies below +0.25 %, and the PR states the conditions it holds under (the
   bench, the blocks, the layout control). This doesn't permit a smaller slowdown that is
   measured reliably: that one is still measurably slower (§1).
3. **Unresolved**: everything else. A sign flip between blocks is unresolved, not "the flag
   didn't hold". So is one block's narrow interval, whatever its sign, when the bench's mode can
   change between blocks (§2.5). An unresolved comparison goes to a council (above).

Every raw flag stays on record with its block, its interval, its slower n/N and its outcome, also
after a later block reads otherwise.

#### Per PR and at the release check

The rule is there to stop code that costs: a design or a pipeline that makes the hot path slower
when it could be written better and faster. Layout luck is a separate question, and it gets a
separate check (the maintainer, 2026-10-27):

- **Per PR**, the cost is judged on **layout-matched** evidence: a comparison whose two sides
  share their code and data layout as far as the disassembly shows (the a64 pair or the
  identical-size placebo above). On a bimodal bench the PR reports, side by side:
  - the unfiltered paired contrast, over all rounds;
  - each binary's **mode occupancy**, its runs in each mode;
  - the **within-mode contrast**, the paired contrast over the rounds in which both runs share a
    mode.

  Declare the mode criterion before the data. The within-mode contrast is diagnosis, not the
  change's cost: if a change moves which mode a run lands in, keeping only the rounds where both
  runs share a mode removes part of its effect by construction.
- **Mode and layout shifts**, a change that moves a binary into another mode or a cost that lives
  only in the layout, are judged at the release candidate check (§6) with the calibrated method.
- **The goal is a method that resolves 0.25 %.** A larger bound for one benchmark, or an
  exception, comes only if calibration shows the method can't get there, and only the
  maintainer grants either. _Open:_ on bindbench it doesn't resolve 0.25 % yet. In the
  maintainer's runs one block of 30 to 40 rounds didn't bound even an A/A below +0.25 % (the
  intervals' upper ends were +0.59 to +0.86 %), and the contrast of two different binaries
  ranged from +5.3 % to −5.3 % over three blocks (§2.5).

### 2.5 Bimodal benchmarks

Some benchmarks land in modes: a frame time settles at one of a few levels. **Look at the per-run
values for clusters before reading a median.**

**On bindbench the mode is mostly fixed per binary and block, not per run.** Inside one block
(one call of the harness, §2.3) a binary stays at one level, with a rare run at another. The next
block can put the same binary at another level while a second binary stays put. In the
maintainer's runs one fix-versus-base pair read +5.31 %, −2.42 % and −5.28 % in three blocks, each
with a narrow interval and 40/40, 1/30 and 0/30 rounds slower, while byte-identical copies agreed
inside every block (A/A +0.12 %, +0.16 %, +0.20 %). What sets a binary's level in a block isn't
known yet; the path and the environment (§2.1) are suspects, not proven causes. So:

- **More rounds in one block don't help.** They narrow that block's interval, which covers only
  the run-to-run noise around the modes the block happened to give each binary. The uncertainty
  between blocks stays where it was. A small difference needs more blocks, not more rounds per
  block.
- **One block's interval is conditional on that block.** A narrow interval with every round on one
  side is still one block's reading, and it can flip sign in the next (§2.4, *Outcomes*).
- **The A/A doesn't show it.** A byte-identical copy lands in base's mode, so the A/A can be tight
  in a block where two different binaries sit 5 % apart.
- **`perf-compare.py`'s `bimodal?` doesn't see it either:** it looks for two clusters among one
  variant's runs, and a binary that keeps one mode for a whole block has one cluster. A mode that
  changes between blocks shows only when the blocks are compared.

**bench_fps_cap's fps can be set by stalls, not by the work per frame.** In the maintainer's
runs (bench_fps_cap, 15 s, uncapped with vsync off, no CPU work or game cap, presenting to a
visible window on the virtual display), a run's fps followed its number of long frames (`long=`)
almost exactly. Runs fell into a fast and a slow mode by time, not by variant, and variants with
no behaviour difference still differed by more than the 0.25 % rule paired within one mode. That
is one configuration: CPU work, GPU load or the bench's own game cap can set the rate instead.
In such a run:
- Don't read a present-path cost from fps alone. Equal fps under a pacing limit (vsync, a cap,
  the display's refresh) only shows that the pacing is unchanged.
- Compare the CPU time per frame: `cpuProc` and `cpuMain` are CPU milliseconds per wall-clock
  second, so divide each by the run's fps. A different stall rate moves fps and the CPU per second
  without changing the cost per frame.
- `p50` is the median frame interval. It includes the CPU work and the waits, so it isn't
  stall-free and isn't the present's cost on its own; compare it paired and within one mode.
- State each value's printed resolution next to the A/A spread: `p50` in 0.01 ms, `cpuProc` and
  `cpuMain` in whole ms per second. A difference below either is unresolved, not zero.

_The maintainer's measurements behind §2.4 and §2.5 are not public._

### 2.6 The virtual display

Whether a benchmark's presents run depends on its window's visibility, not on the display it's
on. When AppKit reports the window as **occluded**, as it does with the session locked (and so in
unattended runs), d9mt **skips the presents**: the numbers are CPU time plus GPU work without the
present copy. With the session unlocked and the display awake, a window on a virtual display is
visible and its presents run (#52).

So read a run's present path from its line, not from where it ran: `scripts/perf-abba.sh`
compares the bridge's visibility changes with the bench's timed start (`TIMED <bench>
HH:MM:SS.mmm`) and records `visible=0` (occluded from before the timed start), `visible=nolog`
(no change logged: visible throughout in a bench that presents every frame), or the changes in
order (`1>0`, `0>1`, ...) and `?` when it can't tell; `scripts/perf-compare.py` warns about runs
that changed and about occluded runs mixed with others. The scaling blit costs only in visible
runs.

On a virtual display `nextDrawable` takes much longer than a frame, so it can't answer
present-path questions (drawable waits, vsync pacing): those are measured **in a game on a real
display**, with the Metal HUD.

## 3. Benchmarks

`scripts/build-benches.sh` builds every bench into `build/bench/<arch>/` (sources in `test/perf/`,
`bench_4k` in `test/`); each prints one `RESULT` or `BENCH` line that `scripts/perf-abba.sh`
collects (§2.3). The arguments are the benches' own (their header comments); the defaults are in
parentheses.

| Benchmark | What it measures | Use it for | Source, arguments |
|---|---|---|---|
| `bench_4k` | 2048² shadow map with 4 cascades, a 3840x2160 main pass and post passes, ~2,200 draws a frame; GPU-bound (`ms/frame`) | GPU work, render passes, pass splits | `test/bench_4k.c`: `[frames (300)] [post passes (4)] [depth clears (1)]` |
| `bench_4k … wait` | The same, ending every frame with an event query polled with `D3DGETDATA_FLUSH`: the GPU idles between frames | Submission when the game waits every frame (G1 doesn't carry) | `test/bench_4k.c`, 4th argument `wait` |
| `bench_4k … <ms>` ("balanced") | The same, with that many ms of CPU before Present; near the GPU's frame time, G1's backlog gate flips | G1 and the submission path | `test/bench_4k.c`, 4th argument `<ms>` (for example `8`) |
| `bindbench` | 320x240, no vsync, 5000 tiny draws a frame, each with another of 64 textures (`avg_ms`); **bimodal** (§2.5) | The per-bind CPU path | `test/perf/bindbench.c`: `[frames (1060)]` |
| `churnbench` | 3000 rebinds of 64 textures plus 125 new 64² textures a frame (`avg_ms`) | Texture creation and release, view lifetime | `test/perf/churnbench.c`: `[frames (1060)]` |
| `psobench` | 3000 draws a frame from one vertex buffer, each switching state: between two states (`topo`, `blend`, `blend2`, `blendop`), four (`blend4`) or none (`none`). `topo` is the floor of a state switch (since #33 it doesn't resolve the PSO); in `blend`, A → B → A hits the PSO MRU (#40), and with `D9MT_NO_PSOMRU=1` every draw builds the full key and looks it up (the key hash's cost, #34). `blend2`/`blendop`/`blend4` are other key sets, so a result that depends on where two keys land shows as a sign flip | PSO resolve, MRU, key and cache | `test/perf/psobench.c`: `<frames> <topo\|blend\|blend2\|blendop\|blend4\|none>` |
| `bench_getdata` | A 4K pass of 1500 draws with an occlusion query around the first 500, polled the next frame (as SWTOR does, #22) | Queries, `GetData` flushes | `test/perf/bench_getdata.c`: `[frames (330)] [flush\|noflush]` |
| `bench_upload` | A 4K pass of 600 draws, each from a `Lock(DISCARD)` vertex buffer, plus 24 managed 256² textures rewritten a frame | Uploads, staging, the upload ring (with `--env <name>:D9MT_MEMSTATS=16`) | `test/perf/bench_upload.c`: `[frames (330)]` |
| `bench_draws` | Many small draws a frame, each with a VS and a PS constant update and a render-state change; reports ns per draw and frame times. For small deltas use `--metric frame_median_ns` (whole ns; its resolution is the printed `qpc_tick_ns` or 1 ns, whichever is coarser, not `frame_median_ms`'s 1 µs). `cpu_tick_ms` is the CPU clock's smallest step observed at startup: an upper bound on its granularity (a fine clock reads as the polling interval), 0 = unknown. With a coarse clock, `proc_cpu_ns_per_draw` can't resolve less than one tick over frames × draws | The per-draw CPU path; `present` mode is layout-sensitive and also depends on drawable availability | `test/perf/bench_draws.c`: `[draws per frame (1000)] [frames (600)] [present\|flush\|spinflush]` |
| `bench_fps_cap` | Frame pacing with vsync on or off, a set CPU time per frame and optional jitter; reports fps, percentiles, `sd` and `long` (frames over 1.5x the median), the CPU per second (`cpuProc`, `cpuMain`) and per frame (`cpuProcFrame`, `cpuMainFrame`), `p50_ms` (p50 to 4 decimals), the CPU clocks' smallest observed steps (`cpu_tick_ms`, `thread_tick_ms`: upper bounds on their granularity, 0 = unknown), `qpc_tick_ns`, and the measured interval (`measured_frames`, `measured_s`). The CPU values cover the whole interval; `n`, fps and the percentiles cover the first 200,000 stored frames. With a coarse clock, a CPU time can't resolve less than one tick over `measured_s` or `measured_frames`; printed rounding adds 0.01 ms (`p50`) or 0.0001 ms (`p50_ms`, `*Frame`) | Present-path and pacing changes (`--metric p99` or `sd`; the CPU per frame with `--metric cpuMainFrame`) | `test/perf/bench_fps_cap.c`: `[seconds (30)] [vsync 0/1 (1)] [cpu ms] [jitter ms] [quads] [game cap fps]` |
| state cache record cost | The render thread's cost of recording a pipeline (`RESULT state_cache_bench rounds= median_ns= mean_ns=`) | Changes to recording | `test/native/state_cache_test.cpp`: `BENCH_ROUNDS=<n> scripts/test-state-cache.sh`; to compare trees, `perf-abba.sh --native` with one `state_cache_test` per tree |
| state cache session | A synthetic SWTOR-like session with many pipelines (200 pixel shaders, 400 pipelines): world-phase frame times, compile waits, `STATECACHE` counters | Cold start, prewarm, the pipeline state cache | `test/code_statecache.c` via `scripts/run-statecache-bench.sh` |
| `archfirst` | Native Metal 4, no Wine: `check` for correctness, `time` for the hit cost through an `MTL4Archive` vs a compiler with `lookupArchives`, and the miss cost with 1, 8 and 64 archives | Bridge PSO-archive changes | _Open:_ only on the branch `bundle/bridge-s1-pso` |

The guards a change to the submission path runs are listed in
[docs/g1-early-commit.md](docs/g1-early-commit.md); its `bench_4kw` is `bench_4k` with the 4th
argument.

## 4. Diagnostics

`D9MT_PROFILE` (any value) turns on the `STATS`, `PACE`, `HITCH`, `COMPILE`, `RESTORE`, `XFER` and
`PROF` lines. Unset, they cost one predictable branch per scope. Set `DXVK_LOG_PATH=<dir>` to get
them in `<game>_d3d9.log`.

**`STATS`**, once a second (fields in `driver/d3d9/d3d9_stats.h`):
- `fps`, `ms=avg/p99/max` (Present to Present), `slow33`/`slow50` (frames over 33.4 and 50 ms);
- per frame: `rp` render encoders, `skip` passes that needed none, `cb` command buffers (G1 prefix
  commits included), `draws`;
- attachments: `ld`/`clr`/`ldDc` loads, `st`/`stDc` stores, `rsv` resolves folded into a store,
  `restore`, `savedMiB` (traffic avoided), `clrFold`/`clrPass`, `attMs` (render-thread time in the
  attachment optimization, ms per second);
- compiles: `sh`, `pso`, `wait=count/ms` (render-thread compile waits);
- `split`, `splitSubmit`, `splitXfer`, `splitClear`, `splitMiB`: pass splits
  ([docs/gpu-4k.md](docs/gpu-4k.md)); `carried`, `carryRefused`, `carryCap`: G1;
- `allocMiB` (Metal's allocated size), `att` (`on`, `off`, `live-off`).
- **No GPU time**: MTL4 command buffers don't report it. Use the **Metal HUD**
  (`MTL_HUD_ENABLED=1`) for GPU time and for *Direct* vs *Composited* presentation, the check for
  every present-path change.

**`PACE`**, once a second (`driver/d3d9/d3d9_pace.h`): `present=min/avg/max` and `sd` (the
interval between the game's Present calls; even pacing is 16.7 ms with an `sd` near 0),
`d9mt=min/avg/max` (time inside d9mt per frame), `wait` (the blocked part of it), `game` (the rest
of the frame), `cpu`, `d9mtBusy`, `gameCpu`.

**`HITCH`**, per frame of at least `D9MT_HITCH_MS` (default 50) (`driver/d3d9/d3d9_hitch.h`):
`inD9mtMs` close to `dtMs` means driver work; a small `inD9mtMs` means the game's own (disk
streaming, its threads). The categories (present, latencyWait, texCreate, texLock, texUpload,
bufCreate, bufLock, shaderCreate, csSync, gpuWait, throttle, query, bgSleep) nest: read a nested
one as a breakdown, not as additive.

**`COMPILE`** (`driver/mtl/mtl_compile.h`): shaders, pipelines and render-thread waits of a frame
that compiled something or took 33.4 ms or more; `skipped` (draws dropped by the wait budget or
`D9MT_ASYNC_SKIP`), `budgetHit`.

**`PROF`**, every 256 frames (`driver/backend/mtl_profiler.h`): wall time per bucket
(beginRecording, prepareDraw, resolvePso, flushVB, flushRes, flushDyn, encodeDraw, flushChunk,
submit, syncWsi, endFrame; they nest) and counters: `psoMiss`, `psoKeyBuild`, `argRotate`,
`abRebuild`, `chunkFlush`, `passClose` by cause, `evRead` (shared-event reads, each a unix call),
`unsubWait` (waits on a serial not yet submitted, also as `PROF-UNSUB` lines).

**Others**: `RESTORE`/`RESTORE-SUMMARY` (attachment re-clears by image, aspect and reason;
`driver/mtl/mtl_restore_stats.h`), `XFER` (StretchRect and ColorFill by path), `STATECACHE`
(`hits`, `late`, `misses`, `boosts`; [docs/state-cache.md](docs/state-cache.md) §7),
`DIAG MEM` (`D9MT_MEMSTATS`), and a per-function profiler (`PROFILE_INSTRUMENT=1` at build time
plus `D9MT_PROFILE_INSTRUMENT=1`).

**Lifetime and GPU faults**: `D9MT_LIFETIME_CHECK=1` aborts at the first texture released before
the GPU is past the command buffer that uses it (`D9MT-LIFETIME`), `=log` only logs; drawables are
logged as `D9MT-LIFETIME drawable` and never abort (`driver/backend/mtl_lifetime.h`). The GPU
canary (on by default) marks the present copy and every fourth other command buffer, logs
`D9MT-GPU-CANARY: on` once it works, and `D9MT-GPU-LOST` for a marked command buffer the GPU
discarded after a page fault (the first 20, then a summary at most every 10 s), so it samples
rather than sees every loss; macOS itself only logs `IOGPUMetalError` in its unified log. Use both
for any change to lifetimes or submission.

**Live flags and captures**: `D9MT_DEBUG_FILE` (flags polled once a second, for example
`no_attach_opt` for a live A/B, `att_trace`, `dump`) and `D9MT_GPUCAPTURE` (an Xcode GPU trace);
see the README's *Developer and diagnostic switches*.

## 5. Design rules for the hot path

Each rule comes from a measurement or a bug; break one only with a measurement that shows it
doesn't apply.

1. **No allocation and no `std::function` per draw, per bind or per command buffer.** Retirements
   are batched per command buffer (`RetireBatch`: no `std::function`, no allocation per entry, one
   lock and at most one pump wake per command buffer; batches keep their capacity) (#31;
   `driver/backend/mtl_submission.h`).
2. **Avoid unix calls on the hot path.** Reading the shared event is a unix call; use
   `completedSerialCached()` where a stale answer will do, and check `serial >= nextSerial()`
   before asking the event (#31, “fewer shared-event reads”). `evRead` in the `PROF` line counts
   them.
3. **Hand releases to the retire queue; don't release on the CS thread at the call site.**
   Releasing texture views on the CS thread (`ac70a8c`) was 1.3–1.5 % slower in churnbench; #24
   hands them to the retire queue instead (ADR-0001). Today the queue runs on the `d9mt-retire`
   pump plus serialised drains on the calling thread (`submit` and `endFrame` on the CS thread,
   and the recovery from a failed host allocation), and `retireAfter` releases inline when the
   serial is already complete. _Open:_ moving all of it to
   the retire thread, with one owner of GPU lifetimes, is not done (ADR-0001).
4. **Keep waits off the render thread, and bound the ones that remain.** `RenderPsoCache::get`
   looks up or starts a build without waiting for it. The limits today: a PSO miss and the
   prewarm take the registry's mutex; `getSync` waits for its build without a limit; a draw
   waits for a pending compile as long as it takes unless `d9mt.pipelineWaitBudgetMs` is set
   (off by default, ignored without compile workers) (`driver/backend/mtl_pso_cache.h`; README,
   *Pipeline wait budget*). _Open:_ when a pipeline-archive flush holds the archive lock, a
   build on the bridge bundle (`bundle/bridge-s1-pso`) falls back to a compiler without the
   serializer instead of waiting; that is not on `main` yet.
5. **Hash word by word, not byte by byte.** The PSO key hash went from a byte-wise FNV-1a over the
   whole key (about 314 ns a lookup) to xxHash64's round over 8-byte words in four lanes (49 ns):
   psobench blend −47 %, with the a64 control at −47 % too (#34; [docs/pso-key-hash.md](docs/pso-key-hash.md)).
6. **Anything compared or hashed as raw bytes has explicit padding and a
   `std::has_unique_object_representations` assert.** `= {}` with default member initializers
   doesn't zero implicit padding, and the front-end leaves struct padding uninitialised: equal
   states compared unequal and missed the caches (#30, #32, #40; `driver/backend/mtl_pso_key.h`,
   `MtlVertexLayout` in `driver/mtl/mtl_context.h`).
7. **Mark the pipeline dirty only when the pipeline really changes.** Setters return early on
   equal state, and a topology change alone doesn't dirty the pipeline, so it skips `resolvePso`
   and the resource flush: psobench topo −24 %, 0/20 slower (#33).
8. **Metal 4 doesn't retain resources: everything a command buffer uses lives until its serial
   retires** (ADR-0001). Pushing references per bind instead was 6.6 % slower on the bind path.
   Drawables never enter the residency set (it would stall `nextDrawable`), and an upload ring
   slice is valid only for the command buffer it was allocated in (`driver/backend/mtl_residency.h`,
   `driver/backend/mtl_ring.h`).
9. **G1 carries only through its gates** ([docs/g1-early-commit.md](docs/g1-early-commit.md)
   §4.2–4.3): the backlog gate (only while the GPU is behind the frame's last serial, re-checked at
   every carry, so a game that waits every frame never carries), the cost gate (attachments of at
   least `D9MT_EARLY_COMMIT_MIN_MIB`, default 8 MiB), at most two carries per serial, completion when
   the carried pass closes, and the completing submit before the pass-ending command records,
   stamps or allocates anything. Not carrying costs typically under 2 µs a frame.
10. **The PSO MRU compares content-addressed generations**, so returning to earlier state hits
    again (#40; `driver/backend/mtl_pso_gen.h`).

## 6. The release candidate's health pass

Before a stable release of an app that ships d9mt, all changes since the last release are built
into **one release candidate** at default alignment, and exactly that build is released. It is
compared with the previous release (and a basecopy) in at least five paired rounds per benchmark,
with no foreign Wine load:

- the hash suite;
- `bench_4k`, `bindbench` (uncapped; its mode can change between blocks, so several blocks, not
  only more rounds, §2.5), `churnbench`;
- `psobench` `blend` and `topo`, and `bench_draws` `present`: the binding check for the commits
  whose deltas were judged as layout;
- `bench_fps_cap` with vsync on, about 20 rounds, with counters for the drawable wait (time,
  timeouts); the present-path question behind it (#26: −2.0 % fps with vsync on, not shown to be
  significant) is also checked in a game on a real display (§2.6).

A measured regression blocks the release. If it is layout, the release candidate gets a layout
fix, each measured on its own; it is never fixed by reverting the commit that moved it.

_Open:_ the drawable-wait counters are on the branch `diag/drawable-wait-stats`, not on `main`
yet.
