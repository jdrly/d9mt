#!/usr/bin/env bash
# test-build-jobs.sh — D9MT_JOBS in build-driver.sh (driver core, libdxbcsm3),
# build-air-compiler.sh (#85) and build-d9mtair.sh (#90), without a compiler and without a real
# build: the scripts run in a fixture tree (the real driver/, bridge/, airlib/ and air/ sources, a
# fake dxbc-spirv, SPIRV-Cross and x86_64 LLVM toolchain, build/ of its own) with stand-ins for
# mingw, clang, clang++, ar, dlltool, objdump, llvm-config, xcrun and python3 on a PATH of the
# system directories. Each stand-in compile records how many compiles run at that moment and its
# argv. Per script:
#   1. the base ref's scripts, D9MT_JOBS unset: the reference;
#   2. the scripts under test with D9MT_JOBS unset, empty, 1 and 2: the build succeeds, at most
#      D9MT_JOBS compiles run at once (the default 12 / 10 when unset or empty; exactly 1 and 2 are
#      reached), and every compile's argv (as a set), the ordered archive/link/run steps and stdout
#      equal the reference's;
#   3. invalid values (0, 08, -1, abc, 1.5, " 2", 1000, a 20-digit number): the script exits
#      nonzero with a D9MT_JOBS message before running any tool;
#   4. one compile fails (#87) in each of the five batch loops (driver core, libdxbcsm3, air
#      compiler, d9mtair's core and front-end objects), with D9MT_JOBS unset, 1 and 2: the third
#      compile of the loop (in the middle of a batch, not its last), and with D9MT_JOBS unset also
#      the first of the last, partial batch. The build exits nonzero with the loop's "compile
#      failed" message as stderr's last line, writes no .flags and runs none of the reference's
#      archive/link/run steps from the loop's own archive or link on.
# Then build-bridge.sh, which runs build-d9mtair.sh first: with D9MT_JOBS=1 it succeeds with one
# compile at a time, and a failed d9mtair compile fails it before winemetal.so's link.
#   scripts/test-build-jobs.sh [base ref (default origin/main)] [ref under test (default: this tree)]
set -uo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${1:-origin/main}" UNDER="${2:-}"
T="$(mktemp -d "${TMPDIR:-/tmp}/d9mt-jobs-test.XXXXXX")"
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=$((fail + 1)); }

# The stand-in toolchain: one script under every tool's name.
mkdir -p "$T/bin"
cat > "$T/bin/stub" <<'EOF'
#!/bin/bash
name=${0##*/} out= src= prev= compile=0
for a in "$@"; do
  { [ "$prev" = -o ] || { [ "$prev" = -l ] && [ "$name" != "${name%-dlltool}" ]; }; } && out=$a
  [ "$prev" = -c ] && src=$a
  [ "$a" = -c ] && compile=1
  prev=$a
done
case "$name" in
  xcrun) exit 1 ;;  # no Metal toolchain: build-driver.sh keeps the committed metallib headers
  llvm-config)
    case "$1" in
      --cxxflags) echo "-I/fake/llvm/include -std=c++14 -fno-exceptions" ;;
      --system-libs) echo "-lz -lcurses" ;;
      *) printf '%s\n' "-L/fake/llvm/lib" "-lLLVMBitWriter -lLLVMCore -lLLVMSupport" ;;  # --ldflags, --libs
    esac
    exit 0 ;;
esac
if [ "$compile" = 1 ]; then
  : > "$REC/running/$$"
  n=$(ls "$REC/running" | wc -l | tr -d ' ')
  echo "$n|$name $(printf '%q ' "$@")" > "$(mktemp "$REC/cc/XXXXXX")"
  sleep 0.2
  rm -f "$REC/running/$$"
  # FAIL: the path suffix of the one source whose compile fails, like a compiler's error
  if [ -n "$FAIL" ] && [ "$src" != "${src%/"$FAIL"}" ]; then echo "$src:1:1: error: stand-in failure" >&2; exit 1; fi
  : > "$out"
  exit 0
