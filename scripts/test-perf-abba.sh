#!/usr/bin/env bash
# test-perf-abba.sh — perf-abba.sh's isolation of a variant's name (#65), without Wine, a bench or
# a display: fake programs and a fake Wine engine record what each run sees, and every variant must
# see the same thing under names of different lengths (a and bbbb):
#   1. --native: the working directory, argv[0] and the environment (each variable's name and the
#      length of its value);
#   2. Wine (a fake ENGINE/bin/wine): the working directory, its arguments, the environment (the
#      cache paths included), and the d3d9.dll in the run directory being the variant's own;
#   3. a relative WORK, in --native mode: every run executes;
#   4. a variant whose binary can't be copied (unreadable): the call fails and nothing runs in its
#      place (not the previous variant's binary, which is still in the run directory);
#   5. --native and 6. Wine, 10 rounds (so rounds 9 and 10): each run's stderr (fd 2) is a file of
#      its own whose path has the same length for every variant and round (#71), and the results
#      header says how to find it (the variant's id and the stderr log pattern).
# 08827cb's harness fails 1 (argv[0] is the variant's own program) and 2 (bin-<name> and
# caches/<name>), and passes 3 and 4 (it runs each program in place, so 4 is a failed run there).
# dbd9139's fails 5 and 6 (logs/<tag>-<pid>-<name>-<round>.err).
#   scripts/test-perf-abba.sh [perf-abba.sh to test (default: this tree's)]
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
ABBA="$(cd "$(dirname "${1:-$V2/scripts/perf-abba.sh}")" && pwd)/$(basename "${1:-perf-abba.sh}")"
T="$(mktemp -d /tmp/d9mt-abba-test.XXXXXX)"
trap 'chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }
lines() { if [ -f "$1" ]; then wc -l < "$1" | tr -d ' '; else echo 0; fi; }

# What a run sees, one line: its working directory, its arguments, and every environment variable
# as NAME=<length of the value>, minus the ones that differ by design (the variant's own --env).
cat > "$T/seen.sh" <<'EOF'
#!/usr/bin/env bash
env_shape=$(env | LC_ALL=C sort | awk -F= '{ n = $1; v = substr($0, length(n) + 2); print n "=" length(v) }' | shasum -a 256 | cut -c1-16)
echo "$(pwd -P)|$0|$*|$env_shape" >> "$SEEN"
# the file fd 2 points to (fcntl F_GETPATH)
python3 -I -c 'import fcntl; print(fcntl.fcntl(2, getattr(fcntl, "F_GETPATH", 50), bytes(1024)).split(b"\0")[0].decode())' >> "$SEEN.fd2"
echo "RESULT fake avg_ms=1.0"
EOF
# --native programs: the same script under different contents (each variant's own binary)
for v in a bbbb; do { cat "$T/seen.sh"; echo "# variant $v"; } > "$T/prog-$v"; chmod +x "$T/prog-$v"; done

same() {  # <file> <what>: all lines identical, and as many as expected
  local n; n=$(sort -u "$1" | wc -l | tr -d ' ')
  if [ "$(lines "$1")" = "$3" ] && [ "$n" = 1 ]; then ok "$2"; else bad "$2"; sed 's/^/       /' "$1"; fi
}

# 1. --native, unequal-length names
export SEEN="$T/seen-native" DIRTY_CHECK=0
WORK="$T/w1" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 2 -- bench 5 >/dev/null 2>&1
same "$SEEN" "native: the same cwd, argv[0] and environment shape under names a and bbbb" 4

# 2. Wine, with a fake engine: wine records what the run sees and which d3d9.dll it found
mkdir -p "$T/engine/bin" "$T/bench"
{ cat <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = wineboot ]; then mkdir -p "$WINEPREFIX" && : > "$WINEPREFIX/system.reg"; exit 0; fi
echo "$(cat d3d9.dll)" >> "$SEEN.dll"
EOF
sed 1d "$T/seen.sh"; } > "$T/engine/bin/wine"
printf '#!/bin/sh\nexit 0\n' > "$T/engine/bin/wineserver"
chmod +x "$T/engine/bin/wine" "$T/engine/bin/wineserver"
: > "$T/bench/fake.exe"
echo dll-a > "$T/d3d9-a.dll"; echo dll-bbbb > "$T/d3d9-bbbb.dll"
export SEEN="$T/seen-wine"
ENGINE="$T/engine" WORK="$T/w2" BENCH_DIR="$T/bench" bash "$ABBA" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 2 fake >/dev/null 2>&1
same "$SEEN" "wine: the same cwd, arguments and environment shape (cache paths included) under names a and bbbb" 4
# rotation: round 1 runs bbbb then a, round 2 a then bbbb
if [ "$(tr '\n' ' ' < "$SEEN.dll" 2>/dev/null)" = "dll-bbbb dll-a dll-a dll-bbbb " ]; then ok "wine: each run found its own variant's d3d9.dll"
else bad "wine: each run found its own variant's d3d9.dll ($(tr '\n' ' ' < "$SEEN.dll" 2>/dev/null))"; fi

