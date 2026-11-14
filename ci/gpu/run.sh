#!/usr/bin/env bash
# The GPU gates of this checkout, by set, for a Mac with a real Apple GPU (a hosted runner has
# none with Metal 4). One entry point, so whatever starts the gates (a workflow on a
# self-hosted runner, a person over SSH) runs the same thing, versioned with the code it tests.
#
#   ci/gpu/run.sh <set> <out dir> [spec words...]
#
# Sets:
#   probe        the Metal device (.github/metal-probe.m): a GPU family of Apple7 or later and
#                the Metal 4 objects the bridge creates at start-up
#   build        scripts/fetch-deps.sh, the bridge (SMOKE=0), the driver for x64 and x86; the
#                AIR codegen id goes into <out>/air-codegen-id.txt
#   gpu-tests    build, then scripts/run-native-tests.sh and the two tests that drive Metal 4:
#                scripts/test-commit-feedback.sh and scripts/test-compile-mt.sh
#   hash         the hash suite of BASE against this checkout: both are built, both run through
#                this checkout's runner for every variant and AIR target of the spec, and
#                scripts/cmp-hash-runs.py compares each pair of runs, frames and diagnostics
#   merged-main  gpu-tests, then hash against this commit's first parent (default variant,
#                AIR targets 26 and 27)
#
# Spec words (hash, merged-main): variants=default,debug,noattach,repro,lifetime,shaderval,early,profile
# (default: default); targets=27,26 (default: this Mac's own); new=<names>, allow=<names>,
# allow-notclean=<names> for scripts/cmp-hash-runs.py.
#
# Env: ENGINE (an x86_64 Wine root: the hash sets), BASE (the base commit: hash), WORK (scratch,
# default <out dir>/work), DXVK_NATIVE (DXVK's native headers; fetched into WORK when unset),
# D9MT_JOBS (default 4). toolchains/llvm-x86_64 must exist (scripts/build-llvm-unixside.sh);
# the base worktree shares it.
#
# QUEUE (optional): the client of a job queue, for a Mac that several projects share. Each
# heavy step (a build, a test script, a suite run) then runs as its own job,
#   $QUEUE run --who $QUEUE_WHO:<step> --mem <MiB> [--wine <dir>:<dir>] -- <command>
# and waits its turn there, so another project's job can run between two steps. QUEUE_WHO
# (default d9mt) names the project. The MiB per kind of step: MEM_BUILD (default 3072, for
# D9MT_JOBS=2), MEM_TEST (2048), MEM_HASH (2048). --wine names a suite run's scratch folder,
# which holds its engine copy and its prefix. A step the queue stopped for swap (its exit 75)
# or never admitted (73) says nothing about the commit: no verdict, exit 3.
#
# Every set builds this checkout first, from what is in the tree now: an older build/ is never
# trusted. <out dir>/summary.md gets one line per step, the swap and memory pressure before
# and after, and the count of IOGPUMetalError lines in the system log of the run's interval
# (<out dir>/system-log.txt, when the log can be read).
# Exit status: 0 every step passed; 1 a gate failed; 3 no verdict (the probe found no usable
# Metal 4, or the GPU was lost or hung, with no gate failed before: nothing then says anything
# about the commit). A failed gate stays a failure whatever happens after it.
set -uo pipefail
V2="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$V2" || exit 70
[ $# -ge 2 ] || { sed -n '2,30p' "$0" >&2; exit 64; }
SET=$1 OUT=$2; shift 2
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WORK="${WORK:-$OUT/work}"
mkdir -p "$WORK"
export D9MT_JOBS="${D9MT_JOBS:-4}"
VARIANTS=default TARGETS="" NEW="" ALLOW="" ALLOW_NOTCLEAN=""
for w in "$@"; do
  for word in $w; do
    case "$word" in
      variants=*) VARIANTS=${word#variants=} ;;
      targets=*) TARGETS=${word#targets=} ;;
      new=*) NEW=${word#new=} ;;
      allow=*) ALLOW=${word#allow=} ;;
      allow-notclean=*) ALLOW_NOTCLEAN=${word#allow-notclean=} ;;
      "") ;;
      *) echo "unknown spec word: $word" >&2; exit 64 ;;
    esac
  done
