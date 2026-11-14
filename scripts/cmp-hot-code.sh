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
# - padding left out: the nop forms and a lea of a register into itself at full width (an int3
#   is code, a trap, wherever it is);
# - a branch or address inside the function written as the index of the instruction it lands on;
# - every other address inside the image (a call target, a RIP-relative or absolute operand, an
#   address constant) written as the nearest symbol at or below it, plus the offset from it; when
#   that symbol is only a data section's own (.rdata, .gcc_except_table: unnamed data, whose
#   offset moves when anything before it grows), as the section and what lies there: for an
#   operand the instruction reads, the bytes it reads (the width from its mnemonic or register,
#   else 16); for an address it takes (lea, $), the string that starts there (up to 256 bytes),
#   else 16 bytes. A pointer-sized word among those bytes that is an address of the image is
#   written as its symbol. A section symbol with a $ suffix (.rdata$.refptr.<name>) names its
#   contents and counts as a named symbol.
# Raw addresses and displacements are gone; which symbol, at which offset, is kept, so a function
# that reaches other code or named data than before is `changed`. What isn't compared: the
# contents of named data it reads (a variable's value, a jump table), only where they are; and
# anything inlined into it shows up in it, while code it calls is a function of its own.
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


# Padding: only what can't change anything when it runs. The nop forms, and a lea of a register
# into itself at the mode's full width (a 32-bit lea on x64 clears the upper half, so it isn't
# one). int3 is never padding, not even at the end: a branch can land on it.
NOP = re.compile(r'^(?:(?:data16|cs|ds)\s+)*(?:nop[wl]?\b|xchg\s+%ax,%ax$)')
LEA_NOP = {'x86_64': re.compile(r'^lea\s+(?:%cs:)?0x0\(%(r[sd]i)(?:,%riz,1)?\),%\1$'),
           'i686': re.compile(r'^lea\s+(?:%cs:)?0x0\(%(e[sd]i)(?:,%eiz,1)?\),%\1$')}

INSN = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f ]*?)\s*\t(.*)$')
HEX = re.compile(r'(\$?)0x([0-9a-f]+)\b')
TARGET = re.compile(r'\b([0-9a-f]+) <[^>]*>')


REG_WIDTH = {**{r: 8 for r in 'rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15'.split()},
             **{r: 4 for r in 'eax ebx ecx edx esi edi ebp esp r8d r9d r10d r11d r12d r13d r14d r15d'.split()},
             **{r: 2 for r in 'ax bx cx dx si di bp sp r8w r9w r10w r11w r12w r13w r14w r15w'.split()},
             **{r: 1 for r in 'al ah bl bh cl ch dl dh sil dil bpl spl r8b r9b r10b r11b r12b r13b r14b r15b'.split()}}
SUFFIXED = {'mov', 'cmp', 'add', 'sub', 'and', 'or', 'xor', 'test', 'inc', 'dec', 'neg', 'not', 'adc', 'sbb',
            'push', 'pop', 'lock', 'xchg', 'cmpxchg', 'xadd', 'imul', 'shl', 'shr', 'sar', 'sal'}


