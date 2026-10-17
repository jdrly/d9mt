#!/usr/bin/env python3
"""SM3 (vs_3_0 / ps_3_0) assembler and random shader generator for the
cross-architecture AIR tests (scripts/run-air-xarch.sh).

  sm3_corpus.py asm <in.asm> <out.bin>     assemble one file
  sm3_corpus.py check                      assemble air/tests/asm/*.asm and
                                           test/shaders/*.asm and compare with
                                           the D3DX output committed next to them
  sm3_corpus.py gen <outdir> <count> [seed] write <count> random shaders

The assembler covers the subset the generator and the committed test shaders
use; `check` proves it matches D3DXAssembleShader byte for byte on those.
Generated shaders follow the D3D9 SM3 rules closely enough for dxbc-spirv's
sm3 front end; they exist to exercise the compile path with many opcodes,
swizzles, modifiers, control flow and special float constants (NaN payloads,
infinities, signed zeros, denormals, half-float edge values), not to render
anything sensible.
"""
import os
import random
import re
import struct
import sys

# --- encoding ---------------------------------------------------------------

OPCODES = {
    'nop': 0, 'mov': 1, 'add': 2, 'sub': 3, 'mad': 4, 'mul': 5, 'rcp': 6,
    'rsq': 7, 'dp3': 8, 'dp4': 9, 'min': 10, 'max': 11, 'slt': 12, 'sge': 13,
    'exp': 14, 'log': 15, 'lit': 16, 'dst': 17, 'lrp': 18, 'frc': 19,
    'm4x4': 20, 'm4x3': 21, 'm3x4': 22, 'm3x3': 23, 'm3x2': 24,
    'call': 25, 'callnz': 26, 'loop': 27, 'ret': 28, 'endloop': 29,
    'label': 30, 'dcl': 31, 'pow': 32, 'crs': 33, 'sgn': 34, 'abs': 35,
    'nrm': 36, 'sincos': 37, 'rep': 38, 'endrep': 39, 'if': 40, 'ifc': 41,
    'else': 42, 'endif': 43, 'break': 44, 'breakc': 45, 'mova': 46,
    'defb': 47, 'defi': 48, 'texkill': 65, 'texld': 66, 'expp': 78,
    'logp': 79, 'cnd': 80, 'def': 81, 'cmp': 88, 'dp2add': 90, 'dsx': 91,
    'dsy': 92, 'texldd': 93, 'setp': 94, 'texldl': 95, 'breakp': 96,
}
COMPARE = {'gt': 1, 'eq': 2, 'ge': 3, 'lt': 4, 'ne': 5, 'le': 6}
USAGE = {'position': 0, 'blendweight': 1, 'blendindices': 2, 'normal': 3,
         'psize': 4, 'texcoord': 5, 'tangent': 6, 'binormal': 7,
         'tessfactor': 8, 'positiont': 9, 'color': 10, 'fog': 11,
         'depth': 12, 'sample': 13}
SAMPLER_TYPE = {'2d': 2, 'cube': 3, 'volume': 4}
SRCMOD = {'': 0, 'neg': 1, 'bias': 2, 'negbias': 3, 'bx2': 4, 'negbx2': 5,
          'comp': 6, 'x2': 7, 'negx2': 8, 'dz': 9, 'dw': 10, 'abs': 11,
          'negabs': 12, 'not': 13}
COMP = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3}


def regtype(name, stage):
    """register prefix -> (D3DSPR type, index-carrying)"""
    return {
        'r': 0, 'v': 1, 'c': 2, 'a': 3, 't': 3, 'o': 6, 'i': 7, 'oc': 8,
        'odepth': 9, 's': 10, 'b': 14, 'al': 15, 'vpos': 17, 'vface': 17,
        'p': 19, 'l': 18,
    }[name]


def type_bits(t):
    return ((t & 7) << 28) | ((t >> 3) << 11)


REG_RE = re.compile(r'^(oC|oDepth|vPos|vFace|aL|[rvcatoisbpl])(\d*)$', re.I)