done
VARIANTS=${VARIANTS//,/ } TARGETS=${TARGETS//,/ }
# shellcheck disable=SC2086 # split into words on purpose
set -- $VARIANTS
[ $# -gt 0 ] || { echo "variants= names no variant" >&2; exit 64; }
for v in "$@"; do
  case "$v" in default|debug|noattach|repro|lifetime|shaderval|early|profile) ;; *) echo "unknown variant: $v" >&2; exit 64 ;; esac
done
for t in $TARGETS; do
  case "$t" in 26|27) ;; *) echo "unknown AIR target: $t (26 or 27)" >&2; exit 64 ;; esac
done

MEM_BUILD="${MEM_BUILD:-3072}" MEM_TEST="${MEM_TEST:-2048}" MEM_HASH="${MEM_HASH:-2048}"
status=0
PROBE_OK=0
GATE_FAILED=0   # a note that starts with FAILED was written
QUEUE_LOST=0    # the queue stopped a step or never admitted it
BASE_TREE=""
cleanup() { [ -z "$BASE_TREE" ] || { git worktree remove --force "$BASE_TREE" 2>/dev/null; rm -rf "$BASE_TREE"; }; }
trap cleanup EXIT
trap 'exit 130' INT TERM
START=$(date +%s)
machine() { echo "swap $(sysctl -n vm.swapusage 2>/dev/null | sed 's/  */ /g'), memory pressure level $(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo unknown)"; }
note() { case "$*" in FAILED*) GATE_FAILED=1 ;; esac; echo "$*"; echo "- $*" >> "$OUT/summary.md"; }
# queued <step name> <MiB> [--wine <dir>] <command...>: the command, as a job of $QUEUE when
# that is set. The command must be a program, not a shell function. Sets QUEUE_RC to 73 or 75
# when the queue ended the step, to 0 otherwise.
QUEUE_RC=0
queued() {
  local name=$1 mem=$2 wine="" rc=0; shift 2
  if [ "$1" = --wine ]; then wine=$2; shift 2; fi
  QUEUE_RC=0
  if [ -z "${QUEUE:-}" ]; then "$@"; return; fi
  "$QUEUE" run --who "${QUEUE_WHO:-d9mt}:${name// /-}" --mem "$mem" ${wine:+--wine "$wine:$wine"} -- "$@" || rc=$?
  case "$rc" in 73|75) QUEUE_RC=$rc; QUEUE_LOST=1 ;; esac
  return "$rc"
}
# step <name> <log> <MiB> <command...>: runs it as a heavy step, notes the result, returns its
# exit status
step() {
  local name=$1 log=$2 mem=$3 rc=0 t0=$SECONDS; shift 3
  queued "$name" "$mem" "$@" > "$log" 2>&1 || rc=$?
  if [ "$rc" = 0 ]; then note "ok: $name ($((SECONDS - t0)) s)"
  elif [ "$QUEUE_RC" != 0 ]; then note "NO VERDICT: the queue ended $name (exit $rc, $((SECONDS - t0)) s): $(grep '^ciq: ' "$log" | tail -n 1)"
  else note "FAILED (exit $rc): $name ($((SECONDS - t0)) s), log $(basename "$log")"; tail -15 "$log"
  fi
  return "$rc"
}

set_probe() {
  local p="$WORK/metal-probe" part rc=0
  clang -Wall -O1 -framework Foundation -framework Metal .github/metal-probe.m -o "$p" || { note "FAILED: the probe doesn't build"; return 1; }
  : > "$OUT/metal-probe.txt"
  for part in device metal4 textures; do
    rc=0
    "$p" "$part" >> "$OUT/metal-probe.txt" 2>&1 || rc=$?
    [ "$rc" = 0 ] || echo "$part: the probe ended with exit status $rc" >> "$OUT/metal-probe.txt"
  done
  cat "$OUT/metal-probe.txt"
  if ! grep -E -q '^supportsFamily: .*Apple(7|8|9|10) 1' "$OUT/metal-probe.txt"; then
    note "NO VERDICT: the Metal device isn't an Apple GPU of family Apple7 or later ($(sed -n 's/^default device: //p' "$OUT/metal-probe.txt"))"
    return 3
  fi
  if ! grep -q '^newMTL4CommandQueue: ok$' "$OUT/metal-probe.txt" || ! grep -q '^Metal 4: the start-up objects exist$' "$OUT/metal-probe.txt"; then
    note "NO VERDICT: Metal 4 isn't usable on this device (see metal-probe.txt)"
    return 3
  fi
  note "ok: probe: $(sed -n 's/^default device: \(.*\) (registryID.*/\1/p' "$OUT/metal-probe.txt"), $(grep '^supportsFamily' "$OUT/metal-probe.txt")"
  PROBE_OK=1
}

