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
#      header says how to find it (the variant's id and the stderr log pattern);
#   7. --native and 8. Wine, two calls of one WORK (#96): from different cwds, with different tags,
#      OLDPWDs, PATHs, shell levels and virtual display positions, one with a secret-like variable:
#      every run sees the same cwd, argv, environment (names, order, values; VD_* only the same
#      length) and umask (the callers' are 077 and 022), fd 2 paths of the same length, no caller
#      variable but D9MT_*, OLDPWD=WORK, and the results headers give the same launch digests;
#   9. RUN_WRAPPER: a wrapper whose first word is an assignment (leading blank included) is refused
#      and nothing runs; a real wrapper (nice -n 3) runs whole and adds nothing to the environment,
#      also when its words are split by newlines (one leading), so none of it is dropped;
#  10. precedence, as before #96: --env changes the Wine defaults (WINEDEBUG, WINEMSYNC) but not the
#      run's own settings (WINEDLLOVERRIDES, D9MT_STATE_CACHE), and only for its variant;
#  11. the launch digest counts bytes: a D9MT_* value of "a" and of "é" give different digests
#      under a UTF-8 locale;
#  12. visible= against the timed start (a fake engine prints the bridge's visibility lines and
#      TIMED): no change is nolog; changes all before it that leave the window visible are pre:0>1;
#      one at or after it keeps the plain sequence; across midnight too; out-of-order or far-off
#      clocks are ?; vis_ms= gives each change's offset; perf-compare.py warns exactly about changed
#      and unknown runs.
# 08827cb's harness fails 1 (argv[0] is the variant's own program) and 2 (bin-<name> and
# caches/<name>), and passes 3 and 4 (it runs each program in place, so 4 is a failed run there).
# dbd9139's fails 5 and 6 (logs/<tag>-<pid>-<name>-<round>.err). 07d4a60's fails 7 and 8 (the
# caller's environment and OLDPWD reach the run, the tag sets fd 2's length) and 9-11.
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
# as NAME=<length of the value>, minus the ones that differ by design (the variant's own --env);
# and (7, 8) the whole environment in order, a "--" line after each run. Where it writes comes from
# D9MT_TEST_SEEN, a caller's D9MT_* switch (those reach every run).
cat > "$T/seen.sh" <<'EOF'
#!/usr/bin/env bash
SEEN=$D9MT_TEST_SEEN
{ env; echo "umask $(umask)"; echo "nice $(ps -o nice= -p $$ | tr -d ' ')"; echo --; } >> "$SEEN.env"
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
export D9MT_TEST_SEEN="$T/seen-native" DIRTY_CHECK=0
WORK="$T/w1" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 2 -- bench 5 >/dev/null 2>&1
same "$D9MT_TEST_SEEN" "native: the same cwd, argv[0] and environment shape under names a and bbbb" 4

# 2. Wine, with a fake engine: wine records what the run sees and which d3d9.dll it found
mkdir -p "$T/engine/bin" "$T/bench"
{ cat <<'EOF'
#!/usr/bin/env bash
if [ "${1:-}" = wineboot ]; then mkdir -p "$WINEPREFIX" && : > "$WINEPREFIX/system.reg"; exit 0; fi
echo "$(cat d3d9.dll)" >> "$D9MT_TEST_SEEN.dll"
EOF
sed 1d "$T/seen.sh"; } > "$T/engine/bin/wine"
printf '#!/bin/sh\nexit 0\n' > "$T/engine/bin/wineserver"
chmod +x "$T/engine/bin/wine" "$T/engine/bin/wineserver"
: > "$T/bench/fake.exe"
echo dll-a > "$T/d3d9-a.dll"; echo dll-bbbb > "$T/d3d9-bbbb.dll"
export D9MT_TEST_SEEN="$T/seen-wine"
ENGINE="$T/engine" WORK="$T/w2" BENCH_DIR="$T/bench" bash "$ABBA" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 2 fake >/dev/null 2>&1
same "$D9MT_TEST_SEEN" "wine: the same cwd, arguments and environment shape (cache paths included) under names a and bbbb" 4
# rotation: round 1 runs bbbb then a, round 2 a then bbbb
if [ "$(tr '\n' ' ' < "$D9MT_TEST_SEEN.dll" 2>/dev/null)" = "dll-bbbb dll-a dll-a dll-bbbb " ]; then ok "wine: each run found its own variant's d3d9.dll"
else bad "wine: each run found its own variant's d3d9.dll ($(tr '\n' ' ' < "$D9MT_TEST_SEEN.dll" 2>/dev/null))"; fi

