#!/usr/bin/env bash
# cmp-hot-code.sh: compares the machine code of named functions in two builds of a PE DLL (base
# and head d3d9.dll, x64 or i686), function by function, so that a change that claims to leave
# the hot paths alone can show it.
#
#   scripts/cmp-hot-code.sh <functions file> <base dll> <head dll> [<listing dir>]
#
# <functions file>: one qualified C++ name per line (`dxvk::D3D9DeviceEx::PrepareDraw`), # starts
# a comment. A name stands for every function whose demangled name is that name followed by its
# parameter list: overloads each get a line of their own.
#
# Output on stdout, tab-separated, a header line first:
#   function  base_bytes  head_bytes  base_insns  head_insns  result  insn_diff
# function is the demangled signature. bytes is the span from the function's start to the end of
# its last instruction that isn't padding; insns counts its instructions without padding.
# result is one of
#   same        the same instruction bytes
#   moved only  other bytes, the same normalized code: what differs is where things lie (the
#               function's address, its targets' addresses, alignment padding)
#   changed     the normalized code differs; insn_diff is head's instruction count minus base's
#   only base, only head   the function exists in one build only
# The normalized code is the disassembly (GNU objdump), each instruction with
# - padding instructions (nop forms, int3) left out;
# - a branch or address inside the function written as the index of the instruction it lands on;
# - every other address inside the image (a call target, a RIP-relative or absolute operand, an
#   address constant) written as the nearest symbol at or below it, plus the offset from it.
# Raw addresses and displacements are gone; which symbol, at which offset, is kept, so a function
# that reaches other code or data than before is `changed`. What isn't compared: the contents of
# the data it reads (a constant in .rdata, a jump table), only where they are; and anything
# inlined into it shows up in it, while code it calls is a function of its own.
# With <listing dir>, each function's normalized code goes to <dir>/base/<n>.s and <dir>/head/<n>.s
# (n: its line in the output, from 1), so `diff -r` shows what changed.
#
# Tools: <prefix>objdump, <prefix>nm and <prefix>c++filt, the prefix x86_64-w64-mingw32- or
# i686-w64-mingw32- after the DLL's machine, or BINUTILS_PREFIX when that is set (empty: the
# host's own binutils, when they read PE).
# Exit status: 0 the table was written (whatever it says); 1 a tool failed, or a name of the file
# matched no function in either build; 2 usage.
set -euo pipefail
[ $# -ge 3 ] && [ $# -le 4 ] || { sed -n '2,36p' "$0" >&2; exit 2; }
for f in "$1" "$2" "$3"; do [ -f "$f" ] || { echo "cmp-hot-code: no file $f" >&2; exit 2; }; done
exec python3 -I - "$@" <<'PY'
import bisect, os, re, subprocess, sys

names_file, base_dll, head_dll = sys.argv[1:4]
listing = sys.argv[4] if len(sys.argv) > 4 else None


def fail(msg):
    print(f'cmp-hot-code: {msg}', file=sys.stderr)
    sys.exit(1)


def machine(path):
    with open(path, 'rb') as f:
        head = f.read(4096)
    if head[:2] != b'MZ':
        fail(f'{path} is no PE file')
    pe = int.from_bytes(head[0x3c:0x40], 'little')
    if head[pe:pe + 4] != b'PE\0\0':
        fail(f'{path} is no PE file')
    m = int.from_bytes(head[pe + 4:pe + 6], 'little')
    return {0x8664: 'x86_64', 0x14c: 'i686'}.get(m) or fail(f'{path}: machine {m:#x} is neither x64 nor i686')


def tool(prefix, name, *args, stdin=None):
    r = subprocess.run([prefix + name, *args], input=stdin, capture_output=True, text=True, errors='replace')
    if r.returncode != 0:
        fail(f'{prefix}{name} {" ".join(args)}: exit {r.returncode}: {r.stderr.strip()[:400]}')
    return r.stdout


PAD = re.compile(r'^(?:(?:data16|cs|ds)\s+)*(?:nop[wl]?\b|xchg\s+%ax,%ax$|int3$|'
                 r'lea\s+(?:%cs:)?0x0\(%[er]?[sd]i(?:,%[er]iz,1)?\),%[er]?[sd]i$)')
INSN = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f ]*?)\s*\t(.*)$')
HEX = re.compile(r'(\$?)0x([0-9a-f]+)\b')
TARGET = re.compile(r'\b([0-9a-f]+) <[^>]*>')


