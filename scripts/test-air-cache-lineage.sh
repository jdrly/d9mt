#!/usr/bin/env bash
# test-air-cache-lineage.sh — the AIR cache's identity (ADR-0006), without Wine, on scratch caches
# under $WORK only. The corpus: every air/tests/bin/*.bin and the hash suite's test/*_bytecode.inc,
# six flag sets each, compiled through build/bridge/winemetal.so's AIR thunk (bridge/tests/air_cache_test.cpp).
#  - The AIR codegen id: an object's SDK version stamp doesn't change it, one byte of its code does.
#  - The cache: the table is the one the identity names, and a second run hits every entry (no row
#    written).
#  - A foreign table (another build's) is dropped.
#  - If the id has a lineage that adopts a predecessor table: a cache holding that table (made by
#    renaming this build's own macOS 27 table, whose keys and values are the predecessor's when the
#    lineage holds; with OLD_BRIDGE=<0.1.5.1's winemetal.so>, made by that bridge itself and compared
#    row by row first) is adopted under macOS 27 with every row kept and every compile a hit, also
#    with a stray table or an empty new table next to it, and dropped under D9MT_AIR_MACOS=26
#    (VERS 2.7 rows instead).
# Needs build/bridge from scripts/build-bridge.sh.
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BR="$V2/build/bridge"
SO="$BR/winemetal.so"
WORK="${WORK:-$BR/air-cache-lineage-test}"
[ -f "$SO" ] && [ -f "$BR/air-codegen-id.txt" ] || { echo "FATAL: no $SO (run scripts/build-bridge.sh)" >&2; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK/corpus" "$WORK/caches"

clang++ -std=c++17 -O1 -arch x86_64 -I"$V2/third_party/dxbc-spirv" -I"$V2" -I"$V2/airlib" \
  "$V2/bridge/tests/air_cache_test.cpp" "$V2"/build/airlib/obj/third_party_dxbc-spirv_*.o -o "$WORK/air_cache_test"
cp "$V2"/air/tests/bin/*.bin "$WORK/corpus/"
for inc in "$V2"/test/*_bytecode.inc; do
  python3 -I - "$inc" "$WORK/corpus/test_$(basename "$inc" .inc).bin" <<'EOF'
import sys
d = open(sys.argv[1]).read().replace('\n', '').strip().strip(',')
open(sys.argv[2], 'wb').write(bytes(int(x) for x in d.split(',') if x.strip()))
EOF
done

pass=0; fail=0
check() {  # check <name> <0|1 ok> [detail]
  if [ "$2" = 1 ]; then printf '%-62s PASS %s\n' "$1" "${3:-}"; pass=$((pass+1))
  else printf '%-62s FAIL %s\n' "$1" "${3:-}"; fail=$((fail+1)); fi
}
# dump <air-cache.sqlite3>: one line per row: <table> <rowid> <key> <sha256 of the value> VERS<a.b>
dump() {
  python3 -I - "$1" <<'EOF'
import hashlib, sqlite3, struct, sys
db = sqlite3.connect(sys.argv[1])
for (t,) in db.execute("select name from sqlite_master where type='table' order by name"):
    for rowid, k, v in db.execute('select rowid, key, value from "%s" order by key' % t):
        m = v[v.find(b'MTLB'):]; j = m.find(b'VERS')
        print(t, rowid, k.hex(), hashlib.sha256(v).hexdigest(), 'VERS%d.%d' % struct.unpack_from('<2H', m, j + 6))
EOF
}
# run <name> <winemetal.so> <cache dir> <host|26>: the corpus through the thunk; <name>.rows lists the cache
run() {
  local n=$1 so=$2 c=$3 m=$4
  mkdir -p "$WORK/$n"
  if [ "$m" = 26 ]; then
    D9MT_AIR_MACOS=26 D9MT_AIR_CACHE="$c/air-cache.sqlite3" "$WORK/air_cache_test" "$so" "$WORK/$n" "$WORK"/corpus/*.bin > "$WORK/$n.log" 2>&1
  else
    env -u D9MT_AIR_MACOS D9MT_AIR_CACHE="$c/air-cache.sqlite3" "$WORK/air_cache_test" "$so" "$WORK/$n" "$WORK"/corpus/*.bin > "$WORK/$n.log" 2>&1
  fi
  dump "$c/air-cache.sqlite3" > "$WORK/$n.rows"
}
tables() { cut -d' ' -f1 "$WORK/$1.rows" | sort -u | tr '\n' ' '; }
rows_as() { sed "s/^[^ ]* /$2 /" "$WORK/$1.rows"; }  # rows_as <name> <table>: the rows, renamed
table_of() { sed -n 's/.*AIR cache .* (table \(cache_[0-9]*\),.*/\1/p' "$WORK/$1.log" | head -1; }

# 1. the id
OBJ="$V2/build/airlib/obj/air_air_metallib.cpp.o"
cp "$OBJ" "$WORK/a.o"; cp "$OBJ" "$WORK/b.o"; cp "$OBJ" "$WORK/c.o"
python3 -I - "$WORK/b.o" "$WORK/c.o" <<'EOF'
import struct, sys
def patch(p, f):
    d = bytearray(open(p, 'rb').read()); off = 32
    for _ in range(struct.unpack_from('<I', d, 16)[0]):
        cmd, cs = struct.unpack_from('<II', d, off)
        f(d, cmd, off)
        off += cs
    open(p, 'wb').write(d)
def sdk(d, cmd, off):  # LC_BUILD_VERSION's sdk field
    if cmd == 0x32: struct.pack_into('<I', d, off + 16, struct.unpack_from('<I', d, off + 16)[0] ^ 0x100)
def text(d, cmd, off):  # one byte of __text
    if cmd == 0x19:
        for i in range(struct.unpack_from('<I', d, off + 64)[0]):
            s = off + 72 + i * 80
            if d[s:s + 6] == b'__text':
                o = struct.unpack_from('<I', d, s + 48)[0]; d[o + 3] ^= 0xff
patch(sys.argv[1], sdk); patch(sys.argv[2], text)
EOF
ida=$(cd "$WORK" && cp a.o x.o && python3 -I "$V2/scripts/air-codegen-id.py" x.o)
idb=$(cd "$WORK" && cp b.o x.o && python3 -I "$V2/scripts/air-codegen-id.py" x.o)
idc=$(cd "$WORK" && cp c.o x.o && python3 -I "$V2/scripts/air-codegen-id.py" x.o)
check "codegen id: the SDK version stamp doesn't change it" "$([ "$ida" = "$idb" ] && ! cmp -s "$WORK/a.o" "$WORK/b.o" && echo 1 || echo 0)"
check "codegen id: one byte of code does" "$([ "$ida" != "$idc" ] && echo 1 || echo 0)"
ID=$(sed -n 's/^AIR codegen \([0-9a-f]\{64\}\) .*/\1/p' "$BR/air-codegen-id.txt")
check "codegen id: winemetal.so carries the build's id" \
  "$([ -n "$ID" ] && strings -a "$SO" | grep -qx "D9MT-AIR-CODEGEN-ID $ID" && echo 1 || echo 0)" "${ID:0:12}"

# 2. the cache: cold, warm, a foreign table
C=$WORK/caches
run cold "$SO" "$C/own" host
T27=$(table_of cold)
n=$(wc -l < "$WORK/cold.rows" | tr -d ' ')
check "cold: one table, the one the log names, VERS 2.9" \
  "$([ -n "$T27" ] && [ "$(tables cold)" = "$T27 " ] && [ "$(grep -vc ' VERS2.9$' "$WORK/cold.rows")" = 0 ] && echo 1 || echo 0)" "$T27, $n rows"
run warm "$SO" "$C/own" host
check "warm: every compile a hit (no row written, same outputs)" \
  "$(cmp -s "$WORK/cold.rows" "$WORK/warm.rows" && cmp -s "$WORK/cold/manifest.txt" "$WORK/warm/manifest.txt" && echo 1 || echo 0)"
cp -R "$C/own" "$C/foreign"
sqlite3 "$C/foreign/air-cache.sqlite3" "ALTER TABLE $T27 RENAME TO cache_42;"
run foreign "$SO" "$C/foreign" host
check "a foreign table is dropped, the cache starts cold" \
  "$([ "$(tables foreign)" = "$T27 " ] && ! grep -q 'kept' "$WORK/foreign.log" && echo 1 || echo 0)"

# 3. the lineage's adoption
read -r LEG LNAME < <(python3 -I - "$V2/bridge/unix/air_cache_lineage.c" "$ID" <<'EOF'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r'\{\s*"%s",\s*\{[^}]*\},\s*(\d+)ull,\s*0x([0-9a-fA-F]+)u,\s*"([^"]*)"' % sys.argv[2], src)
print(('cache_%s %s' % (m.group(1), m.group(3))) if m and m.group(1) != '0' else '- -')
EOF
)
if [ "$LEG" = - ]; then
  echo "lineage: codegen id ${ID:0:12} has no lineage that adopts a table (nothing more to test)"
