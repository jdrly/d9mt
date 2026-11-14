#!/usr/bin/env bash
# test-cmp-hot-code.sh: scripts/cmp-hot-code.sh on small DLLs built here from one C++ source, for
# x64 and i686 (mingw-w64, no Wine, no GPU). Each variant changes one thing against the base:
#   moved     a function and a variable inserted in front: hot code moves (`moved only`, or `same`
#             where everything it reaches moved with it), nothing of it is `changed`
#   constant  one constant inside hot::step: only that function is `changed`
#   variable  hot::step reads the next element of the same array: `changed`, though only an
#             address differs (the normalization must not hide which data a function reads)
#   callee    hot::step calls another function of the same shape: `changed`
#   lambda    a lambda inside hot::step changes: hot::step isn't `changed`, and the lambda (a
#             symbol whose name starts with hot::step's) isn't taken for it
# CXX64 and CXX32 name the compilers (default x86_64-w64-mingw32-g++, i686-w64-mingw32-g++).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cat > "$T/hot.cpp" <<'EOF'
#define NI __attribute__((noinline))
extern "C" __declspec(dllexport) int leaf(int x) { return x ^ 0x5a; }   // first: never moves
#if VARIANT == 1
NI int pad_fn(int x) { return x * 7 + 1; }
volatile int pad_var[64];
#endif
volatile int g_a = 1, g_arr[4] = {1, 2, 3, 4};
namespace hot {
NI int twice(int x) { return x * 2 + g_a; }
NI int thrice(int x) { return x * 3 + g_a; }
template<class F> NI int apply(F f, int x) { return f(x); }
NI int step(int x) {
#if VARIANT == 2
  x += 41;
#else
  x += 42;
#endif
#if VARIANT == 3
  x ^= g_arr[2];
#else
  x ^= g_arr[1];
#endif
#if VARIANT == 4
  x = thrice(x);
#else
  x = twice(x);
#endif
#if VARIANT == 5
  return apply([](int y) NI { return y - 9; }, x);
#else
  return apply([](int y) NI { return y - 8; }, x);
#endif
}
NI int caller(int x) { return step(x) + twice(x); }
}
extern "C" __declspec(dllexport) int entry(int x) { return hot::caller(x) + leaf(x); }
EOF
printf '%s\n' '# the functions under test' hot::step hot::caller leaf > "$T/list.txt"

fails=0
check() {  # <arch> <variant> <expected: function=result[|result] ...>
  local arch=$1 variant=$2 out e f want got; shift 2
  out=$("$V2/scripts/cmp-hot-code.sh" "$T/list.txt" "$T/$arch-0.dll" "$T/$arch-$variant.dll") \
    || { echo "FAIL $arch $variant: cmp-hot-code.sh failed"; fails=$((fails + 1)); return; }
  # one line per listed function: the lambda inside hot::step has no line of its own
  if [ "$(wc -l <<< "$out")" -ne 4 ]; then   # a number: macOS pads wc's output
    echo "FAIL $arch $variant: not one line per function"; echo "$out"; fails=$((fails + 1))
  fi
  for e in "$@"; do
    f=${e%%=*} want=${e#*=}
    got=$(awk -F'\t' -v f="$f" '$1 == f || index($1, f "(") == 1 { print $6 }' <<< "$out")
    if [ -n "$got" ] && [[ "|$want|" == *"|$got|"* ]]; then echo "ok   $arch $variant: $f $got"
    else echo "FAIL $arch $variant: $f is '$got', expected '$want'"; echo "$out"; fails=$((fails + 1)); fi
  done
}
for arch in x64 i686; do
  if [ "$arch" = x64 ]; then cxx=${CXX64:-x86_64-w64-mingw32-g++}; else cxx=${CXX32:-i686-w64-mingw32-g++}; fi
  for v in 0 1 2 3 4 5; do
    "$cxx" -O2 -g -shared -DVARIANT=$v "$T/hot.cpp" -o "$T/$arch-$v.dll"
  done
  check "$arch" 0 hot::step=same hot::caller=same leaf=same
  check "$arch" 1 hot::step="moved only" hot::caller="same|moved only" leaf="same|moved only"
  check "$arch" 2 hot::step=changed hot::caller="same|moved only" leaf=same
  check "$arch" 3 hot::step=changed
  check "$arch" 4 hot::step=changed
  check "$arch" 5 hot::step="same|moved only" hot::caller="same|moved only" leaf=same
done
# a name that matches nothing is an error, not an empty table
printf '%s\n' hot::nothing > "$T/none.txt"
if "$V2/scripts/cmp-hot-code.sh" "$T/none.txt" "$T/x64-0.dll" "$T/x64-0.dll" > /dev/null 2>&1; then
  echo "FAIL: a name without a function passed"; fails=$((fails + 1))
else echo "ok   a name without a function fails"; fi
[ "$fails" = 0 ] && echo "test-cmp-hot-code: all passed" || { echo "test-cmp-hot-code: $fails failed"; exit 1; }