# "${IN_TREE[@]}" <tree> <command...>: the command with <tree> as its working directory (a
# program, so that the queue can start it)
IN_TREE=(/bin/sh -c 'cd "$1" && shift && exec "$@"' in-tree)
# build_tree <tree> <tag>: the bridge and the x64 driver in <tree>
build_tree() {
  local tree=$1 tag=$2 rc=0 t0=$SECONDS
  # a download, not a heavy step
  "${IN_TREE[@]}" "$tree" scripts/fetch-deps.sh > "$OUT/$tag-fetch-deps.log" 2>&1 || rc=$?
  if [ "$rc" = 0 ]; then note "ok: $tag fetch-deps ($((SECONDS - t0)) s)"
  else note "FAILED (exit $rc): $tag fetch-deps ($((SECONDS - t0)) s), log $tag-fetch-deps.log"; tail -15 "$OUT/$tag-fetch-deps.log"; return 1
  fi
  step "$tag bridge" "$OUT/$tag-bridge.log" "$MEM_BUILD" "${IN_TREE[@]}" "$tree" env SMOKE=0 scripts/build-bridge.sh || return 1
  cp "$tree/build/bridge/air-codegen-id.txt" "$OUT/$tag-air-codegen-id.txt"
  note "$tag: $(cat "$OUT/$tag-air-codegen-id.txt")"
  step "$tag driver x64" "$OUT/$tag-driver-x64.log" "$MEM_BUILD" "${IN_TREE[@]}" "$tree" env ARCH=x64 scripts/build-driver.sh || return 1
}
HEAD_BUILT=0
# This checkout is built once per invocation, always: files in build/ from an earlier commit
# would otherwise be tested under this commit's name.
set_build() {
  [ "$HEAD_BUILT" = 1 ] && return 0
  build_tree "$V2" head || return 1
  cp "$OUT/head-air-codegen-id.txt" "$OUT/air-codegen-id.txt"
  step "head driver x86" "$OUT/head-driver-x86.log" "$MEM_BUILD" env ARCH=x86 scripts/build-driver.sh || return 1
  [ -z "$(git status --porcelain --untracked-files=no)" ] || { note "FAILED: the builds changed tracked files"; git status --short | head; return 1; }
  HEAD_BUILT=1
}

dxvk_native() {
  [ -n "${DXVK_NATIVE:-}" ] && return 0
  local d="$WORK/dxvk"
  if [ ! -d "$d/include/native/directx" ]; then
    # DXVK v3.0.2 (driver/UPSTREAM's sync point), by commit, as the Checks workflow fetches it
    rm -rf "$d"; git init -q "$d" || return 1
    git -C "$d" fetch -q --depth 1 https://github.com/doitsujin/dxvk 6b20f622a77b87b2921fe5d2c1774d2f2ba3e9b7 || return 1
    git -C "$d" -c advice.detachedHead=false checkout -q FETCH_HEAD || return 1
    git -C "$d" submodule update -q --init --depth 1 include/native/directx || return 1
  fi
  export DXVK_NATIVE="$d/include/native"
}
set_gpu_tests() {
  local rc=0
  set_build || return 1
  dxvk_native || { note "FAILED: DXVK's native headers"; return 1; }
  step "run-native-tests.sh" "$OUT/native-tests.log" "$MEM_TEST" scripts/run-native-tests.sh || rc=1
  step "test-commit-feedback.sh" "$OUT/test-commit-feedback.log" "$MEM_TEST" scripts/test-commit-feedback.sh || rc=1
  step "test-compile-mt.sh" "$OUT/test-compile-mt.log" "$MEM_TEST" scripts/test-compile-mt.sh || rc=1
  return "$rc"
}