def parse_reg(tok):
    m = REG_RE.match(tok)
    if not m:
        raise ValueError(f'bad register {tok!r}')
    name, num = m.group(1).lower(), m.group(2)
    if name == 'vpos':
        return 17, 0
    if name == 'vface':
        return 17, 1
    return regtype(name, None), int(num) if num else 0


def swizzle_bits(sw):
    if not sw:
        return 0xE4
    comps = [COMP[c] for c in sw.lower()]
    while len(comps) < 4:
        comps.append(comps[-1])
    return comps[0] | (comps[1] << 2) | (comps[2] << 4) | (comps[3] << 6)


def mask_bits(m):
    if not m:
        return 0xF
    v = 0
    for c in m.lower():
        v |= 1 << COMP[c]
    return v


def enc_dst(tok, mod=0, shift=0):
    reg, _, mask = tok.partition('.')
    t, n = parse_reg(reg)
    return [0x80000000 | type_bits(t) | n | (mask_bits(mask) << 16) | (mod << 20) | (shift << 24)]


SRC_RE = re.compile(r'^(-)?(!)?(1-)?([A-Za-z]+)(\d*)(?:\[([^\]]+)\])?(_abs|_bias|_bx2|_x2|_dz|_dw)?(?:\.([xyzwrgba]{1,4}))?$')


def enc_src(tok):
    tok = tok.replace(' ', '')
    m = SRC_RE.match(tok)
    if not m:
        raise ValueError(f'bad source {tok!r}')
    neg, nt, comp, name, num, rel, suffix, sw = m.groups()
    rel_tok = []
    t, n = parse_reg(name + (num or ''))
    if rel:
        # c[aL + N] / c[a0.x + N] / c[aL]
        parts = rel.split('+')
        base = parts[0]
        if len(parts) > 1:
            n = int(parts[1])
        breg, _, bsw = base.partition('.')
        bt, bn = parse_reg(breg)
        rel_tok = [0x80000000 | type_bits(bt) | bn | (swizzle_bits(bsw) << 16)]
    mod = ''
    if suffix:
        mod = suffix[1:]
    if nt:
        mod = 'not'
    elif comp:
        mod = 'comp'
    elif neg:
        mod = 'neg' + mod
    return [0x80000000 | type_bits(t) | n | (0x2000 if rel else 0)
            | (swizzle_bits(sw) << 16) | (SRCMOD[mod] << 24)] + rel_tok


def f2u(x):
    if isinstance(x, int):
        return x & 0xffffffff
    return struct.unpack('<I', struct.pack('<f', x))[0]


def parse_float_token(s):
    s = s.strip()
    if s.startswith('0x') or s.startswith('0X'):
        return int(s, 16)  # raw bits (generator output)
    return f2u(float(s))


