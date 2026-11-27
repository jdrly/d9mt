#!/usr/bin/env bash
# test-query-getdata.sh — a TIMESTAMPFREQ query answers GetData with S_OK on the first call
# (test/native/query_getdata_test.cpp), under AddressSanitizer and UndefinedBehaviorSanitizer.
# The test compiles driver/d3d9/d3d9_query.h and d3d9_query.cpp unchanged against fakes
# (test/native/query_getdata_prepare.py sets them up). Then it runs on three copies of the .cpp
# with a planted fault each (the prepare script's controls); every one must build and fail.
#
#   scripts/test-query-getdata.sh [d3d9_query.cpp [d3d9_query.h]]     # default: this checkout's
#
# Another commit's files test that commit (git show <rev>:driver/d3d9/d3d9_query.cpp > <file>).
# Writes only under build/native-tests/query-getdata.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
CPP="${1:-$V2/driver/d3d9/d3d9_query.cpp}"
HDR="${2:-$V2/driver/d3d9/d3d9_query.h}"
OUT="$V2/build/native-tests/query-getdata"
rm -rf "$OUT"; mkdir -p "$OUT"

build() { # <dir> [<control>]: set up and build; any failure here fails the script
  python3 -I "$V2/test/native/query_getdata_prepare.py" "$CPP" "$HDR" "$1" ${2:+"$2"}
  clang++ -std=c++17 -g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I "$1" -I "$V2/test/native" "$V2/test/native/query_getdata_test.cpp" "$1/d3d9/d3d9_query.cpp" \
    -o "$1/qgt"
}

build "$OUT/driver"
"$OUT/driver/qgt"

for c in gpu-query endable no-frequency; do
  build "$OUT/$c" "$c"
  # A control must fail a check and finish (exit 3), not crash: a crash, a sanitizer's exit
  # included, would pass for a caught fault.
  rc=0; "$OUT/$c/qgt" > "$OUT/$c.log" 2>&1 || rc=$?
  failed=$(grep -c '^FAIL' "$OUT/$c.log" || true)
  if [ "$rc" != 3 ] || [ "$failed" = 0 ]; then
    echo "control $c: exit $rc with $failed failed checks, want exit 3 and a failed check ($OUT/$c.log)"
    exit 1
  fi
  echo "control $c: the test fails, as it must ($failed failed checks)"
done