fi
echo "$name $(printf '%q ' "$@")" >> "$REC/seq"
case "$name" in
  *-ar) : > "$2" ;;
  *-dlltool) : > "$out" ;;
  *-objdump) ;;
  *) if [ -n "$out" ]; then  # a linked program: when the build runs it, it records its argv
       printf '#!/bin/bash\necho "run ${0##*/} $*" >> "%s/seq"\n' "$REC" > "$out"; chmod +x "$out"
     fi ;;
esac
EOF
chmod +x "$T/bin/stub"
for t in x86_64-w64-mingw32-g++ x86_64-w64-mingw32-gcc x86_64-w64-mingw32-ar x86_64-w64-mingw32-dlltool \
         x86_64-w64-mingw32-objdump i686-w64-mingw32-gcc i686-w64-mingw32-dlltool clang clang++ xcrun \
         llvm-config python3; do ln -s stub "$T/bin/$t"; done
ln -s "$BASH" "$T/bin/bash"  # build-bridge.sh runs build-d9mtair.sh with the bash on PATH (local -n)

# The fixture tree: real sources (read only), fake third-party sources, scripts of the variant.
# build-d9mtair.sh's IR_CORE files (ir/ir.cpp, util/util_log.cpp, ...) are left out on purpose:
# a stand-in compile doesn't read its source, and they would change the other scripts' globs.
F="$T/v2"
mkdir -p "$F/scripts" "$F/third_party/SPIRV-Cross" "$F/toolchains/llvm-x86_64/include" "$F/toolchains/llvm-x86_64/lib"
for l in BitWriter Core Support; do : > "$F/toolchains/llvm-x86_64/lib/libLLVM$l.a"; done
for d in driver bridge airlib air; do ln -s "$V2/$d" "$F/$d"; done
for d in sm3:9 ir:8 ir/passes:5 util:6 spirv:3; do
  mkdir -p "$F/third_party/dxbc-spirv/${d%:*}"
  for i in $(seq "${d#*:}"); do : > "$F/third_party/dxbc-spirv/${d%:*}/f$i.cpp"; done
done
: > "$F/third_party/SPIRV-Cross/spirv_msl.cpp"
cp "$V2/scripts/gen-bridge-def.sh" "$F/scripts/"
printf '#!/usr/bin/env bash\nexit 0\n' > "$F/scripts/check-deps.sh"  # the fake dxbc-spirv has no patches

variant() {  # <ref, or empty for this tree>: put its build scripts into the fixture
  local s
  for s in build-driver.sh build-air-compiler.sh build-d9mtair.sh build-bridge.sh; do
    if [ -z "$1" ]; then cp "$V2/scripts/$s" "$F/scripts/$s"
    else git -C "$V2" show "$1:scripts/$s" > "$F/scripts/$s" || { echo "FATAL: no scripts/$s at $1" >&2; exit 2; }; fi
  done
}

