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
#   hash         the hash suite of BASE against this checkout: both are built, both run for
#                every variant and AIR target of the spec, and scripts/cmp-hash-runs.py
#                compares each pair of runs
#   merged-main  gpu-tests, then hash against this commit's first parent (default variant,
#                AIR targets 26 and 27)
#
# Spec words (hash, merged-main): variants=default,debug,noattach,repro,lifetime,shaderval,early
# (default: default); targets=27,26 (default: this Mac's own); new=<names>, allow=<names>,
# allow-notclean=<names> for scripts/cmp-hash-runs.py.
#
# Env: ENGINE (an x86_64 Wine root: the hash sets), BASE (the base commit: hash), WORK (scratch,
# default <out dir>/work), DXVK_NATIVE (DXVK's native headers; fetched into WORK when unset),
# D9MT_JOBS (default 4). toolchains/llvm-x86_64 must exist (scripts/build-llvm-unixside.sh);
# the base worktree shares it.
#
# <out dir>/summary.md gets one line per step. Exit status: 0 every step passed; 1 a gate
# failed; 3 no verdict (the GPU was lost or hung during a hash run, or the probe found no
# usable Metal 4: nothing here says anything about the commit).
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
status=0
note() { echo "$*"; echo "- $*" >> "$OUT/summary.md"; }
# step <name> <log> <command...>: runs it, notes the result, returns its exit status
step() {
  local name=$1 log=$2 rc=0 t0=$SECONDS; shift 2
  "$@" > "$log" 2>&1 || rc=$?
  if [ "$rc" = 0 ]; then note "ok: $name ($((SECONDS - t0)) s)"
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
}

# in_tree <tree> <command...>: the command with <tree> as its working directory
in_tree() { ( cd "$1" && shift && "$@" ); }
# build_tree <tree> <tag>: the bridge and the x64 driver in <tree>
build_tree() {
  local tree=$1 tag=$2
  step "$tag fetch-deps" "$OUT/$tag-fetch-deps.log" in_tree "$tree" scripts/fetch-deps.sh || return 1
  step "$tag bridge" "$OUT/$tag-bridge.log" in_tree "$tree" env SMOKE=0 scripts/build-bridge.sh || return 1
  cp "$tree/build/bridge/air-codegen-id.txt" "$OUT/$tag-air-codegen-id.txt"
  note "$tag: $(cat "$OUT/$tag-air-codegen-id.txt")"
  step "$tag driver x64" "$OUT/$tag-driver-x64.log" in_tree "$tree" env ARCH=x64 scripts/build-driver.sh || return 1
}
set_build() {
  build_tree "$V2" head || return 1
  cp "$OUT/head-air-codegen-id.txt" "$OUT/air-codegen-id.txt"
  step "head driver x86" "$OUT/head-driver-x86.log" env ARCH=x86 scripts/build-driver.sh || return 1
  [ -z "$(git status --porcelain --untracked-files=no)" ] || { note "FAILED: the builds changed tracked files"; git status --short | head; return 1; }
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
  [ -f build/bridge/winemetal.so ] && [ -f build/driver-x64/d3d9.dll ] || set_build || return 1
  dxvk_native || { note "FAILED: DXVK's native headers"; return 1; }
  step "run-native-tests.sh" "$OUT/native-tests.log" scripts/run-native-tests.sh || rc=1
  step "test-commit-feedback.sh" "$OUT/test-commit-feedback.log" scripts/test-commit-feedback.sh || rc=1
  step "test-compile-mt.sh" "$OUT/test-compile-mt.log" scripts/test-compile-mt.sh || rc=1
  return "$rc"
}

