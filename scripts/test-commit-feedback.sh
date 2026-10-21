#!/usr/bin/env bash
# MTL4 commit feedback as D9MT-GPU-ERROR lines (bridge bundle part 7, docs/bridge-bundle.md):
# bridge/tests/commit_feedback_test (no Wine) commits empty MTL4 command buffers through
# build/bridge/winemetal.so's commit thunk, so Metal runs the real handler:
#   clean    no error: no line
#   each     D9MT_COMMIT_FEEDBACK_TEST=3: one PageFault line per commit (Metal calls the
#            handler for every commit), with the GPU times and the commit count
#   limit    D9MT_COMMIT_FEEDBACK_TEST=2, 50 commits: 30 lines (the diagnostic limit per 10 s);
#            one more commit 10.5 s later: its line says 20 earlier lines were suppressed
#   off      D9MT_COMMIT_FEEDBACK=0 with the test error: no handler, no line
#   wsilog   D9MT_WSILOG=1: the queue_commit lines as before
# Needs build-bridge.sh to have run.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"
OUT="$V2/build/commit-feedback"; SO="$V2/build/bridge/winemetal.so"
rm -rf "$OUT"; mkdir -p "$OUT"
clang++ -std=c++17 -O1 -g -arch x86_64 -I"$BR" "$BR/tests/commit_feedback_test.cpp" -o "$OUT/commit_feedback_test" \
  || { echo "FATAL: commit_feedback_test does not build" >&2; exit 1; }
fails=0
check() { if [ "$2" = "$3" ]; then echo "  $1: ok ($2)"; else echo "  $1: FAIL (got '$2', want '$3')"; fails=$((fails + 1)); fi; }
run() { # <log> <env...> -- <args...>
  local log=$1; shift
  local envs=(); while [ "$1" != -- ]; do envs+=("$1"); shift; done; shift
  env ${envs[@]+"${envs[@]}"} "$OUT/commit_feedback_test" "$SO" "$@" > "$OUT/$log.out" 2> "$OUT/$log.err"
  local rc=$?; [ $rc = 0 ] || { echo "  $log: FAIL (rc $rc)"; sed -n '/FAIL/p' "$OUT/$log.err" | head -3; fails=$((fails + 1)); }
}
lines() { grep -c '^D9MT-GPU-ERROR ' "$OUT/$1.err"; }

echo "[commit-feedback] clean"
run clean -- 20
check "no error, no line" "$(lines clean)" 0
check "completed" "$(cat "$OUT/clean.out")" "commits: 20 completed"

echo "[commit-feedback] each"
run each D9MT_COMMIT_FEEDBACK_TEST=3 -- 5
check "a line per commit" "$(lines each)" 5
check "named PageFault (3)" "$(grep -c '^D9MT-GPU-ERROR [0-9:.]* PageFault (3): (test) gpu=[0-9.]*-[0-9.]* now=[0-9.]* commits=[1-5]$' "$OUT/each.err")" 5

echo "[commit-feedback] limit"
run limit D9MT_COMMIT_FEEDBACK_TEST=2 -- 50 late
check "lines in the first 10 s" "$(grep -c 'Timeout (2)' "$OUT/limit.err")" 31
check "the late line counts the suppressed" "$(grep -c 'commits=51 \[20 earlier lines suppressed\]$' "$OUT/limit.err")" 1

echo "[commit-feedback] off"
run off D9MT_COMMIT_FEEDBACK=0 D9MT_COMMIT_FEEDBACK_TEST=3 -- 5
check "no handler, no line" "$(lines off)" 0

echo "[commit-feedback] wsilog"
run wsilog D9MT_WSILOG=1 -- 3
check "queue_commit lines" "$(grep -c '^D9MT-WSI\[unix\] queue_commit #[0-2] count=1$' "$OUT/wsilog.err")" 3

echo "[commit-feedback] $fails failure(s)"
[ "$fails" -eq 0 ]