# run <out> <script> [D9MT_JOBS value]: a fresh build/ and record; <out>/{rc,stdout,stderr,cc,seq,max,
# flags}. FAIL (a source's path suffix) makes that compile fail.
run() {
  local out="$1" s="$2"; shift 2
  rm -rf "$F/build" "$T/rec"; mkdir -p "$T/rec/running" "$T/rec/cc" "$out"; : > "$T/rec/seq"
  env -i HOME="$HOME" PATH="$T/bin:/usr/bin:/bin:/usr/sbin:/sbin" REC="$T/rec" FAIL="${FAIL:-}" ARCH=x64 SMOKE=0 \
    LLVM_CONFIG="$T/bin/llvm-config" ${1+D9MT_JOBS="$1"} \
    "$BASH" "$F/scripts/$s" > "$out/stdout" 2> "$out/stderr"
  echo $? > "$out/rc"
  cat "$T/rec/cc"/* 2>/dev/null | cut -d'|' -f2- | LC_ALL=C sort > "$out/cc"
  { cat "$T/rec/cc"/* 2>/dev/null | cut -d'|' -f1; echo 0; } | sort -n | tail -1 > "$out/max"
  cp "$T/rec/seq" "$out/seq"
  find "$F/build" -name .flags > "$out/flags" 2>/dev/null
}

variant "$BASE"
for s in build-driver.sh build-air-compiler.sh build-d9mtair.sh; do run "$T/out/$s/base" "$s"; done
variant "$UNDER"
for s in build-driver.sh build-air-compiler.sh build-d9mtair.sh; do
  case "$s" in build-driver.sh) def=12 ;; *) def=10 ;; esac
  ref="$T/out/$s/base"
  if [ "$(cat "$ref/rc")" != 0 ] || [ ! -s "$ref/cc" ]; then
    bad "$s: the base build ran (exit $(cat "$ref/rc"))"; sed 's/^/       /' "$ref/stderr" | tail -5; continue
  fi
  echo "     $s at $BASE: $(wc -l < "$ref/cc" | tr -d ' ') compiles, at most $(cat "$ref/max") at once"
  for j in unset empty 1 2; do
    o="$T/out/$s/$j"
    case "$j" in unset) run "$o" "$s"; cap=$def ;; empty) run "$o" "$s" ""; cap=$def ;; *) run "$o" "$s" "$j"; cap=$j ;; esac
    max=$(cat "$o/max") what="$s D9MT_JOBS=$j"
    if [ "$(cat "$o/rc")" != 0 ]; then bad "$what: the build fails (exit $(cat "$o/rc"))"; sed 's/^/       /' "$o/stderr" | tail -5; continue; fi
    if [ "$max" -le "$cap" ] && { [ "$j" != 1 ] && [ "$j" != 2 ] || [ "$max" = "$cap" ]; }; then
      ok "$what: at most $max compiles at once (cap $cap)"
    else bad "$what: at most $max compiles at once (cap $cap)"; fi
    for f in cc seq stdout; do
      case "$f" in cc) d="every compile's argv" ;; seq) d="the archive, link and run steps, in order" ;; *) d="stdout" ;; esac
      if cmp -s "$ref/$f" "$o/$f"; then ok "$what: $d equal $BASE's"
      else bad "$what: $d equal $BASE's"; diff "$ref/$f" "$o/$f" | head -6 | cut -c1-200 | sed 's/^/       /'; fi
    done
  done
  for v in 0 08 -1 abc 1.5 " 2" 1000 99999999999999999999; do
    o="$T/out/$s/bad"; run "$o" "$s" "$v"
    if [ "$(cat "$o/rc")" != 0 ] && grep -q D9MT_JOBS "$o/stderr" && [ ! -s "$o/cc" ] && [ ! -s "$o/seq" ]; then
      ok "$s D9MT_JOBS='$v': stops before any tool runs ($(head -1 "$o/stderr"))"
    else bad "$s D9MT_JOBS='$v': stops before any tool runs (exit $(cat "$o/rc"), $(wc -l < "$o/cc" | tr -d ' ') compiles)"; fi
  done
done

# A failed compile in each batch loop (#87). Each loop's sources in compile order: the driver core's
# from the reference's [cc] lines (without sha1.c, compiled after the loop), libdxbcsm3's as the
# script lists them (it prints no [cc] lines), the air compiler's and d9mtair's from their [cc]
# lines (d9mtair's front-end objects: ir_legalize, the passes, sm3 and util_md5).
front='/(ir/ir_legalize|ir/passes/|sm3/|util/util_md5)'
for loop in core sm3 air aircore airfront; do
  case "$loop" in
    core) s=build-driver.sh def=12 msg="FATAL: CORE compile failed" next=libdriver_core.a
          srcs=$(awk '/CORE compile/ {on = 1} /archive libdriver_core.a/ {on = 0} on && /^\[cc\] / {print "driver/" substr($0, 6)}' \
                   "$T/out/$s/base/stdout" | grep -vx driver/util/sha1/sha1.c) ;;
    sm3)  s=build-driver.sh def=12 msg="FATAL: libdxbcsm3 compile failed" next=libdxbcsm3.a
          srcs=$(cd "$F" && find -L third_party/dxbc-spirv/sm3 third_party/dxbc-spirv/ir third_party/dxbc-spirv/util -name '*.cpp' | LC_ALL=C sort) ;;
    air)  s=build-air-compiler.sh def=10 msg="FATAL: air compiler compile failed" next=air_compiler
          srcs=$(sed -n 's/^\[cc\] //p' "$T/out/$s/base/stdout") ;;
    aircore)
          s=build-d9mtair.sh def=10 msg="FATAL: d9mtair compile failed" next=d9mtair.so
          srcs=$(sed -n 's/^\[cc\] //p' "$T/out/$s/base/stdout" | grep -Ev "$front") ;;
    *)    s=build-d9mtair.sh def=10 msg="FATAL: d9mtair compile failed" next=d9mtair.so
          srcs=$(sed -n 's/^\[cc\] //p' "$T/out/$s/base/stdout" | grep -E "$front") ;;
  esac
  ref="$T/out/$s/base" n=$(printf '%s\n' "$srcs" | wc -l | tr -d ' ')
  # the reference's steps before the loop's own archive or link: all a failed build may run
  awk -v p="$next" 'index($0, p) {exit} {print}' "$ref/seq" > "$T/expect"
  for j in unset 1 2; do
    case "$j" in unset) cap=$def ;; *) cap=$j ;; esac
    ps=3; [ "$j" = unset ] && [ $((n % cap)) -ge 2 ] && ps="3 $((n - n % cap + 1))"
    for p in $ps; do
      fsrc=$(printf '%s\n' "$srcs" | sed -n "${p}p") o="$T/out/$s/fail"
      if [ "$j" = unset ]; then FAIL=$fsrc run "$o" "$s"; else FAIL=$fsrc run "$o" "$s" "$j"; fi
      what="$s $loop D9MT_JOBS=$j: compile $p of $n (${fsrc#*/}, batch $(( (p - 1) / cap + 1 )) of $(( (n + cap - 1) / cap ))) fails"
      last=$(tail -1 "$o/stderr") why=
      [ "$(cat "$o/rc")" != 0 ] || why+=", exit 0"
      [ "$last" = "$msg" ] || why+=", stderr ends '$last'"
      [ ! -s "$o/flags" ] || why+=", .flags written"
      cmp -s "$T/expect" "$o/seq" || why+=", $(wc -l < "$o/seq" | tr -d ' ') archive/link/run steps, $(wc -l < "$T/expect" | tr -d ' ') expected"
      if [ -z "$why" ]; then ok "$what: stops with '$msg', no .flags, no archive or link"
      else bad "$what: stops with '$msg', no .flags, no archive or link (${why#, })"; fi
    done
  done