else
  if [ -n "${OLD_BRIDGE:-}" ]; then
    run old "$OLD_BRIDGE" "$C/legacy" host
    check "$LNAME's bridge: its table holds this build's rows, byte for byte" \
      "$([ "$(tables old)" = "$LEG " ] && [ "$(rows_as cold x)" = "$(rows_as old x)" ] && echo 1 || echo 0)"
  else
    cp -R "$C/own" "$C/legacy"
    sqlite3 "$C/legacy/air-cache.sqlite3" "ALTER TABLE $T27 RENAME TO $LEG;"
    dump "$C/legacy/air-cache.sqlite3" > "$WORK/old.rows"
  fi
  for d in a27 a26 stray empty; do cp -R "$C/legacy" "$C/$d"; done
  sqlite3 "$C/stray/air-cache.sqlite3" "CREATE TABLE cache_123 (key BLOB PRIMARY KEY, value BLOB NOT NULL);"
  sqlite3 "$C/empty/air-cache.sqlite3" "CREATE TABLE $T27 (key BLOB PRIMARY KEY, value BLOB NOT NULL);"
  run a27 "$SO" "$C/a27" host
  check "macOS 27: $LNAME's table adopted (log)" "$(grep -q "kept $LNAME's table $LEG as $T27" "$WORK/a27.log" && echo 1 || echo 0)"
  check "macOS 27: every row kept as it was (rowid, key, value), no row written" \
    "$([ "$(rows_as old "$T27")" = "$(cat "$WORK/a27.rows")" ] && echo 1 || echo 0)" "$(tables a27)"
  check "macOS 27: the outputs are this build's own" "$(cmp -s "$WORK/cold/manifest.txt" "$WORK/a27/manifest.txt" && echo 1 || echo 0)"
  run stray "$SO" "$C/stray" host
  check "macOS 27, a stray table next to it: adopted, the stray dropped" \
    "$([ "$(rows_as old "$T27")" = "$(cat "$WORK/stray.rows")" ] && echo 1 || echo 0)" "$(tables stray)"
  run empty "$SO" "$C/empty" host
  check "macOS 27, an empty new table already there: adopted all the same" \
    "$([ "$(rows_as old "$T27")" = "$(cat "$WORK/empty.rows")" ] && echo 1 || echo 0)" "$(tables empty)"
  run a26 "$SO" "$C/a26" 26
  T26=$(table_of a26)
  check "macOS 26: not adopted, $LNAME's table dropped" \
    "$(! grep -q 'kept' "$WORK/a26.log" && [ "$(tables a26)" = "$T26 " ] && [ "$T26" != "$T27" ] && echo 1 || echo 0)" "$T26"
  check "macOS 26: every row VERS 2.7, as many as compiled" \
    "$([ "$(grep -vc ' VERS2.7$' "$WORK/a26.rows")" = 0 ] && [ "$(wc -l < "$WORK/a26.rows" | tr -d ' ')" = "$n" ] && echo 1 || echo 0)"
fi
echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ] && [ "$pass" -gt 0 ]