# 3. a relative WORK in --native mode
export D9MT_TEST_SEEN="$T/seen-rel"
(cd "$T" && WORK=relative-work bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 1 -- bench 5 >/dev/null 2>&1)
rc=$?
if [ "$rc" = 0 ] && [ "$(lines "$D9MT_TEST_SEEN")" = 2 ]; then ok "a relative WORK: every run executes"
else bad "a relative WORK: every run executes (exit $rc, $(lines "$D9MT_TEST_SEEN") runs)"; fi

# 4. a binary that can't be copied (unreadable): rotation runs a first, then bbbb's copy fails
export D9MT_TEST_SEEN="$T/seen-copy"
cp "$T/prog-bbbb" "$T/prog-locked"; chmod 000 "$T/prog-locked"
WORK="$T/w4" bash "$ABBA" --native --variant bbbb="$T/prog-locked" --variant a="$T/prog-a" --rounds 1 -- bench 5 >/dev/null 2>&1
rc=$?
if [ "$rc" != 0 ] && [ "$(lines "$D9MT_TEST_SEEN")" = 1 ]; then ok "a failed copy: the call fails (exit $rc), nothing runs in its place"
else bad "a failed copy: the call fails, nothing runs in its place (exit $rc, $(lines "$D9MT_TEST_SEEN") runs)"; fi

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
export D9MT_TEST_SEEN="$T/seen-fd2n"
WORK="$T/w5" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 10 -- bench 5 >/dev/null 2>&1
fd2 "native, 10 rounds" "$D9MT_TEST_SEEN" "$T/w5" 20
export D9MT_TEST_SEEN="$T/seen-fd2w"
ENGINE="$T/engine" WORK="$T/w6" BENCH_DIR="$T/bench" bash "$ABBA" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 10 fake >/dev/null 2>&1
fd2 "wine, 10 rounds" "$D9MT_TEST_SEEN" "$T/w6" 20