def assemble(text):
    toks = []
    stage = None
    for raw in text.splitlines():
        line = raw.split(';')[0].split('//')[0].strip()
        if not line:
            continue
        pred = []
        predflag = 0
        mp = re.match(r'^\((!?)p0(?:\.([xyzw]))?\)\s*(.*)$', line)
        if mp:
            line = mp.group(3)
            predflag = 1 << 28
            sw = mp.group(2) or ''
            pred = [0x80000000 | type_bits(19) | (swizzle_bits(sw) << 16) | ((13 if mp.group(1) else 0) << 24)]
        op, _, rest = line.partition(' ')
        args = [a.strip() for a in split_args(rest)] if rest.strip() else []
        op = op.lower()
        if op in ('vs_3_0', 'ps_3_0'):
            stage = op[0]
            toks.append(0xFFFE0300 if stage == 'v' else 0xFFFF0300)
            continue
        if op.startswith('dcl'):
            usage = op[4:] if op.startswith('dcl_') else ''
            if usage in SAMPLER_TYPE:
                ut = 0x80000000 | (SAMPLER_TYPE[usage] << 27)
            elif usage:
                m = re.match(r'^([a-z]+?)(\d*)$', usage)
                ut = 0x80000000 | USAGE[m.group(1)] | ((int(m.group(2)) if m.group(2) else 0) << 16)
            else:
                ut = 0x80000000
            body = [ut] + enc_dst(args[0])
            toks += [(0x1F | (len(body) << 24))] + body
            continue
        if op == 'def':
            body = enc_dst(args[0]) + [parse_float_token(a) for a in args[1:5]]
            toks += [0x51 | (len(body) << 24)] + body
            continue
        if op == 'defi':
            body = enc_dst(args[0]) + [int(a) & 0xffffffff for a in args[1:5]]
            toks += [0x30 | (len(body) << 24)] + body
            continue
        if op == 'defb':
            body = enc_dst(args[0]) + [1 if args[1].lower() in ('true', '1') else 0]
            toks += [0x2F | (len(body) << 24)] + body
            continue
        dmod = 0
        base = op
        ctrl = 0
        while True:
            m = re.match(r'^(.*)_(sat|pp|centroid)$', base)
            if not m:
                break
            base = m.group(1)
            dmod |= {'sat': 1, 'pp': 2, 'centroid': 4}[m.group(2)]
        m = re.match(r'^(if|break|setp)_(gt|eq|ge|lt|ne|le)$', base)
        if m:
            base = {'if': 'ifc', 'break': 'breakc', 'setp': 'setp'}[m.group(1)]
            ctrl = COMPARE[m.group(2)]
        if base == 'texldp':
            base, ctrl = 'texld', 1
        elif base == 'texldb':
            base, ctrl = 'texld', 2
        if base == 'end':
            break
        if base == 'sub':
            # D3DX assembles sub a, b, c as add a, b, -c
            base = 'add'
            args[-1] = args[-1][1:] if args[-1].startswith('-') else '-' + args[-1]
        code = OPCODES[base]
        has_dst = base not in ('if', 'ifc', 'else', 'endif', 'rep', 'endrep', 'loop',
                               'endloop', 'break', 'breakc', 'breakp', 'ret',
                               'call', 'callnz', 'label', 'nop')
        body = []
        if has_dst and args:
            body += enc_dst(args[0], dmod)
            srcs = args[1:]
        else:
            srcs = args
        body += pred
        for s in srcs:
            body += enc_src(s)
        toks += [code | (ctrl << 16) | predflag | (len(body) << 24)] + body
    toks.append(0x0000FFFF)
    return struct.pack('<%dI' % len(toks), *toks)


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '[':
            depth += 1
        elif ch == ']':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


# --- check against the committed D3DX output --------------------------------

def check(root):
    fails = 0
    n = 0
    for d, ref in (('air/tests/asm', 'bin'), ('test/shaders', 'inc')):
        for f in sorted(os.listdir(os.path.join(root, d))):
            if not f.endswith('.asm'):
                continue
            name = f[:-4]
            if ref == 'bin':
                rp = os.path.join(root, 'air/tests/bin', name + '.bin')
                expected = open(rp, 'rb').read()
            else:
                rp = os.path.join(root, 'test', name + '_bytecode.inc')
                expected = bytes(int(x) for x in open(rp).read().strip().split(','))
            got = assemble(open(os.path.join(root, d, f)).read())
            n += 1
            if got != expected:
                fails += 1
                print(f'MISMATCH {d}/{f}')
    print(f'{n - fails}/{n} assemble byte-identical to D3DX')
    return fails == 0


# --- generator --------------------------------------------------------------

F32 = lambda bits: '0x%08x' % bits
SPECIAL = [
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x40490fdb,
    0x3eaaaaab, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00000,
    0x7f800001, 0x7fffffff, 0x00000001, 0x80000001, 0x007fffff, 0x00800000,
    0x7f7fffff, 0xff7fffff, 0x477fe000, 0x477ff000, 0x47800000, 0x38800000,
    0x33800000, 0x33000000, 0x387fc000, 0x4b000000, 0x4b800000, 0xcf000000,
    0x4f800000, 0x3dcccccd, 0x2edbe6ff,
]