# hash_run <tree> <tag> <variant> <target or "host">: one run of the suite; its files go to
# <out>/<tag>-<target>-<variant>.{hashes,stats}.txt. Returns the runner's exit status, or 4
# when the queue ended the run.
hash_run() {
  local tree=$1 tag=$2 variant=$3 target=$4 rc=0 label
  # without the caller's CACHE (the runner reads an empty one as unset): base and head each
  # start on empty caches of their own
  local -x CACHE=""
  local -a tenv=()
  label="$tag-$target-$variant"
  [ "$target" = host ] || tenv=("D9MT_AIR_MACOS=$target")
  # this checkout's runner and harness for both trees: a base from before the runner existed
  # has neither, and two runners would be two harnesses
  queued "hash $label" "$MEM_HASH" --wine "$WORK/hash-$label" \
    env ${tenv[@]+"${tenv[@]}"} SRC="$tree" VARIANT="$variant" WORK="$WORK/hash-$label" "$V2/scripts/run-hash-tests.sh" \
    > "$OUT/$label.log" 2>&1 || rc=$?
  if [ "$QUEUE_RC" != 0 ]; then
    note "NO VERDICT: the queue ended the $label run (exit $rc): $(grep '^ciq: ' "$OUT/$label.log" | tail -n 1)"
    keep_killed_logs "$label"
    rm -rf "$WORK/hash-$label"
    return 4
  fi
  cp "$WORK/hash-$label/hashes.txt" "$OUT/$label.hashes.txt" 2>/dev/null || true
  cp "$WORK/hash-$label/stats.txt" "$OUT/$label.stats.txt" 2>/dev/null || true
  [ ! -f "$OUT/$label.stats.txt" ] || note "$(cat "$OUT/$label.stats.txt")"
  # a failed run's logs, and every profile run's: its HITCH/STATS/PROF lines are the point
  if [ "$rc" != 0 ] || [ "$variant" = profile ]; then
    mkdir -p "$OUT/$label-logs" && cp "$WORK/hash-$label/logs/"*.err "$WORK/hash-$label/logs/"*.out "$WORK/hash-$label/logs/"*.exc "$OUT/$label-logs/" 2>/dev/null || true
  fi
  keep_killed_logs "$label"
  rm -rf "$WORK/hash-$label"
  return "$rc"
}
# keep_killed_logs <label>: the first logs of a program that was killed at exit and run again
# (issue #164) go to <out>/<label>-killed/. Call it before a suite run's scratch folder is removed.
keep_killed_logs() {
  if [ -d "$WORK/hash-$1/logs/killed" ]; then
    mkdir -p "$OUT/$1-killed" && cp "$WORK/hash-$1/logs/killed/"* "$OUT/$1-killed/" 2>/dev/null || true
  fi
}
set_hash() {
  : "${ENGINE:?hash needs ENGINE (an x86_64 Wine root)}"
  : "${BASE:?hash needs BASE (the base commit)}"
  export ENGINE
  local rc=0 v t r tag compared=0
  local -a args=()
  set_build || return 1
  BASE_TREE="$WORK/base"
  git worktree remove --force "$BASE_TREE" 2>/dev/null; rm -rf "$BASE_TREE"; git worktree prune
  git worktree add -q --detach "$BASE_TREE" "$BASE" || { note "FAILED: no worktree of BASE $BASE"; return 1; }
  ln -s "$V2/toolchains" "$BASE_TREE/toolchains"
  build_tree "$BASE_TREE" base || return 1
  note "hash suite: base $(git -C "$BASE_TREE" rev-parse --short HEAD) against head $(git rev-parse --short HEAD), variants $VARIANTS, targets ${TARGETS:-host}"
  [ -z "$NEW" ] || args+=(--new "$NEW")
  [ -z "$ALLOW" ] || args+=(--allow "$ALLOW")
  [ -z "$ALLOW_NOTCLEAN" ] || args+=(--allow-notclean "$ALLOW_NOTCLEAN")
  # "host" stands for no D9MT_AIR_MACOS: the AIR target of this Mac's own macOS
  for t in ${TARGETS:-host}; do
    for v in $VARIANTS; do
      for tag in base head; do
        r=0
        if [ "$tag" = base ]; then hash_run "$BASE_TREE" base "$v" "$t" || r=$?; else hash_run "$V2" head "$v" "$t" || r=$?; fi
        if [ "$r" = 3 ]; then
          note "NO VERDICT: the GPU was lost or hung during the $tag run of target $t, $v (its logs are in $tag-$t-$v-logs)"
          [ "$rc" = 0 ] && return 3
          return 1
        fi
        if [ "$r" = 4 ]; then   # the queue ended the run: hash_run noted it
          [ "$rc" = 0 ] && return 3
          return 1
        fi
        [ "$r" = 0 ] || { note "FAILED: the $tag run of target $t, $v is unusable (exit $r)"; rc=1; }
      done
      if python3 -I scripts/cmp-hash-runs.py "$OUT/base-$t-$v.hashes.txt" "$OUT/head-$t-$v.hashes.txt" ${args[@]+"${args[@]}"} \
           --stats "$OUT/base-$t-$v.stats.txt" "$OUT/head-$t-$v.stats.txt" > "$OUT/cmp-$t-$v.txt" 2>&1; then
        note "ok: target $t, $v: $(head -n 1 "$OUT/cmp-$t-$v.txt"); $(tail -n 1 "$OUT/cmp-$t-$v.txt")"
      else
        note "FAILED: target $t, $v:"; sed 's/^/    /' "$OUT/cmp-$t-$v.txt" | tee -a "$OUT/summary.md"; rc=1
      fi
      compared=$((compared + 1))
    done
  done
  [ "$compared" -gt 0 ] || { note "FAILED: no pair of runs was compared"; rc=1; }
  return "$rc"
}