# 7./8. two calls of one WORK from different callers: only VD_*'s values (at a fixed width) and the
# test's own D9MT_TEST_SEEN may differ
twocalls() {  # <what> <work> <perf-abba args after --tag>
  local what=$1 w=$2 d h1 h2 r1 r2; shift 2
  mkdir -p "$T/c1" "$T/a/much/longer/caller/directory"
  export D9MT_TEST_SEEN="$T/seen-two-$what-1"
  (cd "$T/c1" && umask 077 && env OLDPWD=/x SHLVL=2 PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH" SECRET_TOKEN=hunter2 \
     VD_BOUNDS="1920 0 1600 1400" VD_X=1980 VD_Y=60 WORK="$w" bash "$ABBA" --tag t "$@" >/dev/null 2>&1)
  r1=$?
  export D9MT_TEST_SEEN="$T/seen-two-$what-2"
  (cd "$T/a/much/longer/caller/directory" && env OLDPWD=/a/much/longer/previous/directory SHLVL=7 \
     PATH="$PATH:/usr/local/bin" VD_BOUNDS="-1600 -220 1600 1400" VD_X=-1540 VD_Y=-160 WORK="$w" \
     bash "$ABBA" --tag a-much-longer-tag "$@" >/dev/null 2>&1)
  r2=$?
  d=$(python3 -I - "$T/seen-two-$what-1" "$T/seen-two-$what-2" "$w" <<'CMP'
import sys
a, b, w = sys.argv[1:4]
def runs(p):
    try:
        return [r.strip("\n").split("\n") for r in open(p + ".env").read().split("--\n") if r.strip()]
    except OSError:
        return []
A, B = runs(a), runs(b)
out = [] if A and len(A) == len(B) else ["runs %d vs %d" % (len(A), len(B))]
for i, (x, y) in enumerate(zip(A, B)):
    for r in (x, y):
        e = dict(l.split("=", 1) for l in r if "=" in l)
        if "SECRET_TOKEN" in e or e.get("OLDPWD") != w or e.get("SHLVL") != "1":
            out.append("the caller's environment got through (SECRET_TOKEN, OLDPWD or SHLVL)")
    if [l.split("=", 1)[0] for l in x] != [l.split("=", 1)[0] for l in y]:
        out.append("the variables' names or order differ")
    for l, m in zip(x, y):
        if len(l) != len(m) or (l != m and not l.startswith(("VD_", "D9MT_TEST_SEEN="))):
            out.append("%s differs" % l.split("=", 1)[0])
try:
    sa, sb = open(a).read().splitlines(), open(b).read().splitlines()
    fa, fb = [len(l) for l in open(a + ".fd2")], [len(l) for l in open(b + ".fd2")]
except OSError:
    sa, sb, fa, fb = [], [1], [], [1]
if len(sa) != 4 or sa != sb:
    out.append("the runs' cwd, argv or environment shape differ (%d and %d runs)" % (len(sa), len(sb)))
if len(fa) != 4 or fa != fb:
    out.append("the fd 2 path lengths differ")
print("; ".join(sorted(set(out))[:6]))
CMP
)
  h1=$(grep -ho 'launch=[^ ]*' "$w"/results/t-*.txt 2>/dev/null)
  h2=$(grep -ho 'launch=[^ ]*' "$w"/results/a-much-longer-tag-*.txt 2>/dev/null)
  { [ -n "$h1" ] && [ "$h1" = "$h2" ]; } || d="${d:+$d; }the headers' launch digests differ or are missing"
  [ "$r1$r2" = 00 ] || d="${d:+$d; }exit $r1/$r2"
  if [ -z "$d" ]; then ok "$what, two callers (cwd, tag, OLDPWD, PATH, SHLVL, umask, a secret, VD_*): every run sees the same inputs, VD_* at a fixed width; the same launch digests"
  else bad "$what, two callers: $d"; fi
}
twocalls native "$T/w7" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 2 -- bench 5
ENGINE="$T/engine" BENCH_DIR="$T/bench" twocalls wine "$T/w8" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 2 fake

# 9. RUN_WRAPPER
export D9MT_TEST_SEEN="$T/seen-wrap-bad"
RUN_WRAPPER=' SECRET_TOKEN=x /usr/bin/env' WORK="$T/w9" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 1 -- bench 5 >/dev/null 2>&1
rc=$?
if [ "$rc" = 2 ] && [ "$(lines "$D9MT_TEST_SEEN")" = 0 ]; then ok "RUN_WRAPPER with an assignment for its first word (a leading blank): refused (exit 2), nothing runs"
else bad "RUN_WRAPPER with an assignment: exit $rc, $(lines "$D9MT_TEST_SEEN") runs"; fi
# the environments (and umask, nice) without the test's own D9MT_TEST_SEEN, whose file names differ
envs() { grep -v '^D9MT_TEST_SEEN=' "$1" 2>/dev/null; }
k=0
# nice 3: a wrapper that is dropped (nice 0) or cut after its first word (nice's default, 10) shows
for wr in "" "/usr/bin/nice -n 3" $'/usr/bin/nice\n-n 3' $'\n/usr/bin/nice -n 3'; do
  export D9MT_TEST_SEEN="$T/seen-wrap-$k"
  RUN_WRAPPER="$wr" WORK="$T/w9b" bash "$ABBA" --native --variant a="$T/prog-a" --variant bbbb="$T/prog-bbbb" --rounds 1 -- bench 5 >/dev/null 2>&1
  if [ "$k" -gt 0 ]; then
    if [ "$(lines "$D9MT_TEST_SEEN")" = 2 ] && [ -n "$(envs "$T/seen-wrap-0.env")" ] \
       && [ "$(envs "$T/seen-wrap-0.env" | sed 's/^nice 0$/nice 3/')" = "$(envs "$D9MT_TEST_SEEN.env")" ]; then
      ok "RUN_WRAPPER=$(printf '%q' "$wr"): both runs execute, at nice 3, and see what they see without it"
    else bad "RUN_WRAPPER=$(printf '%q' "$wr"): $(lines "$D9MT_TEST_SEEN") runs, $(diff <(envs "$T/seen-wrap-0.env") <(envs "$D9MT_TEST_SEEN.env") 2>&1 | grep '^[<>]' | head -3 | tr '\n' ' ')"; fi
  fi
  k=$((k + 1))
done

# 10. precedence: --env over the Wine defaults, the run's own settings over --env (variant a only)
export D9MT_TEST_SEEN="$T/seen-prec"
ENGINE="$T/engine" WORK="$T/w10" BENCH_DIR="$T/bench" bash "$ABBA" --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" \
  --env a:WINEDEBUG=+timestamp --env a:WINEMSYNC=0 --env a:WINEDLLOVERRIDES=d3d9=b --env a:D9MT_STATE_CACHE=1 --rounds 1 fake >/dev/null 2>&1
# round 1 runs bbbb, then a
p=$(awk 'BEGIN { RS = "--\n" } { n++; printf "%s:", n; split($0, l, "\n"); for (i in l) if (l[i] ~ /^(WINEDEBUG|WINEMSYNC|WINEDLLOVERRIDES|D9MT_STATE_CACHE)=/) printf " %s", l[i]; print "" }' "$D9MT_TEST_SEEN.env" 2>/dev/null | LC_ALL=C sort)
want_b="1: $(printf '%s\n' D9MT_STATE_CACHE=0 WINEDEBUG=-all WINEDLLOVERRIDES=d3d9=n,b WINEMSYNC=1 | tr '\n' ' ' | sed 's/ $//')"
want_a="2: $(printf '%s\n' D9MT_STATE_CACHE=0 WINEDEBUG=+timestamp WINEDLLOVERRIDES=d3d9=n,b WINEMSYNC=0 | tr '\n' ' ' | sed 's/ $//')"
p1=$(echo "$p" | sed -n 1p | tr ' ' '\n' | sed 1d | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
p2=$(echo "$p" | sed -n 2p | tr ' ' '\n' | sed 1d | LC_ALL=C sort | tr '\n' ' ' | sed 's/ $//')
if [ "1: $p1" = "$want_b" ] && [ "2: $p2" = "$want_a" ]; then ok "precedence: --env sets variant a's WINEDEBUG and WINEMSYNC, not its WINEDLLOVERRIDES or D9MT_STATE_CACHE; bbbb keeps the defaults"
else bad "precedence: bbbb '$p1', a '$p2'"; fi

# 11. the launch digest counts bytes, not characters
for v in a é; do
  D9MT_TEST_PAD=$v LC_ALL=en_US.UTF-8 WORK="$T/w11-$([ "$v" = a ] && echo 1 || echo 2)" bash "$ABBA" --native --variant a="$T/prog-a" \
    --variant bbbb="$T/prog-bbbb" --rounds 1 -- bench 5 >/dev/null 2>&1
done
d1=$(grep -ho 'launch=[^ ]*' "$T"/w11-1/results/*.txt 2>/dev/null | head -1); d2=$(grep -ho 'launch=[^ ]*' "$T"/w11-2/results/*.txt 2>/dev/null | head -1)
if [ -n "$d1" ] && [ -n "$d2" ] && [ "$d1" != "$d2" ]; then ok "the launch digest counts bytes: D9MT_TEST_PAD=a and =é differ under a UTF-8 locale"
else bad "the launch digest under a UTF-8 locale: '$d1' and '$d2'"; fi

# 12. visible= against the timed start: a fake engine prints the bridge's visibility lines (stderr)
# and the bench's TIMED (stdout); D9MT_TEST_TIMED is the timed start, D9MT_TEST_VIS the changes
# (<HH:MM:SS.mmm>/<destination: 2 occluded, 3 visible>). perf-compare.py must warn about every
# changed or unknown run and stay quiet about nolog and pre: runs that end visible.
mkdir -p "$T/engine12/bin"
cat > "$T/engine12/bin/wine" <<'EOF2'
#!/usr/bin/env bash
if [ "${1:-}" = wineboot ]; then mkdir -p "$WINEPREFIX" && : > "$WINEPREFIX/system.reg"; exit 0; fi
echo "warn:  MtlPresenter: layer props (create)" >&2
prev=3
for c in $D9MT_TEST_VIS; do
  echo "D9MT-WSI ${c%/*} visibility 0x$prev -> 0x${c#*/} on test (window 0x1)" >&2
  prev=${c#*/}
done
echo "TIMED fake $D9MT_TEST_TIMED"
echo "RESULT fake avg_ms=1.0"
EOF2
cp "$T/engine/bin/wineserver" "$T/engine12/bin/"; chmod +x "$T/engine12/bin/wine"
k=0
N=12:00:00
for c in "$N.500||nolog|quiet" \
         "$N.500|$N.100/2 $N.105/3|pre:0>1 vis_ms=-400,-395|quiet" \
         "$N.500|$N.100/2|0 vis_ms=-400|quiet" \
         "$N.500|$N.100/2 $N.900/3|0>1 vis_ms=-400,+400|warn" \
         "$N.500|$N.900/2|1>0 vis_ms=+400|warn" \
         "$N.500|$N.100/2 $N.105/3 $N.500/2|0>1>0 vis_ms=-400,-395,+0|warn" \
         "23:59:59.500|23:59:59.100/2 23:59:59.105/3 00:00:00.100/2 00:00:00.200/3|0>1>0>1 vis_ms=-400,-395,+600,+700|warn" \
         "00:00:00.200|23:59:59.900/2 23:59:59.950/3|pre:0>1 vis_ms=-300,-250|quiet" \
         "$N.500|$N.100/2 $N.050/3|?|warn" \
         "$N.500|$N.100/2 12:20:00.000/3|?|warn"; do
  IFS='|' read -r tm vis want cmp <<< "$c"
  k=$((k + 1))
  D9MT_TEST_TIMED="$tm" D9MT_TEST_VIS="$vis" ENGINE="$T/engine12" WORK="$T/w12-$k" BENCH_DIR="$T/bench" bash "$ABBA" \
    --variant a="$T/d3d9-a.dll" --variant bbbb="$T/d3d9-bbbb.dll" --rounds 1 fake >/dev/null 2>&1
  got=$(grep -h '^round=' "$T/w12-$k"/results/*.txt 2>/dev/null | sed -n 's/.* visible=\(.*\) RESULT.*/\1/p' | sort -u)
  warned=quiet
  python3 -I "$V2/scripts/perf-compare.py" "$T/w12-$k"/results/*.txt 2>/dev/null | grep -q '^WARNING: runs whose window changed' && warned=warn
  if [ "$got" = "$want" ] && [ "$warned" = "$cmp" ]; then ok "visible= with changes '$vis', timed start $tm: $got (perf-compare: $warned)"
  else bad "visible= with changes '$vis', timed start $tm: want '$want' ($cmp), got '$(echo $got)' ($warned)"; fi
done

[ "$fail" = 0 ] && echo "test-perf-abba: all passed ($ABBA)" || echo "test-perf-abba: $fail FAILED ($ABBA)"
[ "$fail" = 0 ]