class Gen:
    def __init__(self, rnd):
        self.r = rnd
        self.lines = []

    def ch(self, seq):
        return self.r.choice(seq)

    def const_val(self):
        k = self.r.random()
        if k < 0.45:
            return F32(self.ch(SPECIAL))
        if k < 0.75:
            return F32(f2u(self.r.uniform(-8.0, 8.0)))
        if k < 0.9:
            return F32(f2u(self.r.choice([-1, 1]) * 10 ** self.r.uniform(-40, 38)))
        return F32(self.r.getrandbits(32))

    def swz(self, n=4):
        if n == 1:
            return '.' + self.ch('xyzw')
        k = self.r.random()
        if k < 0.4:
            return ''
        if k < 0.6:
            return '.' + self.ch('xyzw')
        return '.' + ''.join(self.ch('xyzw') for _ in range(4))

    def mask(self):
        k = self.r.random()
        if k < 0.5:
            return ''
        comps = [c for c in 'xyzw' if self.r.random() < 0.5] or ['x']
        return '.' + ''.join(comps)

    def srcmod(self, reg, allow_abs=True):
        k = self.r.random()
        if k < 0.7:
            return reg
        if k < 0.85 or not allow_abs:
            return '-' + reg
        return ('-' if self.r.random() < 0.5 else '') + reg + '_abs'

    def gen(self):
        r = self.r
        ps = r.random() < 0.5
        L = self.lines
        L.append('ps_3_0' if ps else 'vs_3_0')
        self.ps = ps
        inputs = []
        if ps:
            nin = r.randint(1, 6)
            used = set()
            for i in range(nin):
                while True:
                    u = self.ch(['texcoord%d' % r.randint(0, 7), 'color%d' % r.randint(0, 1)])
                    if u not in used:
                        break
                used.add(u)
                # full mask: reading undeclared components trips dxbc-spirv asserts
                L.append('dcl_%s v%d' % (u, i))
                inputs.append('v%d' % i)
            if r.random() < 0.2:
                L.append('dcl vPos.xy')
                inputs.append('vPos.xyyy')  # .xyxy trips a dxbc-spirv assert (both archs)
            if r.random() < 0.2:
                L.append('dcl vFace')
                inputs.append('vFace.xxxx')
            samplers = []
            for i in range(r.randint(0, 4)):
                t = self.ch(['2d', '2d', 'cube', 'volume'])
                L.append('dcl_%s s%d' % (t, i))
                samplers.append(('s%d' % i, t))
            outputs = ['oC%d' % i for i in range(r.randint(1, 4))]
            if r.random() < 0.15:
                outputs.append('oDepth')
        else:
            nin = r.randint(1, 6)
            L.append('dcl_position v0')
            inputs.append('v0')
            used = {('position', 0)}
            for i in range(1, nin):
                while True:
                    u = (self.ch(['color', 'texcoord', 'normal', 'tangent', 'blendweight', 'binormal']), r.randint(0, 3))
                    if u not in used:
                        break
                used.add(u)
                L.append('dcl_%s%d v%d' % (u[0], u[1], i))
                inputs.append('v%d' % i)
            samplers = []
            if r.random() < 0.15:
                L.append('dcl_2d s0')
                samplers.append(('s0', '2d'))
            L.append('dcl_position o0')
            outputs = ['o0']
            oused = set()
            for i in range(1, r.randint(1, 8)):
                while True:
                    u = self.ch(['texcoord%d' % r.randint(0, 7), 'color%d' % r.randint(0, 1)])
                    if u not in oused:
                        break
                oused.add(u)
                L.append('dcl_%s o%d' % (u, i))
                outputs.append('o%d' % i)
            if r.random() < 0.15:
                L.append('dcl_psize o%d.x' % len(outputs))
                outputs.append('o%d.x' % len(outputs))
        ndef = r.randint(1, 6)
        defs = []
        for i in range(ndef):
            c = r.randint(0, 31) if r.random() < 0.7 else r.randint(32, 220 if ps else 250)
            if c in defs:
                continue
            defs.append(c)
            L.append('def c%d, %s' % (c, ', '.join(self.const_val() for _ in range(4))))
        self.consts = ['c%d' % c for c in defs] + ['c%d' % r.randint(0, 31) for _ in range(3)]
        if r.random() < 0.4:
            L.append('defi i0, %d, %d, %d, 0' % (r.randint(0, 4), r.randint(-2, 3), r.randint(-2, 2)))
        ntemps = r.randint(2, 8)
        self.temps = ['r%d' % i for i in range(ntemps)]
        for t in self.temps:
            L.append('mov %s, %s' % (t, self.ch(inputs + self.consts)))
        self.inputs = inputs
        self.samplers = samplers
        depth = 0
        loops = 0
        n = r.randint(4, 40)
        for _ in range(n):
            k = r.random()
            if k < 0.08 and depth < 2:
                cc = self.ch(list(COMPARE))
                L.append('if_%s %s, %s' % (cc, self.anysrc(1), self.anysrc(1)))
                self.body(r.randint(1, 4))
                if r.random() < 0.5:
                    L.append('else')
                    self.body(r.randint(1, 4))
                L.append('endif')
            elif k < 0.12 and depth < 2 and loops < 2 and 'defi i0' in ' '.join(L):
                loops += 1
                if r.random() < 0.5 or ps:
                    L.append('rep i0')
                    self.body(r.randint(1, 4))
                    if r.random() < 0.3:
                        L.append('break_%s %s, %s' % (self.ch(list(COMPARE)), self.anysrc(1), self.anysrc(1)))
                    L.append('endrep')
                else:
                    L.append('loop aL, i0')
                    self.body(r.randint(1, 3), rel='aL')
                    L.append('endloop')
            elif k < 0.16 and not ps:
                L.append('mova a0.x, %s' % self.anysrc(1))
                L.append('add %s, %s, c[a0.x + %d]' % (self.ch(self.temps), self.anysrc(), r.randint(0, 64)))
            else:
                self.instr()
        for o in outputs:
            src = self.anysrc()
            if o == 'oDepth':
                L.append('mov oDepth, %s' % self.anysrc(1))
            elif o.endswith('.x'):
                L.append('mov %s, %s' % (o, self.anysrc(1)))
            else:
                L.append('mov%s %s, %s' % ('_sat' if r.random() < 0.2 else '', o, src))
        return '\n'.join(L) + '\n'

    def anysrc(self, n=4):
        reg = self.ch(self.temps + self.temps + self.inputs + self.consts)
        if '.' in reg:
            base, sw = reg.split('.')
            return self.srcmod(base) + ('.' + sw[0] if n == 1 else '.' + sw)
        return self.srcmod(reg) + self.swz(n)

    def body(self, n, rel=None):
        for _ in range(n):
            if rel and self.r.random() < 0.4:
                self.lines.append('add %s, %s, c[aL + %d]' % (self.ch(self.temps), self.anysrc(), self.r.randint(0, 40)))
            else:
                self.instr()

    def instr(self):
        r = self.r
        L = self.lines
        d = self.ch(self.temps)
        sat = '_sat' if r.random() < 0.15 else ''
        pp = '_pp' if self.ps and r.random() < 0.1 else ''
        k = r.randint(0, 27)
        if k <= 6:
            op = self.ch(['add', 'sub', 'mul', 'min', 'max', 'slt', 'sge'])
            L.append('%s%s%s %s%s, %s, %s' % (op, sat, pp, d, self.mask(), self.anysrc(), self.anysrc()))
        elif k <= 8:
            op = self.ch(['mad', 'lrp', 'cmp'])
            L.append('%s%s %s%s, %s, %s, %s' % (op, sat, d, self.mask(), self.anysrc(), self.anysrc(), self.anysrc()))
        elif k <= 10:
            op = self.ch(['dp3', 'dp4'])
            L.append('%s%s %s%s, %s, %s' % (op, sat, d, self.mask(), self.anysrc(), self.anysrc()))
        elif k <= 13:
            op = self.ch(['rcp', 'rsq', 'exp', 'log'])
            L.append('%s%s %s%s, %s' % (op, sat, d, self.mask(), self.anysrc(1)))
        elif k == 14:
            L.append('pow%s %s%s, %s, %s' % (sat, d, self.mask(), self.anysrc(1), self.anysrc(1)))
        elif k == 15:
            op = self.ch(['frc', 'abs', 'mov', 'sgn'])
            if op == 'sgn' and not self.ps:
                L.append('sgn %s%s, %s, %s, %s' % (d, self.mask(), self.anysrc(), self.ch(self.temps), self.ch(self.temps)))
            elif op == 'sgn':
                L.append('frc %s%s, %s' % (d, self.mask(), self.anysrc()))
            else:
                L.append('%s%s %s%s, %s' % (op, sat, d, self.mask(), self.anysrc()))
        elif k == 16:
            L.append('nrm %s.xyz, %s' % (d, self.anysrc()))
        elif k == 17:
            L.append('sincos %s.%s, %s' % (d, self.ch(['x', 'y', 'xy']), self.anysrc(1)))
        elif k == 18:
            L.append('crs %s.%s, %s, %s' % (d, self.ch(['xyz', 'x', 'yz']), self.ch(self.temps), self.ch(self.temps)))
        elif k == 19:
            L.append('lit %s, %s' % (d, self.anysrc()))
        elif k == 20:
            L.append('dst %s, %s, %s' % (d, self.anysrc(), self.anysrc()))
        elif k == 21:
            op = self.ch(['m4x4', 'm3x3', 'm4x3', 'm3x4', 'm3x2'])
            mask = {'m4x4': '', 'm3x3': '.xyz', 'm4x3': '.xyz', 'm3x4': '', 'm3x2': '.xy'}[op]
            L.append('%s %s%s, %s, c%d' % (op, d, mask, self.ch(self.temps), r.randint(0, 200 if self.ps else 240)))
        elif k == 22 and self.ps:
            L.append('dp2add%s %s%s, %s, %s, %s' % (sat, d, self.mask(), self.anysrc(), self.anysrc(), self.anysrc(1)))
        elif k == 23 and self.ps:
            L.append('%s %s%s, %s' % (self.ch(['dsx', 'dsy']), d, self.mask(), self.anysrc()))
        elif k >= 24 and self.samplers:
            s, t = self.ch(self.samplers)
            coord = self.ch(self.temps + [i for i in self.inputs if '.' not in i])
            if not self.ps:
                L.append('texldl %s, %s, %s' % (d, coord, s))
                return
            op = self.ch(['texld', 'texld', 'texldp', 'texldb', 'texldl', 'texldd'])
            if op == 'texldd':
                L.append('texldd %s, %s, %s, %s, %s' % (d, coord, s, self.ch(self.temps), self.ch(self.temps)))
            else:
                L.append('%s %s, %s, %s' % (op, d, coord, s))
        elif k == 25 and self.ps and r.random() < 0.3:
            L.append('texkill %s' % self.ch(self.temps))
        else:
            L.append('mad %s, %s, %s, %s' % (d, self.anysrc(), self.anysrc(), self.anysrc()))


def generate(outdir, count, seed):
    os.makedirs(outdir, exist_ok=True)
    rnd = random.Random(seed)
    for i in range(count):
        text = Gen(rnd).gen()
        name = 'gen%05d' % i
        with open(os.path.join(outdir, name + '.asm'), 'w') as f:
            f.write(text)
        with open(os.path.join(outdir, name + '.bin'), 'wb') as f:
            f.write(assemble(text))


def main(argv):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if len(argv) >= 3 and argv[0] == 'asm':
        open(argv[2], 'wb').write(assemble(open(argv[1]).read()))
        return 0
    if argv and argv[0] == 'check':
        return 0 if check(root) else 1
    if len(argv) >= 3 and argv[0] == 'gen':
        generate(argv[1], int(argv[2]), int(argv[3]) if len(argv) > 3 else 1)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