def is_function(demangled, name):
    """name(parameters) with nothing after the list but qualifiers: not a lambda or a static
    variable inside that function, whose names start the same way."""
    if demangled == name:   # a C function
        return True
    if not demangled.startswith(name + '('):
        return False
    depth = 0
    for i in range(len(name), len(demangled)):
        depth += {'(': 1, ')': -1}.get(demangled[i], 0)
        if depth == 0:
            return re.fullmatch(r'(?: const)?(?: volatile)?(?: &&?)?(?: \[clone [^\]]+\])*', demangled[i + 1:]) is not None
    return False


class Image:
    def __init__(self, path):
        self.path = path
        self.mach = machine(path)
        env = os.environ.get('BINUTILS_PREFIX')
        self.prefix = env if env is not None else f'{self.mach}-w64-mingw32-'
        # the loaded sections: an address inside one of them is an address, anything else a number
        self.ranges = []
        for line in tool(self.prefix, 'objdump', '-h', path).splitlines():
            p = line.split()
            if len(p) >= 4 and p[0].isdigit() and not p[1].startswith('.debug'):
                vma, size = int(p[3], 16), int(p[2], 16)
                self.ranges.append((vma, vma + size))
        syms = []
        for line in tool(self.prefix, 'nm', '-n', path).splitlines():
            p = line.split()
            if len(p) == 3 and p[1] not in 'UuwvNn' and re.fullmatch(r'[0-9a-f]+', p[0]):
                syms.append((int(p[0], 16), p[1], p[2]))
        if not syms:
            fail(f'{path} has no symbol table')
        self.syms = syms
        self.addrs = [s[0] for s in syms]
        texts = sorted({a for a, t, _ in syms if t in 'Tt'})
        self.text_ends = {a: b for a, b in zip(texts, texts[1:])}
        self.text_ends[texts[-1]] = max(e for _, e in self.ranges)
        # demangled names of the text symbols; i686 adds a leading underscore (taken off here, so
        # c++filt -n) and stdcall's @n
        mangled = sorted({n for _, t, n in syms if t in 'Tt'})
        plain = [re.sub(r'@\d+$', '', n[1:] if self.mach == 'i686' and n.startswith('_') else n) for n in mangled]
        out = tool(self.prefix, 'c++filt', '-n', stdin='\n'.join(plain) + '\n').splitlines()
        if len(out) != len(plain):
            fail('c++filt gave a different number of lines')
        self.demangled = dict(zip(mangled, out))

    def inside(self, v):
        return any(lo <= v < hi for lo, hi in self.ranges)

    def symbolize(self, v):
        i = bisect.bisect_right(self.addrs, v) - 1
        if i < 0:
            return f'?{v:#x}'
        # among symbols at one address (aliases), the first by name: the same choice in both builds
        a = self.addrs[i]
        name = min(n for s, _, n in self.syms if s == a)
        return f'<{name}+{v - a:#x}>'

    def functions(self, wanted):
        """{signature: (start, end)} of the text symbols that are one of the wanted functions."""
        found = {}
        for a, t, n in self.syms:
            if t in 'Tt' and any(is_function(self.demangled[n], w) for w in wanted):
                found.setdefault(self.demangled[n], (a, self.text_ends[a]))
        return found

    def code(self, start, end):
        """(bytes without trailing padding, normalized instruction lines)"""
        out = tool(self.prefix, 'objdump', '-d', '--insn-width=16', f'--start-address={start:#x}',
                   f'--stop-address={end:#x}', self.path)
        insns = []
        for line in out.splitlines():
            m = INSN.match(line)
            if m:
                text = re.sub(r'\s+', ' ', m.group(3).strip())
                insns.append((int(m.group(1), 16), bytes.fromhex(m.group(2).replace(' ', '')), text))
        while insns and PAD.match(insns[-1][2]):
            insns.pop()
        if not insns:
            fail(f'{self.path}: no instructions at {start:#x}')
        real = [i for i in insns if not PAD.match(i[2])]
        # an address inside the function: the index of the first real instruction at or after it
        index = {}
        k = 0
        for a, _, text in insns:
            index[a] = k
            if not PAD.match(text):
                k += 1
        last_end = insns[-1][0] + len(insns[-1][1])

        def addr(v):
            if start <= v < last_end:
                return f'@{index.get(v, "mid+" + hex(v - start))}'
            return self.symbolize(v)

        lines = []
        for a, raw, text in real:
            # objdump's comment for a RIP-relative operand gives its target: put that in place of
            # the displacement
            m = re.search(r'\s#\s*([0-9a-f]+)(?: <[^>]*>)?$', text)
            if m:
                text = text[:m.start()]
                text = re.sub(r'-?0x[0-9a-f]+\(%rip\)', addr(int(m.group(1), 16)) + '(%rip)', text)
            # a branch or call target, printed as "<hex> <symbol+off>"
            text = TARGET.sub(lambda t: addr(int(t.group(1), 16)), text)
            # any other number that is an address of the image (i686 absolute operands, address constants)
            text = HEX.sub(lambda h: h.group(1) + addr(int(h.group(2), 16)) if self.inside(int(h.group(2), 16)) else h.group(0), text)
            lines.append(text)
        code = b''.join(r for _, r, t in insns)
        return code, lines, last_end - start