def access_width(insn):
    """The bytes an instruction with one memory operand reads there, as far as the mnemonic or a
    register operand tells; 16 when it doesn't (more than it reads: a false `changed` at worst)."""
    words = insn.split(' ')
    while len(words) > 1 and words[0] in ('lock', 'rep', 'repz', 'repnz', 'repe', 'repne', 'notrack', 'bnd', 'data16', 'cs', 'ds'):
        words = words[1:]
    mn, ops = words[0], ' '.join(words[1:])
    if m := re.match(r'mov[sz]([bwl])[wlq]$', mn):   # movzbl, movswl, movslq: the source's width
        return {'b': 1, 'w': 2, 'l': 4}[m.group(1)]
    if m := re.match(r'cvt(t?)(ss|sd|si|dq|ps|pd|pi)2', mn):
        return {'ss': 4, 'sd': 8, 'ps': 16, 'pd': 16, 'dq': 16, 'pi': 8}.get(m.group(2)) or (8 if mn.endswith('q') else 4)
    if re.search(r'sd$', mn) and '%xmm' in ops:
        return 8
    if re.search(r'ss$', mn) and '%xmm' in ops:
        return 4
    if mn in ('movq',):
        return 8
    if mn in ('movd',):
        return 4
    if '%xmm' in ops:
        return 16
    if mn.startswith('fi'):     # x87 integer: fild[s|l|ll], fiadd[s|l], ...
        return 8 if mn.endswith(('ll', 'q')) else 2 if mn.endswith('s') else 4
    if mn.startswith('f'):      # x87: s 4, l 8, t 10
        return {'s': 4, 'l': 8, 't': 10}.get(mn[-1], 16)
    if mn[-1:] in 'bwlq' and mn[:-1] in SUFFIXED:
        return {'b': 1, 'w': 2, 'l': 4, 'q': 8}[mn[-1]]
    widths = [REG_WIDTH[r] for r in re.findall(r'%([a-z0-9]+)', re.sub(r'\([^)]*\)', '', ops)) if r in REG_WIDTH]
    return widths[0] if widths else 16


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
        # the loaded sections: an address inside one of them is an address, anything else a number.
        # sections: (start, end, file offset or None, executable), for reading unnamed data
        self.ranges, self.sections = [], []
        lines = tool(self.prefix, 'objdump', '-h', path).splitlines()
        for line, flags in zip(lines, lines[1:] + ['']):
            p = line.split()
            if len(p) >= 6 and p[0].isdigit() and not p[1].startswith('.debug'):
                vma, size = int(p[3], 16), int(p[2], 16)
                self.ranges.append((vma, vma + size))
                self.sections.append((vma, vma + size, int(p[5], 16) if 'CONTENTS' in flags else None, 'CODE' in flags))
        with open(path, 'rb') as f:
            self.data = f.read()
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

    def symbolize(self, v, contents=True, width=None):
        i = bisect.bisect_right(self.addrs, v) - 1
        if i < 0:
            return f'?{v:#x}'
        # Among symbols at one address (aliases), a named one first, then the first by name: the
        # same choice in both builds. A section symbol with a $ suffix names its contents
        # (.text$<function>, .rdata$.refptr.<variable>), so it counts as named.
        a = self.addrs[i]
        names = [n for s, _, n in self.syms if s == a]
        named = [n for n in names if not n.startswith('.') or '$' in n]
        if named or not contents:
            return f'<{min(named or names)}+{v - a:#x}>'
        # Only a section's own symbol below it: unnamed data (a string literal, a constant, an
        # exception table), whose offset moves whenever anything before it in the section grows.
        # What it is, is what it holds. Read by the instruction (width: the bytes it loads):
        # those bytes. Its address taken (width None): the string when it starts one (up to 256
        # bytes), else 16 bytes. Bytes beyond what is read would be a neighbour's, which moves.
        # A pointer-sized word that is an address of the image is written as its symbol (i686
        # keeps pointers in constants).
        # (By the symbol, not the output section: i686's linker puts .gcc_except_table into .text.
        # Only .text's own symbol stands for code.)
        for lo, hi, off, code in self.sections:
            if lo <= v < hi and not min(names).startswith('.text'):
                if off is None:
                    return f'<{min(names)}:zero>'
                start = off + v - lo
                text = self.data[start:start + min(hi - v, 256)]
                nul = text.find(0)
                text = text[:nul] if nul >= 0 else text
                if width is None and len(text) >= 2 and all(32 <= c < 127 or c in (9, 10, 13) for c in text):
                    return f'<{min(names)}:{text.decode()!r}{"" if nul >= 0 else "..."}>'
                raw = self.data[start:start + min(hi - v, width or 16)]
                w = 8 if self.mach == 'x86_64' else 4
                words = [raw[k:k + w] for k in range(0, len(raw), w)]
                return f'<{min(names)}:' + ' '.join(
                    self.symbolize(int.from_bytes(x, 'little'), False) if len(x) == w and self.inside(int.from_bytes(x, 'little'))
                    else x.hex() for x in words) + '>'
        return f'<{min(names)}+{v - a:#x}>'

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
        pad = lambda text: NOP.match(text) or LEA_NOP[self.mach].match(text)
        while insns and pad(insns[-1][2]):
            insns.pop()
        if not insns:
            fail(f'{self.path}: no instructions at {start:#x}')
        real = [i for i in insns if not pad(i[2])]
        # an address inside the function: the index of the first real instruction at or after it
        index = {}
        k = 0
        for a, _, text in insns:
            index[a] = k
            if not pad(text):
                k += 1
        last_end = insns[-1][0] + len(insns[-1][1])

        def addr(v, width=None):
            if start <= v < last_end:
                return f'@{index.get(v, "mid+" + hex(v - start))}'
            return self.symbolize(v, width=width)

        lines = []
        for a, raw, text in real:
            # Each replacement waits in a placeholder until all three passes are done, so that no
            # pass rewrites what an earlier one put in (a string that reads like an address).
            subs = []

            def keep(rep):
                subs.append(rep)
                return f'\x01{len(subs) - 1}\x02'

            # objdump's comment for a RIP-relative operand gives its target: put that in place of
            # the displacement
            m = re.search(r'\s#\s*([0-9a-f]+)(?: <[^>]*>)?$', text)
            if m:
                text = text[:m.start()]
                # a lea takes the address; anything else reads what lies there
                target = keep(addr(int(m.group(1), 16), None if text.startswith('lea') else access_width(text)))
                text = re.sub(r'-?0x[0-9a-f]+\(%rip\)', lambda _: target + '(%rip)', text)
            # a branch or call target, printed as "<hex> <symbol+off>"
            text = TARGET.sub(lambda t: keep(addr(int(t.group(1), 16))), text)
            # any other number that is an address of the image (i686 absolute operands, address
            # constants): with $ an address taken, without it a memory operand read
            text = HEX.sub(lambda h: h.group(1) + keep(addr(int(h.group(2), 16), None if h.group(1) or text.startswith('lea') else access_width(text)))
                           if self.inside(int(h.group(2), 16)) else h.group(0), text)
            lines.append(re.sub('\x01(\\d+)\x02', lambda k: subs[int(k.group(1))], text))
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