done

# build-bridge.sh (the variant's, with its build-d9mtair.sh): D9MT_JOBS reaches the d9mtair
# compiles, and their failure stops the bridge build before its own link.
s=build-bridge.sh o="$T/out/$s/1"; run "$o" "$s" 1
if [ "$(cat "$o/rc")" = 0 ] && [ "$(cat "$o/max")" = 1 ] && grep -q winemetal.so "$o/seq"; then
  ok "$s D9MT_JOBS=1: builds winemetal.so with one compile at a time"
else bad "$s D9MT_JOBS=1: builds winemetal.so with one compile at a time (exit $(cat "$o/rc"), at most $(cat "$o/max") at once)"; fi
fsrc=$(sed -n 's/^\[cc\] //p' "$T/out/build-d9mtair.sh/base/stdout" | sed -n 3p) o="$T/out/$s/fail"
FAIL=$fsrc run "$o" "$s"
if [ "$(cat "$o/rc")" != 0 ] && [ "$(tail -1 "$o/stderr")" = "FATAL: d9mtair compile failed" ] && ! grep -q winemetal "$o/seq"; then
  ok "$s: a failed d9mtair compile (${fsrc#*/}) stops it before winemetal.so's link"
else bad "$s: a failed d9mtair compile (${fsrc#*/}) stops it before winemetal.so's link (exit $(cat "$o/rc"), stderr ends '$(tail -1 "$o/stderr")')"; fi

under="${UNDER:-this tree}"
[ "$fail" = 0 ] && echo "test-build-jobs: all passed ($under against $BASE)" || echo "test-build-jobs: $fail FAILED ($under against $BASE)"
[ "$fail" = 0 ]