wanted = []
for line in open(names_file):
    line = line.split('#', 1)[0].strip()
    if line:
        wanted.append(line)
if not wanted:
    fail(f'{names_file} names no function')
base, head = Image(base_dll), Image(head_dll)
if base.mach != head.mach:
    fail(f'base is {base.mach}, head {head.mach}')
fb, fh = base.functions(wanted), head.functions(wanted)
missing = [w for w in wanted if not any(is_function(s, w) for s in list(fb) + list(fh))]
if missing:
    fail(f'no function in either build for: {", ".join(missing)}')

order = sorted(set(fb) | set(fh), key=lambda s: (next(i for i, w in enumerate(wanted) if is_function(s, w)), s))
print('function\tbase_bytes\thead_bytes\tbase_insns\thead_insns\tresult\tinsn_diff')
for n, sig in enumerate(order, 1):
    cb = base.code(*fb[sig]) if sig in fb else None
    ch = head.code(*fh[sig]) if sig in fh else None
    if listing:
        for side, c in (('base', cb), ('head', ch)):
            if c:
                os.makedirs(os.path.join(listing, side), exist_ok=True)
                with open(os.path.join(listing, side, f'{n}.s'), 'w') as f:
                    f.write(f'# {sig}\n' + '\n'.join(c[1]) + '\n')
    if cb is None or ch is None:
        result = 'only head' if cb is None else 'only base'
    elif cb[1] != ch[1]:
        result = 'changed'
    elif cb[0] == ch[0]:
        result = 'same'
    else:
        result = 'moved only'
    cols = [str(c[2]) if c else '-' for c in (cb, ch)] + [str(len(c[1])) if c else '-' for c in (cb, ch)]
    diff = f'{len(ch[1]) - len(cb[1]):+d}' if cb and ch and result == 'changed' else ''
    print('\t'.join([sig] + cols + [result, diff]))
PY
