#!/usr/bin/env bash
# build-driver.sh's dependency tracking (#15). Part 1 needs no compiler: stale() is taken from
# build-driver.sh and run on crafted objects and dependency files:
#   current, a newer header, a header that's gone (moved or deleted), a path with a space
#   ("\ " in the .d), -MP's "header:" lines, no .d, the flags changed.
# Part 2 (BUILD=1, a real x64 build in build/driver-x64): a no-op build compiles 0 TUs; a flag
# change (STATE_CACHE_TESTING=1) rebuilds every object, and so does changing it back; then 0.
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
fails=0
check() { if [ "$2" = "$3" ]; then echo "  $1: ok"; else echo "  $1: FAIL (got '$2', want '$3')"; fails=$((fails + 1)); fi; }
eval "$(sed -n '/^stale() {/,/^}/p' "$V2/scripts/build-driver.sh")"
T=$(mktemp -d "${TMPDIR:-/tmp}/d9mt-deps.XXXXXX"); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/inc" "$T/with space"
touch -t 202601010000 "$T/a.cpp" "$T/inc/h1.h" "$T/with space/h2.h"
mk() { printf '%s.o: %s \\\n %s \\\n %s\n%s:\n%s:\n' "$T/a.cpp" "$T/a.cpp" "$T/inc/h1.h" "$T/with\\ space/h2.h" "$T/inc/h1.h" "$T/with\\ space/h2.h" > "$T/a.o.d"
      touch -t 202602010000 "$T/a.o" "$T/a.o.d"; }
st() { stale "$T/a.o" "$T/a.cpp" && echo stale || echo current; }
FLAGS_STALE=0
echo "[build-deps] stale()"
mk; check "everything older than the object" "$(st)" current
touch -t 202603010000 "$T/inc/h1.h"; check "a newer header" "$(st)" stale
mk; touch -t 202601010000 "$T/inc/h1.h"; touch -t 202603010000 "$T/with space/h2.h"; check "a newer header with a space in its path" "$(st)" stale
mk; touch -t 202601010000 "$T/with space/h2.h"; mv "$T/inc/h1.h" "$T/h1.moved"; check "a header that's gone" "$(st)" stale
mv "$T/h1.moved" "$T/inc/h1.h"; touch -t 202601010000 "$T/inc/h1.h"; mk; check "back again" "$(st)" current
rm "$T/a.o.d"; check "no dependency file" "$(st)" stale
mk; FLAGS_STALE=1; check "the flags changed" "$(st)" stale; FLAGS_STALE=0
touch -t 202603010000 "$T/a.cpp"; check "a newer source" "$(st)" stale

if [ "${BUILD:-0}" = 1 ]; then
  echo "[build-deps] real build (ARCH=x64)"
  b() { env ARCH=x64 "$@" "$V2/scripts/build-driver.sh" > "$T/b.log" 2>&1 || { echo "  build FAILED"; tail -5 "$T/b.log"; fails=$((fails + 1)); }; grep -c '^\[cc\]' "$T/b.log"; }
  b > /dev/null
  check "no-op build: TUs compiled" "$(b)" 0
  n=$(b STATE_CACHE_TESTING=1); [ "$n" -ge 70 ] && echo "  flags changed: $n TUs rebuilt: ok" || { echo "  flags changed: FAIL ($n TUs)"; fails=$((fails + 1)); }
  n=$(b); [ "$n" -ge 70 ] && echo "  flags back: $n TUs rebuilt: ok" || { echo "  flags back: FAIL ($n TUs)"; fails=$((fails + 1)); }
  check "no-op build again: TUs compiled" "$(b)" 0
fi
echo "[build-deps] $fails failure(s)"
[ "$fails" -eq 0 ]