# The system log of the run's interval: the evidence of a GPU fault, and the faults the driver's
# own lines may miss. Kept whenever the GPU was used, whatever the verdict so far.
system_log() {
  local n
  if log show --start "@$START" --end "@$(date +%s)" --style compact --predicate \
       'process == "kernel" OR process == "WindowServer" OR subsystem CONTAINS[c] "GPU" OR subsystem CONTAINS[c] "IOAccel" OR senderImagePath CONTAINS[c] "GPU" OR senderImagePath CONTAINS[c] "IOAccel"' \
       > "$OUT/system-log.txt" 2> "$OUT/system-log.err"; then
    n=$(grep -c 'IOGPUMetalError' "$OUT/system-log.txt" || true)
    note "system log: $n IOGPUMetalError lines in the run's interval"
    [ "$n" = 0 ] || return 3
  else
    note "system log: not readable here ($(head -n 1 "$OUT/system-log.err"))"
  fi
}

: > "$OUT/summary.md"
note "$SET at $(git rev-parse HEAD) on $(sysctl -n hw.model 2>/dev/null), macOS $(sw_vers -productVersion 2>/dev/null)"
note "before: $(machine)"
# worst <status>: a failed gate (1) stays; "no verdict" (3) only replaces a pass
worst() { case "$status:$1" in 0:*) status=$1 ;; 3:1) status=1 ;; esac; }
run_set() { local r=0; "$@" || r=$?; worst "$r"; }
case "$SET" in
  probe) run_set set_probe ;;
  build) run_set set_build ;;
  gpu-tests) run_set set_probe; [ "$status" != 0 ] || run_set set_gpu_tests ;;
  hash) run_set set_probe; [ "$status" != 0 ] || run_set set_hash ;;
  merged-main)
    run_set set_probe
    if [ "$status" = 0 ]; then
      BASE="${BASE:-$(git rev-parse HEAD^)}" TARGETS="${TARGETS:-26 27}"
      run_set set_gpu_tests
      run_set set_hash
    fi ;;
  *) echo "unknown set: $SET" >&2; exit 64 ;;
esac
# After any set that used the GPU (the probe passed), also when a gate failed or a run ended
# with no verdict: that is when the log is needed most. worst() keeps a failure a failure.
case "$SET" in
  probe|build) ;;
  *) [ "$PROBE_OK" != 1 ] || run_set system_log ;;
esac
# A step the queue ended made its set return a failure. With no failed gate beside it, that is
# no verdict: the commit wasn't judged.
if [ "$status" = 1 ] && [ "$QUEUE_LOST" = 1 ] && [ "$GATE_FAILED" = 0 ]; then status=3; fi
note "after: $(machine)"
note "verdict: $(case "$status" in 0) echo pass ;; 3) echo "no verdict" ;; *) echo FAIL ;; esac)"
exit "$status"