# hash_run <tree> <tag> <variant> <target or "host">: one run of the suite; its files go to
# <out>/<tag>-<target>-<variant>.{hashes,stats}.txt. Returns the runner's exit status.
hash_run() {
  local tree=$1 tag=$2 variant=$3 target=$4 rc=0 label
  local -a tenv=()
  label="$tag-$target-$variant"
  [ "$target" = host ] || tenv=("D9MT_AIR_MACOS=$target")
  in_tree "$tree" env ${tenv[@]+"${tenv[@]}"} VARIANT="$variant" WORK="$WORK/hash-$label" scripts/run-hash-tests.sh \
    > "$OUT/$label.log" 2>&1 || rc=$?
  cp "$WORK/hash-$label/hashes.txt" "$OUT/$label.hashes.txt" 2>/dev/null || true
  cp "$WORK/hash-$label/stats.txt" "$OUT/$label.stats.txt" 2>/dev/null || true
  [ ! -f "$OUT/$label.stats.txt" ] || note "$(cat "$OUT/$label.stats.txt")"
  if [ "$rc" != 0 ]; then
    mkdir -p "$OUT/$label-logs" && cp "$WORK/hash-$label/logs/"*.err "$WORK/hash-$label/logs/"*.out "$WORK/hash-$label/logs/"*.exc "$OUT/$label-logs/" 2>/dev/null || true
  fi
  rm -rf "$WORK/hash-$label"
  return "$rc"
}
set_hash() {
  : "${ENGINE:?hash needs ENGINE (an x86_64 Wine root)}"
  : "${BASE:?hash needs BASE (the base commit)}"
  export ENGINE
  local base="$WORK/base" rc=0 v t r tag
  local -a args=()
  [ -f build/bridge/winemetal.so ] && [ -f build/driver-x64/d3d9.dll ] || set_build || return 1
  git worktree remove --force "$base" 2>/dev/null; rm -rf "$base"
  git worktree add -q --detach "$base" "$BASE" || { note "FAILED: no worktree of BASE $BASE"; return 1; }
  ln -s "$V2/toolchains" "$base/toolchains"
  build_tree "$base" base || return 1
  note "hash suite: base $(git -C "$base" rev-parse --short HEAD) against head $(git rev-parse --short HEAD), variants $VARIANTS, targets ${TARGETS:-host}"
  [ -z "$NEW" ] || args+=(--new "$NEW")
  [ -z "$ALLOW" ] || args+=(--allow "$ALLOW")
  [ -z "$ALLOW_NOTCLEAN" ] || args+=(--allow-notclean "$ALLOW_NOTCLEAN")
  # "host" stands for no D9MT_AIR_MACOS: the AIR target of this Mac's own macOS
  for t in ${TARGETS:-host}; do
    for v in $VARIANTS; do
      for tag in base head; do
        r=0
        if [ "$tag" = base ]; then hash_run "$base" base "$v" "$t" || r=$?; else hash_run "$V2" head "$v" "$t" || r=$?; fi
        if [ "$r" = 3 ]; then note "NO VERDICT: the GPU was lost during the $tag run of target $t, $v"; git worktree remove --force "$base" 2>/dev/null; return 3; fi
        [ "$r" = 0 ] || { note "FAILED: the $tag run of target $t, $v is unusable (exit $r)"; rc=1; }
      done
      if python3 -I scripts/cmp-hash-runs.py "$OUT/base-$t-$v.hashes.txt" "$OUT/head-$t-$v.hashes.txt" ${args[@]+"${args[@]}"} \
           > "$OUT/cmp-$t-$v.txt" 2>&1; then
        note "ok: target $t, $v: $(head -n 1 "$OUT/cmp-$t-$v.txt")"
      else
        note "FAILED: target $t, $v:"; sed 's/^/    /' "$OUT/cmp-$t-$v.txt" | tee -a "$OUT/summary.md"; rc=1
      fi
    done
  done
  git worktree remove --force "$base" 2>/dev/null
  return "$rc"
}

: > "$OUT/summary.md"
note "$SET at $(git rev-parse HEAD) on $(sysctl -n hw.model 2>/dev/null), macOS $(sw_vers -productVersion 2>/dev/null)"
case "$SET" in
  probe) set_probe || status=$? ;;
  build) set_build || status=$? ;;
  gpu-tests) set_probe || status=$?; [ "$status" != 0 ] || set_gpu_tests || status=$? ;;
  hash) set_probe || status=$?; [ "$status" != 0 ] || set_hash || status=$? ;;
  merged-main)
    set_probe || status=$?
    if [ "$status" = 0 ]; then
      set_gpu_tests || status=$?
      BASE="${BASE:-$(git rev-parse HEAD^)}" TARGETS="${TARGETS:-26 27}"
      r=0; set_hash || r=$?
      [ "$r" = 0 ] || { [ "$r" = 3 ] && status=3; [ "$status" != 0 ] || status=$r; }
    fi ;;
  *) echo "unknown set: $SET" >&2; exit 64 ;;
esac
note "verdict: $([ "$status" = 0 ] && echo pass || { [ "$status" = 3 ] && echo "no verdict" || echo FAIL; })"
exit "$status"
