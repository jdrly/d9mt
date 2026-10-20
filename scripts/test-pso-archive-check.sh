#!/usr/bin/env bash
# PSO archive integrity at load (bridge bundle part 2, docs/bridge-bundle.md): runs
# bridge/tests/compile_mt_test (no Wine) on two shaders against build/bridge/winemetal.so
# and damages the archives between runs. Every case must load the good archives and
# delete the bad ones without crashing:
#   v2      a cold run writes "2 <size> <mtime ns> <sha>" sums; the warm run loads them
#   v1      sums rewritten in the old "<size> <sha>" form load (checked once) and come back as v2
#   trunc   an archive cut short (its .sum unchanged)               -> deleted
#   zero    an archive zero-filled, size and mtime kept as in .sum   -> deleted (SHA fallback)
#   shape   fat magic with an offset past EOF, size and mtime kept   -> deleted (shape check)
#   tailzero the last 4 KB zeroed, size and mtime kept (power loss)  -> deleted (no ENDT: the
#           SHA fallback, logged)
#   verify  D9MT_PSO_VERIFY=1 loads the good archives (full SHA-256)
#   shapeok a wrong SHA in .sum with the right size, mtime and shape still loads (so the
#           shape check, not the hash, accepted it); with D9MT_PSO_VERIFY=1 it doesn't
# Needs build-bridge.sh to have run (airlib objects, LLVM toolchain).
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/bridge"; DX="$V2/third_party/dxbc-spirv"; LLVM="$V2/toolchains/llvm-x86_64"
OUT="$V2/build/pso-check"; SO="$V2/build/bridge/winemetal.so"
rm -rf "$OUT"; mkdir -p "$OUT"
# the parser and the shape decisions on crafted input (bridge/tests/pso_archive_check_test.c)
clang -O1 "$BR/tests/pso_archive_check_test.c" "$BR/unix/pso_archive_check.c" -o "$OUT/pso_archive_check_test" \
  && "$OUT/pso_archive_check_test" | tail -1 || { echo "FATAL: pso_archive_check_test" >&2; exit 1; }
clang++ -std=c++17 -O1 -g -arch x86_64 -I"$DX" -I"$V2" -I"$LLVM/include" -I"$BR" -I"$V2/airlib" \
  -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
  "$BR/tests/compile_mt_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -lpthread -o "$OUT/compile_mt_test" \
  || { echo "FATAL: harness does not build" >&2; exit 1; }
BINS=("$V2/air/tests/bin/vs_mvp.bin" "$V2/air/tests/bin/ps_show.bin")
fails=0
run() { # <name> [env...]: one compile_mt_test run on work dir $W; prints the loaded count
  local name=$1; shift
  env MT_THREADS=2 "$@" "$OUT/compile_mt_test" "$SO" "$W" "${BINS[@]}" > "$OUT/$name.log" 2>&1
  local rc=$?
  [ $rc -ge 128 ] && echo "  $name: CRASH (signal $((rc - 128)))"
  sed -n 's/^d9mt: PSO archives: \([0-9]*\) loaded.*/\1/p' "$OUT/$name.log"
}
check() { if [ "$2" = "$3" ]; then echo "  $1: ok ($2)"; else echo "  $1: FAIL (got $2, want $3)"; fails=$((fails + 1)); fi; }
archives() { ls "$W"/pso/os-*/*.mtl4archive 2>/dev/null | sort; }
keep_time() { python3 -I -c 'import os,sys; st=os.stat(sys.argv[1]); open(sys.argv[1],"r+b").close(); os.utime(sys.argv[1], ns=(st.st_atime_ns, int(sys.argv[2])))' "$1" "$2"; }

W="$OUT/w"; mkdir -p "$W"
echo "[pso-check] v2"
run cold > /dev/null
n=$(archives | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "FATAL: the cold run wrote no archive" >&2; exit 1; }
v2=$(for a in $(archives); do head -c 2 "$a.sum"; echo; done | grep -c '^2 ')
check "v2 sums written" "$v2" "$n"
check "warm load" "$(run warm)" "$n"
cnt() { archives | wc -l | tr -d ' '; }

echo "[pso-check] v1 migration"
for a in $(archives); do
  printf '%s %s' "$(stat -f %z "$a")" "$(shasum -a 256 "$a" | cut -d' ' -f1)" > "$a.sum"
done
n=$(cnt); check "v1 load" "$(run v1)" "$n"
v1=$(for a in $(archives); do head -c 2 "$a.sum"; echo; done | grep -vc '^2 ')
check "v1 sums left (all rewritten as v2)" "$v1" 0

echo "[pso-check] verify"
n=$(cnt); check "D9MT_PSO_VERIFY=1 load" "$(run verify D9MT_PSO_VERIFY=1)" "$n"

damage() { # <case> <python: damage file f, keep its size>
  local c=$1 code=$2 a mt
  W="$OUT/$c"; rm -rf "$W"; cp -Rp "$OUT/w" "$W"
  a=$(archives | head -1); n=$(cnt)
  mt=$(sed -n 's/^2 [0-9]* \([0-9]*\) .*/\1/p' "$a.sum")
  python3 -I -c "import sys; f=sys.argv[1]; $code" "$a"
  [ "$c" = trunc ] || keep_time "$a" "$mt"
  check "$c: loaded" "$(run "$c")" "$((n - 1))"
  [ -e "$a" ] && { echo "  $c: FAIL (archive not deleted)"; fails=$((fails + 1)); } || echo "  $c: deleted"
}
echo "[pso-check] damaged archives"
damage trunc "import os; os.truncate(f, os.path.getsize(f) // 2)"
damage zero "import os; n=os.path.getsize(f); open(f,'r+b').write(bytes(n))"
damage shape "d=bytearray(open(f,'rb').read()); d[16:20]=(len(d)+4096).to_bytes(4,'big'); open(f,'wb').write(d)"
damage tailzero "import os; n=os.path.getsize(f); h=open(f,'r+b'); h.seek(n-4096); h.write(bytes(4096))"
grep -q 'unexpected shape (no ENDT)' "$OUT/tailzero.log" && echo "  tailzero: decided by the SHA fallback (logged)" \
  || { echo "  tailzero: FAIL (no fallback log line)"; fails=$((fails + 1)); }

echo "[pso-check] the shape check, not the SHA, accepts good archives"
W="$OUT/shapeok"; rm -rf "$W"; cp -Rp "$OUT/w" "$W"; n=$(cnt)
for a in $(archives); do sed -i '' 's/ [0-9a-f]\{64\}$/ 0000000000000000000000000000000000000000000000000000000000000000/' "$a.sum"; done
check "wrong SHA, right size/mtime/shape: loaded" "$(run shapeok)" "$n"
bad=$n; n=$(cnt)
check "the same with D9MT_PSO_VERIFY=1: only the last run's own archives loaded" "$(run shapeok-verify D9MT_PSO_VERIFY=1)" "$((n - bad))"

echo "[pso-check] $fails failure(s)"
[ "$fails" -eq 0 ]
