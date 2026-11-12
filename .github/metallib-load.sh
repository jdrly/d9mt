#!/bin/bash
# metallib-load.sh <dir> <target>=<loads|rejects>...
#
# Loads d9mt's AIR metallibs with this Mac's Metal, through the classic API (no Metal 4), and
# says whether this macOS takes them. <dir>/t<target>/<shader>.metallib are the AIR corpus
# (air/tests/bin) compiled for an AIR target by airlib_loopback, the driver's compile path.
#
# For every (vertex shader, pixel shader) pair of scripts/run-air-diff.sh's plain cases,
# .github/metallib-load.m loads both libraries (newLibraryWithData), looks up their entry
# points, builds a render pipeline from them (Metal compiles the AIR here), draws one triangle
# and waits for the command buffer. It judges no pixel.
#   <target>=loads    every pair must get through all of that (the tool's exit status 0);
#   <target>=rejects  every pair must be refused at newLibraryWithData with Metal's own error
#                     (exit status 3). A pair that loads fails the run (this macOS takes that
#                     target after all, so the run can't tell a wrong target), and so does any
#                     other outcome.
#
# What a pass says: this macOS version's Metal accepts the container and the AIR of that target
# and can compile it. On a hosted runner the device is the Apple Paravirtual device, so it says
# nothing about an Apple GPU's own compiler back end, about Metal 4, or about what a frame
# looks like.
#
# Runs under macOS's /bin/bash 3.2. Exit status 1 if any pair misses its expectation.
set -u
cd "$(dirname "$0")/.." || exit 70
dir=${1:?usage: metallib-load.sh <dir> <target>=<loads|rejects>...}
shift
[ $# -gt 0 ] || { echo "usage: metallib-load.sh <dir> <target>=<loads|rejects>..." >&2; exit 64; }
work="$(mktemp -d "${TMPDIR:-/tmp}/metallib-load.XXXXXX")"
trap 'rm -rf "$work"' EXIT

clang -Wall -O1 -fobjc-arc -framework Foundation -framework Metal .github/metallib-load.m \
  -o "$work/metallib-load" || { echo "FAIL: .github/metallib-load.m doesn't build"; exit 1; }

# The plain cases of run-air-diff.sh: "name:vs:ps" with no @variant and no extra arguments.
sed -n 's/^  "[a-z0-9_]*:\(vs_[a-z0-9_]*\):\(ps_[a-z0-9_]*\)"$/\1 \2/p' scripts/run-air-diff.sh | sort -u >"$work/pairs.txt"
npairs=$(wc -l <"$work/pairs.txt" | tr -d ' ')
[ "$npairs" -ge 30 ] || { echo "FAIL: only $npairs shader pairs found in scripts/run-air-diff.sh (its CASES list changed shape?)"; exit 1; }

summary() { [ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$*" >>"$GITHUB_STEP_SUMMARY"; }
echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), $npairs shader pairs"
summary "### AIR metallibs on macOS $(sw_vers -productVersion)"
summary ""

fails=0
for spec in "$@"; do
  target=${spec%%=*} want=${spec#*=}
  case "$want" in loads|rejects) ;; *) echo "FAIL: $spec: expected <target>=loads or <target>=rejects"; exit 64 ;; esac
  [ -d "$dir/t$target" ] || { echo "FAIL: no $dir/t$target"; exit 1; }
  ok=0 rejected=0 other=0 missed=0 reason=""
  while read -r vs ps; do
    out="$work/$target-$vs-$ps.txt"
    rc=0
    "$work/metallib-load" "$dir/t$target/$vs.metallib" "$dir/t$target/$ps.metallib" >"$out" 2>&1 || rc=$?
    case "$rc" in
      0) got=loads; ok=$((ok + 1)) ;;
      3) got=rejects; rejected=$((rejected + 1))
         [ -n "$reason" ] || reason=$(sed -n 's/^newLibraryWithData refused [^:]*: //p' "$out" | head -n 1) ;;
      *) got="exit $rc"; other=$((other + 1)) ;;
    esac
    if [ "$got" != "$want" ]; then
      missed=$((missed + 1))
      echo "FAIL target $target, $vs + $ps: expected '$want', got '$got' (exit $rc):"
      sed 's/^/    /' "$out" | head -n 12
    fi
  done <"$work/pairs.txt"
  line="AIR target $target, expected '$want': $ok pairs load, compile and draw, $rejected refused by newLibraryWithData, $other ended otherwise"
  [ -z "$reason" ] || line="$line. Metal's error: $reason"
  if [ "$missed" = 0 ]; then echo "ok   $line"; summary "- ok: $line"; else echo "FAIL $line"; summary "- **FAIL**: $line"; fails=$((fails + missed)); fi
done
[ "$fails" = 0 ]
