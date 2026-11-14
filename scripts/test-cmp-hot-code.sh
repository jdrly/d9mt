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
#   trap      a nop inside hot::step becomes int3: `changed` (padding is dropped, a trap isn't)
#   lea       a lea of a register into itself (padding) writes another register: `changed`
#   lea32     (x64) that lea at 32 bits, which clears the upper half: `changed`
#   tailtrap  tailfn's last instruction, a branch target, turns from nop into int3: `changed`
#   strmoved  a string literal of a function in front pushes hot::step's string further into
#             .rdata: `moved only` (unnamed data is compared by what it holds, not by its offset)
#   strchange hot::step's string literal changes, at the same length: `changed`
#   double    hot::step multiplies by another unnamed double whose first bytes ("AA", NUL)
#             read like the first one's as a string: `changed`
#   addrstr   built at another image base, hot::step's string names an address at the same
#             place in the image ("0x10001000" against "0x20001000"): `changed` (the string
#             is data, never rewritten as an address)
#   ehmoved   lsdafn's unnamed data in .gcc_except_table (which the linker puts into .text) moves
#             behind a longer block: `moved only`
#   ehchange  that data changes: `changed`
# CXX64 and CXX32 name the compilers (default x86_64-w64-mingw32-g++, i686-w64-mingw32-g++).
set -euo pipefail
V2="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
cat > "$T/hot.cpp" <<'EOF'
#define NI __attribute__((noinline))
// as bytes: the assembler would drop the zero displacement, which the padding form has
#ifdef __x86_64__
#define LEA_SAME ".byte 0x48, 0x8d, 0x76, 0x00"   // lea 0x0(%rsi),%rsi
#define LEA_OTHER ".byte 0x48, 0x8d, 0x7e, 0x00"  // lea 0x0(%rsi),%rdi
#define LEA_32 ".byte 0x8d, 0x76, 0x00"           // lea 0x0(%esi),%esi
#define CLOBBER "rsi", "rdi"
#else
#define LEA_SAME ".byte 0x8d, 0x76, 0x00"         // lea 0x0(%esi),%esi
#define LEA_OTHER ".byte 0x8d, 0x7e, 0x00"        // lea 0x0(%esi),%edi
#define LEA_32 LEA_SAME
#define CLOBBER "esi", "edi"
#endif
extern "C" __declspec(dllexport) int leaf(int x) { return x ^ 0x5a; }   // first: never moves
#if VARIANT == 1
NI int pad_fn(int x) { return x * 7 + 1; }
volatile int pad_var[64];
#endif
volatile int g_a = 1, g_arr[4] = {1, 2, 3, 4};
const char* volatile g_last;
NI void sink(const char* s) { g_last = s; }
#if VARIANT == 10
NI void pad_str(void) { sink("a string in front of the next one"); }
#endif
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
#if VARIANT == 11
  sink("hellp");
#else
  sink("hello");
#endif
#if VARIANT == 12
  x += (int)(g_a * 0x1.0000000004141p+1);   // 0x4000000000004141
#else
  x += (int)(g_a * 0x1.0000000004141p+0);   // 0x3ff0000000004141
#endif
#if VARIANT == 13
  sink("0x20001000");
#else
  sink("0x10001000");
#endif
#if VARIANT == 6
  asm volatile("int3");
#else
  asm volatile("nop");
#endif
#if VARIANT == 7
  asm volatile(LEA_OTHER ::: CLOBBER);
#elif VARIANT == 8
  asm volatile(LEA_32 ::: CLOBBER);
#else
  asm volatile(LEA_SAME ::: CLOBBER);
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
// a function whose last instruction is reached only by its branch
#if VARIANT == 9
#define TAIL "int3"
#else
#define TAIL "nop"
#endif
// unnamed data (a local label) in .gcc_except_table, whose address lsdafn takes
#if VARIANT == 14
#define EH_FRONT "8"
#else
#define EH_FRONT "4"
#endif
#if VARIANT == 15
#define EH_LAST "0x0d0c0b0a"
#else
#define EH_LAST "0x0c0b0a09"
#endif
asm(".section .gcc_except_table,\"dr\"\n .space " EH_FRONT "\n.Lmine:\n .long 0x04030201, 0x08070605, 0x0c0b0a09, " EH_LAST "\n"
    ".text\n.globl lsdafn\nlsdafn:\n mov $.Lmine,%eax\n ret\n");
asm(".text\n.globl tailfn\ntailfn:\n test %ecx,%ecx\n jz 1f\n ret\n1:\n " TAIL "\n.globl tailnext\ntailnext:\n ret\n");
EOF
printf '%s\n' '# the functions under test' hot::step hot::caller leaf tailfn lsdafn > "$T/list.txt"

fails=0
check() {  # <arch> <variant> <expected: function=result[|result] ...>
  local arch=$1 variant=$2 out e f want got; shift 2
  out=$("$V2/scripts/cmp-hot-code.sh" "$T/list.txt" "$T/$arch-0.dll" "$T/$arch-$variant.dll") \
    || { echo "FAIL $arch $variant: cmp-hot-code.sh failed"; fails=$((fails + 1)); return; }
  # one line per listed function: the lambda inside hot::step has no line of its own
  if [ "$(wc -l <<< "$out")" -ne 6 ]; then   # a number: macOS pads wc's output
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
  for v in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    # a fixed image base, which addrstr's strings name
    if [ "$v" = 13 ]; then ib=0x20000000; else ib=0x10000000; fi
    "$cxx" -O2 -g -shared -DVARIANT=$v -Wl,--image-base,$ib "$T/hot.cpp" -o "$T/$arch-$v.dll"
  done
  check "$arch" 0 hot::step=same hot::caller=same leaf=same
  check "$arch" 1 hot::step="moved only" hot::caller="same|moved only" leaf="same|moved only"
  check "$arch" 2 hot::step=changed hot::caller="same|moved only" leaf=same
  check "$arch" 3 hot::step=changed
  check "$arch" 4 hot::step=changed
  check "$arch" 5 hot::step="same|moved only" hot::caller="same|moved only" leaf=same
  check "$arch" 6 hot::step=changed
  check "$arch" 7 hot::step=changed
  if [ "$arch" = x64 ]; then check "$arch" 8 hot::step=changed; fi
  check "$arch" 9 tailfn=changed hot::step="same|moved only"
  check "$arch" 10 hot::step="moved only" hot::caller="same|moved only"
  check "$arch" 11 hot::step=changed
  check "$arch" 12 hot::step=changed
  check "$arch" 13 hot::step=changed hot::caller="same|moved only"
  check "$arch" 14 lsdafn="moved only"
  check "$arch" 15 lsdafn=changed
done
# a name that matches nothing is an error, not an empty table
printf '%s\n' hot::nothing > "$T/none.txt"
if "$V2/scripts/cmp-hot-code.sh" "$T/none.txt" "$T/x64-0.dll" "$T/x64-0.dll" > /dev/null 2>&1; then
  echo "FAIL: a name without a function passed"; fails=$((fails + 1))
else echo "ok   a name without a function fails"; fi
[ "$fails" = 0 ] && echo "test-cmp-hot-code: all passed" || { echo "test-cmp-hot-code: $fails failed"; exit 1; }