# 3. a relative WORK in --native mode
export SEEN="$T/seen-rel"
(cd "$T" && WORK=relative-work bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 1 -- bench 5 >/dev/null 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ "$(lines "$SEEN")" = 2 ]; then ok "a relative WORK: every run executes"
else bad "a relative WORK: every run executes (exit $rc, $(lines "$SEEN") runs)"; fi

# 4. a binary that can't be copied (unreadable): rotation runs a first, then bbbb's copy fails
export SEEN="$T/seen-copy"
cp "$T/prog-bbbb" "$T/prog-locked"; chmod 000 "$T/prog-locked"
WORK="$T/w4" bash "$ABBA" --native --variant bbbb="$T/prog-locked" --variant a="$T/prog-a" --rounds 1 -- bench 5 >/dev/null 2>&1
rc=$?
if [ "$rc" != 0 ] && [ "$(lines "$SEEN")" = 1 ]; then ok "a failed copy: the call fails (exit $rc), nothing runs in its place"
else bad "a failed copy: the call fails, nothing runs in its place (exit $rc, $(lines "$SEEN") runs)"; fi

# 5./6. the stderr log of every run: one file per run, the same path length, findable from the header
fd2() {  # <what> <seen file> <results dir> <runs>
  local what=$1 f=$2.fd2 res pat n ok=1 missing="" v id r p
  n=$(sort -u "$f" 2>/dev/null | awk '{ print length($0) }' | sort -u | wc -l | tr -d ' ')
  if [ "$(lines "$f")" = "$4" ] && [ "$(sort -u "$f" | wc -l | tr -d ' ')" = "$4" ] && [ "$n" = 1 ]; then
    ok "$what: each run's stderr is its own file, the same path length for every variant and round ($(head -1 "$f" | awk '{ print length($0) }') characters)"
  else bad "$what: each run's stderr is its own file, the same path length for every variant and round"; sed 's/^/       /' "$f" 2>/dev/null; fi
  res=$(printf "%s\n" "$3"/results/*.txt | head -1)
  pat=$(sed -n 's/^#   stderr logs: //p' "$res" 2>/dev/null)
  [ -n "$pat" ] || ok=0
  for v in a bbbb; do
    id=$(awk -v v="$v" '$1 == "#" && $2 == v && $3 == "=" { for (i = 4; i <= NF; i++) if ($i ~ /^(cache|id)=[0-9a-f]+$/) { sub(/^[a-z]+=/, "", $i); print $i } }' "$res" 2>/dev/null)
    [ -n "$id" ] || { ok=0; missing="$missing $v:no-id"; continue; }
    for r in $(seq 10); do
      p=$(printf '%s' "$pat" | sed -e "s/<[a-z ]*id>/$id/" -e "s/<round %02d>/$(printf '%02d' "$r")/")
      # fd 2's path is the resolved one (/private/tmp for /tmp)
      sed 's#^/private/#/#' "$f" | grep -qxF "${p#/private}" && [ -f "$p" ] || { ok=0; missing="$missing $v:$r"; }
    done
  done
  if [ "$ok" = 1 ]; then ok "$what: the results header names each run's stderr log (variant id + pattern)"
  else bad "$what: the results header names each run's stderr log (pattern '${pat:-none}', missing$missing)"; fi
}
export SEEN="$T/seen-fd2n"
WORK="$T/w5" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 10 -- bench 5 >/dev/null 2>&1
fd2 "native, 10 rounds" "$SEEN" "$T/w5" 20
export SEEN="$T/seen-fd2w"
ENGINE="$T/engine" WORK="$T/w6" BENCH_DIR="$T/bench" bash "$ABBA" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 10 fake >/dev/null 2>&1
fd2 "wine, 10 rounds" "$SEEN" "$T/w6" 20

[ "$fail" = 0 ] && echo "test-perf-abba: all passed ($ABBA)" || echo "test-perf-abba: $fail FAILED ($ABBA)"
[ "$fail" = 0 ]
