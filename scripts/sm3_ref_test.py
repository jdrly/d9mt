#!/usr/bin/env python3
"""GPU reference test for the SM3 converter fixes of GitHub issue #8: crs and
setp with every write mask, and packed / duplicate I/O declarations (VS
inputs, VS outputs, PS inputs). Every case runs through the whole driver
under Wine (d3d9.dll front end, winemetal.so AIR compiler, Metal) and its
render-target words are compared with an independent scalar model of the
D3D9 semantics computed here.

  ENGINE=<engine> [FRAMEWORKS=<dir>] [WORK=<dir>] \\
    scripts/sm3_ref_test.py <d3d9.dll> [--strict] [--only <prefix>] [--corpus <dir>...]

--strict      d3d9.floatEmulation = Strict (legacy 0 * x = 0 multiplies)
              instead of the default (Enabled: IEEE multiplies)
--corpus dir  also run the crs differential over a generated corpus
              (scripts/sm3_corpus.py gen): every shader with a crs runs as
              is and with each crs replaced by mul/mul/add (no crs left);
              both must give the same words
--list        print the case names and exit
--x86         the driver is the 32-bit build (i686 harness)
--skip file   leave out the cases whose names the file lists (one per line)

Comparison: exact bits, except that +0 and -0 are equal, any NaN equals
any NaN, and cases built from non-dyadic inputs allow 2 ulps (their
products round, and the compiler may fuse a multiply into the subtraction).

Semantics modelled (D3D9 docs, wined3d, DXVK's dxso):
- crs: dst.x = s0.y*s1.z - s0.z*s1.y, dst.y = s0.z*s1.x - s0.x*s1.z,
  dst.z = s0.x*s1.y - s0.y*s1.x, after the source swizzles and modifiers;
  only the components in the write mask change. .w is not a crs result:
  a mask with .w is invalid D3D9 (the runtime rejects it) and d9mt leaves
  .w unwritten.
- setp: p0 components in the write mask get the comparison, others keep
  their value.
- Predication: component c is written if p0[swizzle[c]] (negated for !p0).
- I/O packing: a dcl mask selects components of the semantic at the same
  positions (dcl_texcoord1 v0.zw: v0.z = texcoord1.z, v0.w = texcoord1.w);
  a register component no dcl covers takes the nearest declared semantic
  below it (else above); a semantic declared again maps to the same data.
  Vertex elements missing components read (0, 0, 0, 1) as in D3D9.
"""
import hashlib
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm3_corpus  # noqa: E402
from sm3_wine import WineRunner  # noqa: E402

# --- float32 model -------------------------------------------------------------


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def from_bits(b):
    return struct.unpack('<f', struct.pack('<I', b))[0]


def mul(a, b, legacy):
    if legacy:
        # ArithmeticPass::emitMulLegacy: (b == 0 ? 0 : a) * (a == 0 ? 0 : b)
        a, b = (0.0 if b == 0 else a), (0.0 if a == 0 else b)
    return f32(a * b)


def sub(a, b):
    return f32(a - b)


def crs(a, b, legacy):
    return [sub(mul(a[1], b[2], legacy), mul(a[2], b[1], legacy)),
            sub(mul(a[2], b[0], legacy), mul(a[0], b[2], legacy)),
            sub(mul(a[0], b[1], legacy), mul(a[1], b[0], legacy))]


def same(want, got_bits, ulps):
    if want is None:
        return True
    g = from_bits(got_bits)
    w = f32(want)
    if w != w:
        return g != g
    if g != g:
        return False
    if w == 0 and g == 0:
        return True
    if bits(w) == got_bits:
        return True
    if ulps and (w < 0) == (g < 0):
        return abs(bits(w) - got_bits) <= ulps
    return False


COMP = 'xyzw'


def mask_set(m):
    return [COMP.index(c) for c in m]


def swz(v, s):
    s = (s + s[-1] * 4)[:4]
    return [v[COMP.index(c)] for c in s]


def apply_mod(v, mod):
    if mod == 'neg':
        return [-x for x in v]
    if mod == 'abs':
        return [abs(x) for x in v]
    if mod == 'negabs':
        return [-abs(x) for x in v]
    return list(v)


def src_text(reg, mod, sw):
    t = reg + ('_abs' if mod in ('abs', 'negabs') else '') + ('.' + sw if sw != 'xyzw' else '')
    return ('-' if mod in ('neg', 'negabs') else '') + t


def fhex(x):
    return '0x%08x' % bits(x)


def def_line(reg, v):
    return 'def %s, %s' % (reg, ', '.join(fhex(f32(x)) for x in v))


# --- cases ---------------------------------------------------------------------

POS = [-0.5, 0.5, 0.5, 1.0]  # NDC of pixel (1,1) of the 4x4 targets
D_POS, D_NORMAL, D_TEX, D_COLOR = 0, 3, 5, 10
T_F1, T_F2, T_F3, T_F4 = 0, 1, 2, 3


class Case:
    def __init__(self, name, vs, ps, elems, vertex, expect, ulps=0):
        self.name, self.vs, self.ps = name, vs, ps
        self.elems = elems      # [(usage, index, type, offset)]
        self.vertex = vertex    # floats (or ('bits', n))
        self.expect = expect    # 17 entries: oC0..oC3 (1,1) words + oC0 (3,3) red, None = any
        self.ulps = ulps


def expect_rt(*rts, corner=None):
    e = []
    for i in range(4):
        e += list(rts[i]) if i < len(rts) and rts[i] is not None else [None] * 4
    return e + [corner]


VS_PASS2 = '''vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_texcoord1 v2
dcl_position o0
dcl_texcoord0 o1
dcl_texcoord1 o2
mov o0, v0
mov o1, v1
mov o2, v2
'''
PS_PASS1 = 'ps_3_0\ndcl_texcoord0 v0\nmov oC0, v0\n'
ELEMS3 = [(D_POS, 0, T_F4, 0), (D_TEX, 0, T_F4, 16), (D_TEX, 1, T_F4, 32)]

VALUES = {
    # dyadic: every product and difference exact in f32 (and in f16 for _pp)
    'i': ([3.0, -2.0, 0.5, 7.0], [-1.5, 4.0, 2.0, -3.0], 0),
    'z': ([0.0, -0.0, 5.0, 1.0], [2.0, 0.0, -4.0, -0.0], 0),
    # rounding products: 2 ulps
    'f': ([1.1, 2345.6789, -0.0777, 1.0], [-3.3, 0.441, 987.65, 1.0], 2),
}
INIT = [101.0, 102.0, 103.0, 104.0]
PRED = [True, False, True, False]   # setp_gt p0, c1, c2 below
MASKS = ['x', 'y', 'z', 'w', 'xy', 'xz', 'xw', 'yz', 'yw', 'zw', 'xyz', 'xyw', 'xzw', 'yzw', 'xyzw']
PREDS = ['', '(p0) ', '(!p0) ', '(p0.y) ']


def pred_mask(pred, comps):
    if not pred:
        return comps
    if pred == '(p0.y) ':
        return comps if PRED[1] else []
    want = pred == '(p0) '
    return [c for c in comps if PRED[c] == want]


def crs_case(stage, mask, pred='', sat=False, pp=False, mods=('', ''), sws=('xyzw', 'xyzw'),
             values='i', source='input', legacy=False):
    a, b, ulps = VALUES[values]
    code = [def_line('c0', INIT), def_line('c1', [1, -1, 1, -1]), def_line('c2', [0, 0, 0, 0])]
    if source == 'def':
        code += [def_line('c3', a), def_line('c4', b), 'mov r0, c3', 'mov r1, c4']
    else:
        code += ['mov r0, %s' % ('v0' if stage == 'ps' else 'v1'),
                 'mov r1, %s' % ('v1' if stage == 'ps' else 'v2')]
    code += ['mov r2, c0']
    if pred:
        code += ['setp_gt p0, c1, c2']
    op = 'crs' + ('_sat' if sat else '') + ('_pp' if pp else '')
    code += ['%s%s r2.%s, %s, %s' % (pred, op, mask, src_text('r0', mods[0], sws[0]),
                                       src_text('r1', mods[1], sws[1]))]
    if stage == 'ps':
        vs = VS_PASS2
        ps = 'ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\n' + '\n'.join(code) + '\nmov oC0, r2\n'
    else:
        vs = ('vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_position o0\n'
              'dcl_texcoord0 o1\n' + '\n'.join(code) + '\nmov o0, v0\nmov o1, r2\n')
        ps = PS_PASS1
    sa = apply_mod(swz(a, sws[0]), mods[0])
    sb = apply_mod(swz(b, sws[1]), mods[1])
    res = crs(sa, sb, legacy)
    if sat:
        res = [min(max(x, 0.0), 1.0) for x in res]
    out = list(INIT)
    for c in pred_mask(pred, [c for c in mask_set(mask) if c < 3]):
        out[c] = res[c]
    name = 'crs_%s_%s_%s%s%s%s_%s%s_%s_%s_%s' % (
        stage, mask, {'': 'np', '(p0) ': 'p', '(!p0) ': 'np0', '(p0.y) ': 'py'}[pred],
        '_sat' if sat else '', '_pp' if pp else '', '', mods[0] or 'id', mods[1] or 'id',
        sws[0] + sws[1] if sws != ('xyzw', 'xyzw') else 'swid', values, source)
    return Case(name, vs, ps, ELEMS3, POS + a + b, expect_rt(out), ulps)


SETP_A = [1.0, 2.0, 3.0, -1.0]
SETP_B = [1.0, 1.0, 4.0, -2.0]
CMP = {'gt': lambda x, y: x > y, 'lt': lambda x, y: x < y, 'ge': lambda x, y: x >= y,
       'le': lambda x, y: x <= y, 'eq': lambda x, y: x == y, 'ne': lambda x, y: x != y}


def setp_case(stage, mask, cmp, neg=False, sw='xyzw'):
    code = [def_line('c0', [0, 0, 0, 0]), def_line('c5', [1, 1, 1, 1]),
            'mov r0, %s' % ('v0' if stage == 'ps' else 'v1'),
            'mov r1, %s' % ('v1' if stage == 'ps' else 'v2'),
            'setp_ge p0, c0, c0',
            'setp_%s p0.%s, %s, r1' % (cmp, mask, src_text('r0', '', sw)),
            'mov r2, c0',
            '%s mov r2, c5' % ('(!p0)' if neg else '(p0)')]
    if stage == 'ps':
        vs = VS_PASS2
        ps = 'ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\n' + '\n'.join(code) + '\nmov oC0, r2\n'
    else:
        vs = ('vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_texcoord1 v2\ndcl_position o0\n'
              'dcl_texcoord0 o1\n' + '\n'.join(code) + '\nmov o0, v0\nmov o1, r2\n')
        ps = PS_PASS1
    a = swz(SETP_A, sw)
    p = [CMP[cmp](a[c], SETP_B[c]) if c in mask_set(mask) else True for c in range(4)]
    out = [1.0 if (x != neg) else 0.0 for x in p]
    name = 'setp_%s_%s_%s%s%s' % (stage, mask, cmp, '_not' if neg else '', '' if sw == 'xyzw' else '_' + sw)
    return Case(name, vs, ps, ELEMS3, POS + SETP_A + SETP_B, expect_rt(out))


# I/O: feeder VS with every PS-visible semantic, distinct exact values
SEMS = ['texcoord%d' % i for i in range(8)] + ['color0', 'color1']
SEMVAL = {s: [10.0 * (i + 1) + k + 1 for k in range(4)] for i, s in enumerate(SEMS)}
VS_FEED = ('vs_3_0\ndcl_position v0\ndcl_position o0\n'
           + ''.join('dcl_%s o%d\n' % (s, i + 1) for i, s in enumerate(SEMS))
           + ''.join(def_line('c%d' % i, SEMVAL[s]) + '\n' for i, s in enumerate(SEMS))
           + 'mov o0, v0\n'
           + ''.join('mov o%d, c%d\n' % (i + 1, i) for i, s in enumerate(SEMS)))
ELEMS1 = [(D_POS, 0, T_F4, 0)]


def ps_io_case(name, dcls, body, outs):
    """PS reading the feeder's semantics through dcls [(sem, reg, mask)]."""
    ps = 'ps_3_0\n' + ''.join('dcl_%s %s%s\n' % (s, r, '.' + m if m else '') for s, r, m in dcls) + body
    return Case('io_ps_' + name, VS_FEED, ps, ELEMS1, POS, expect_rt(*outs))


def packed_reg(dcls, reg):
    """Model: value of every component of a PS input register."""
    entries = [(s, mask_set(m or 'xyzw')) for s, r, m in dcls if r == reg]
    out = []
    for c in range(4):
        best, dist = None, 99
        for s, comps in entries:
            if c in comps:
                d = 0
            elif [x for x in comps if x < c]:
                d = c - max(x for x in comps if x < c)
            else:
                d = 4 + min(x for x in comps if x > c) - c
            if d < dist:
                best, dist = s, d
        out.append(SEMVAL[best][c])
    return out


def ps_io_cases():
    cases = []

    def packed(name, dcls, regs, swizzles=None):
        swizzles = swizzles or ['xyzw'] * len(regs)
        body = ''.join('mov oC%d, %s%s\n' % (i, r, '' if sw == 'xyzw' else '.' + sw)
                       for i, (r, sw) in enumerate(zip(regs, swizzles)))
        outs = [swz(packed_reg(dcls, r), sw) for r, sw in zip(regs, swizzles)]
        cases.append(ps_io_case(name, dcls, body, outs))

    packed('plain', [('texcoord0', 'v0', ''), ('texcoord1', 'v1', 'xy')], ['v0', 'v1'])
    packed('xy_zw', [('texcoord0', 'v0', 'xy'), ('texcoord1', 'v0', 'zw')], ['v0'])
    packed('xy_zw_swz', [('texcoord0', 'v0', 'xy'), ('texcoord1', 'v0', 'zw')], ['v0'], ['wzyx'])
    packed('x_yzw', [('texcoord2', 'v3', 'x'), ('texcoord5', 'v3', 'yzw')], ['v3'])
    packed('xyz_w', [('color1', 'v2', 'xyz'), ('texcoord7', 'v2', 'w')], ['v2'])
    packed('x_y_z_w', [('texcoord0', 'v0', 'x'), ('texcoord1', 'v0', 'y'), ('texcoord2', 'v0', 'z'),
                       ('texcoord3', 'v0', 'w')], ['v0'])
    packed('xz_yw', [('texcoord4', 'v1', 'xz'), ('texcoord6', 'v1', 'yw')], ['v1'])
    packed('docs_y_zw', [('texcoord1', 'v1', 'y'), ('texcoord0', 'v1', 'zw')], ['v1'])
    packed('gap_x_w', [('texcoord1', 'v0', 'x'), ('texcoord2', 'v0', 'w')], ['v0'])
    packed('two_packed', [('texcoord0', 'v0', 'xy'), ('texcoord1', 'v0', 'zw'),
                          ('color0', 'v1', 'xyz'), ('texcoord5', 'v1', 'w'), ('texcoord6', 'v2', '')],
           ['v0', 'v1', 'v2'])
    packed('dupsem_split', [('texcoord3', 'v0', 'xy'), ('texcoord3', 'v0', 'zw')], ['v0'])
    packed('dupsem_tworeg', [('texcoord3', 'v0', 'xy'), ('texcoord3', 'v1', 'zw')], ['v0', 'v1'])
    # an undeclared v1 reads as color1, which v0 declares: same data
    cases.append(ps_io_case('implicit_alias', [('color1', 'v0', '')], 'add oC0, v0, v1\n',
                            [[2 * x for x in SEMVAL['color1']]]))
    # relative addressing (the dynamic load function): r0 = v0 + v1
    dcls = [('texcoord0', 'v0', 'xy'), ('texcoord1', 'v0', 'zw'), ('texcoord2', 'v1', '')]
    want = [x + y for x, y in zip(packed_reg(dcls, 'v0'), packed_reg(dcls, 'v1'))]
    cases.append(ps_io_case('rel_aL', dcls, 'defi i0, 2, 0, 1, 0\ndef c0, 0, 0, 0, 0\nmov r0, c0\n'
                            'loop aL, i0\nadd r0, r0, v0[aL]\nendloop\nmov oC0, r0\n', [want]))
    return cases


def vs_out_cases():
    """Packed VS outputs, read back unpacked by the PS."""
    cases = []
    val = [1.0, 2.0, 3.0, 6.0]

    def case(name, dcls, body, sems, outs, corner=None, extra_defs=''):
        vs = ('vs_3_0\ndcl_position v0\ndcl_position o0\n'
              + ''.join('dcl_%s %s%s\n' % (s, r, '.' + m if m else '') for s, r, m in dcls)
              + def_line('c0', val) + '\n' + def_line('c1', [7, 8, 9, 10]) + '\n' + extra_defs
              + 'mov o0, v0\n' + body)
        ps = ('ps_3_0\n' + ''.join('dcl_%s v%d\n' % (s, i) for i, s in enumerate(sems))
              + ''.join('mov oC%d, v%d\n' % (i, i) for i in range(len(sems))))
        cases.append(Case('io_vsout_' + name, vs, ps, ELEMS1, POS, expect_rt(*outs, corner=corner)))

    z = 0.0
    one = 1.0  # unwritten components of color0 default to 1.0, other outputs to 0
    case('xy_zw', [('texcoord0', 'o1', 'xy'), ('texcoord1', 'o1', 'zw')], 'mov o1, c0\n',
         ['texcoord0', 'texcoord1'], [[1, 2, z, z], [z, z, 3, 6]])
    case('x_y_z_w', [('texcoord0', 'o2', 'x'), ('texcoord1', 'o2', 'y'), ('texcoord2', 'o2', 'z'),
                     ('color0', 'o2', 'w')], 'mov o2, c0\n',
         ['texcoord0', 'texcoord1', 'texcoord2', 'color0'],
         [[1, z, z, z], [z, 2, z, z], [z, z, 3, z], [one, one, one, 6]])
    case('partial', [('texcoord0', 'o1', 'xy'), ('texcoord1', 'o1', 'zw')],
         'mov o1, c1\nmov o1.y, c0.x\nmov o1.z, c0.y\n',
         ['texcoord0', 'texcoord1'], [[7, 1, z, z], [z, z, 2, 10]])
    case('dupsem_split', [('texcoord4', 'o3', 'xy'), ('texcoord4', 'o3', 'zw')], 'mov o3, c0\n',
         ['texcoord4'], [val])
    # one dcl per register: each register writes all of texcoord4, the last write wins
    case('dupsem_tworeg', [('texcoord4', 'o3', 'xy'), ('texcoord4', 'o4', 'zw')],
         'mov o3, c0\nmov o4, c1\n', ['texcoord4'], [[7, 8, 9, 10]])
    case('dupsem_tworeg_masked', [('texcoord4', 'o3', 'xy'), ('texcoord4', 'o4', 'zw')],
         'mov o3.xy, c0\nmov o4.zw, c1\n', ['texcoord4'], [[1, 2, 9, 10]])
    # an undeclared o3 is texcoord3, which o1 declares: o3 writes texcoord3
    case('implicit_alias', [('texcoord3', 'o1', '')], 'mov o1, c0\nmov o3.zw, c1\n',
         ['texcoord3'], [[1, 2, 9, 10]])
    # point size packed into .w: size 6 covers pixel (3,3) (corner = texcoord0.x)
    case('psize_w', [('texcoord0', 'o1', 'xyz'), ('psize', 'o1', 'w')], 'mov o1, c0\n',
         ['texcoord0'], [[1, 2, 3, z]], corner=1.0)
    # relative output write (the dynamic store function): o[aL + 1] = c0, aL = 0
    case('rel_aL', [('texcoord0', 'o1', 'xy'), ('texcoord1', 'o1', 'zw')],
         'defi i0, 1, 0, 1, 0\nloop aL, i0\nREL_STORE\nendloop\n',
         ['texcoord0', 'texcoord1'], [[1, 2, z, z], [z, z, 3, 6]])
    return cases


def vs_in_cases():
    """Packed VS inputs fed by vertex elements, passed to the PS unpacked."""
    cases = []
    T = [1.0, 2.0, 3.0, 4.0]
    N = [5.0, 6.0, 7.0, 8.0]
    C = [9.0, 10.0, 11.0, 12.0]

    def case(name, dcls, elems, data, body, outs):
        vs = ('vs_3_0\ndcl_position v0\n'
              + ''.join('dcl_%s %s%s\n' % (s, r, '.' + m if m else '') for s, r, m in dcls)
              + 'dcl_position o0\ndcl_texcoord0 o1\ndcl_texcoord1 o2\nmov o0, v0\n' + body)
        ps = 'ps_3_0\ndcl_texcoord0 v0\ndcl_texcoord1 v1\nmov oC0, v0\nmov oC1, v1\n'
        cases.append(Case('io_vsin_' + name, vs, ps, [(D_POS, 0, T_F4, 0)] + elems, POS + data,
                          expect_rt(*outs)))

    f4 = lambda u, i, off: (u, i, T_F4, off)
    case('xy_zw', [('texcoord0', 'v1', 'xy'), ('normal', 'v1', 'zw')],
         [f4(D_TEX, 0, 16), f4(D_NORMAL, 0, 32)], T + N, 'mov o1, v1\n', [[1, 2, 7, 8]])
    # FLOAT2 normal reads (5, 6, 0, 1): .zw are the defaults, not the data
    case('xy_zw_float2', [('texcoord0', 'v1', 'xy'), ('normal', 'v1', 'zw')],
         [f4(D_TEX, 0, 16), (D_NORMAL, 0, T_F2, 32)], T + N[:2], 'mov o1, v1\n', [[1, 2, 0, 1]])
    case('x_y_z_w', [('texcoord0', 'v1', 'x'), ('normal', 'v1', 'y'), ('color0', 'v1', 'z'),
                     ('texcoord1', 'v1', 'w')],
         [f4(D_TEX, 0, 16), f4(D_NORMAL, 0, 32), f4(D_COLOR, 0, 48), f4(D_TEX, 1, 64)],
         T + N + C + [13.0, 14.0, 15.0, 16.0], 'mov o1, v1\n', [[1, 6, 11, 16]])
    case('xyz_w', [('normal', 'v2', 'xyz'), ('texcoord0', 'v2', 'w')],
         [f4(D_NORMAL, 0, 16), f4(D_TEX, 0, 32)], N + T, 'mov o1, v2\n', [[5, 6, 7, 4]])
    case('dupsem_split', [('texcoord0', 'v1', 'xy'), ('texcoord0', 'v1', 'zw')],
         [f4(D_TEX, 0, 16)], T, 'mov o1, v1\n', [T])
    case('dupsem_tworeg', [('texcoord0', 'v1', ''), ('normal', 'v2', ''), ('texcoord0', 'v3', '')],
         [f4(D_TEX, 0, 16), f4(D_NORMAL, 0, 32)], T + N, 'mov o1, v3\nmov o2, v2\n', [T, N])
    # an undeclared v2 is color2, which v1 declares: same data
    case('implicit_alias', [('color2', 'v1', '')], [(D_COLOR, 2, T_F4, 16)], C,
         'add o1, v1, v2\n', [[2 * x for x in C]])
    # relative addressing (the dynamic load function): r0 = v1 + v2
    case('rel_aL', [('texcoord0', 'v1', 'xy'), ('normal', 'v1', 'zw'), ('color0', 'v2', '')],
         [f4(D_TEX, 0, 16), f4(D_NORMAL, 0, 32), f4(D_COLOR, 0, 48)], T + N + C,
         'defi i0, 2, 0, 1, 0\ndef c0, 0, 0, 0, 0\nmov r0, c0\nloop aL, i0\nadd r0, r0, v1[aL]\n'
         'endloop\nmov o1, r0\n', [[1 + 9, 2 + 10, 7 + 11, 8 + 12]])
    return cases


def all_cases(legacy):
    cases = []
    for st in ('ps', 'vs'):
        for m in MASKS:
            for pred in PREDS:
                for sat in (False, True):
                    for v in ('i', 'z', 'f'):
                        cases.append(crs_case(st, m, pred, sat, values=v, legacy=legacy))
            for m0 in ('', 'neg', 'abs', 'negabs'):
                for m1 in ('', 'neg', 'abs', 'negabs'):
                    for sws in (('xyzw', 'xyzw'), ('yzxw', 'wzyx')):
                        if (m0, m1, sws) == ('', '', ('xyzw', 'xyzw')):
                            continue
                        cases.append(crs_case(st, m, mods=(m0, m1), sws=sws, legacy=legacy))
            for v in ('i', 'z') if st == 'ps' else ():  # _pp is pixel-shader only
                cases.append(crs_case(st, m, pp=True, values=v, legacy=legacy))
                cases.append(crs_case(st, m, pred='(p0) ', pp=True, sat=True, values=v, legacy=legacy))
            cases.append(crs_case(st, m, source='def', legacy=legacy))
            cases.append(crs_case(st, m, source='def', values='z', legacy=legacy))
            for cmp in CMP:
                cases.append(setp_case(st, m, cmp))
            cases.append(setp_case(st, m, 'gt', neg=True))
            cases.append(setp_case(st, m, 'lt', sw='wzyx'))
    cases += ps_io_cases() + vs_out_cases() + vs_in_cases()
    return cases


# --- corpus crs differential ---------------------------------------------------

CRS_RE = re.compile(r'^crs\s+(r\d+)(\.[xyzw]+)?,\s*(r\d+),\s*(r\d+)\s*$')


def crs_oracle(text):
    """The shader with every crs replaced by mul/mul/add (r30/r31 as scratch)."""
    out = []
    for line in text.splitlines():
        m = CRS_RE.match(line.strip())
        if m:
            dst, mask, a, b = m.group(1), m.group(2) or '', m.group(3), m.group(4)
            out += ['mul r30.xyz, %s.yzxw, %s.zxyw' % (a, b),
                    'mul r31.xyz, %s.zxyw, %s.yzxw' % (a, b),
                    'add %s%s, r30, -r31' % (dst, mask)]
        else:
            out.append(line)
    return '\n'.join(out) + '\n'


def corpus_cases(d, tag='c0'):
    cases = []
    feed_elems = [(D_POS, 0, T_F4, 0), (D_TEX, 0, T_F4, 16)]
    feed_data = POS + [0.25, -1.5, 2.0, 3.0]
    # feeder for corpus pixel shaders: every semantic, computed per vertex
    feed = ('vs_3_0\ndcl_position v0\ndcl_texcoord0 v1\ndcl_position o0\n'
            + ''.join('dcl_%s o%d\n' % (s, i + 1) for i, s in enumerate(SEMS))
            + ''.join(def_line('c%d' % i, [0.5 * (i + 1), -0.25 * i, 1.5, 0.125 * i]) + '\n'
                      for i in range(len(SEMS)))
            + 'mov o0, v0\n'
            + ''.join('mad o%d, v1, c%d, c%d.wzyx\n' % (i + 1, i, i) for i in range(len(SEMS))))
    for f in sorted(os.listdir(d)):
        if not f.endswith('.asm'):
            continue
        text = open(os.path.join(d, f)).read()
        if not any(CRS_RE.match(l.strip()) for l in text.splitlines()):
            continue
        base = f[:-4]
        if text.startswith('ps_3_0'):
            for variant, t in (('orig', text), ('oracle', crs_oracle(text))):
                cases.append(Case('corpus_%s_%s_%s' % (tag, base, variant), feed, t, feed_elems, feed_data, None))
            continue
        # vertex shader: position from v0, no point size (both would only
        # move or hide the point), every input semantic fed, outputs read back
        lines = text.splitlines()
        psize = None
        decl, outs = [], []
        for l in lines:
            m = re.match(r'^dcl_psize (o\d+)', l)
            if m:
                psize = m.group(1)
            m = re.match(r'^dcl_([a-z]+)(\d*) v(\d+)$', l)
            if m:
                decl.append((m.group(1), int(m.group(2) or 0), int(m.group(3))))
            m = re.match(r'^dcl_(texcoord\d|color\d) o(\d+)$', l)
            if m:
                outs.append(m.group(1))
        keep = []
        for l in lines:
            if psize and (l.startswith('dcl_psize') or re.match(r'^mov(_sat)? %s\.x,' % psize, l)):
                continue
            if re.match(r'^mov(_sat)? o0,', l):
                l = 'mov o0, v0'
            keep.append(l)
        text = '\n'.join(keep) + '\n'
        usage = {'position': 0, 'blendweight': 1, 'normal': 3, 'texcoord': 5, 'tangent': 6,
                 'binormal': 7, 'color': 10}
        elems, data = [], []
        for i, (u, idx, reg) in enumerate(sorted(decl, key=lambda x: x[2])):
            elems.append((usage[u], idx, T_F4, 16 * i))
            data += POS if u == 'position' else [0.5 + reg, -1.25 * reg, 2.0 - reg, 0.75 * reg]
        for k in range(0, len(outs), 4):
            sems = outs[k:k + 4]
            ps = ('ps_3_0\n' + ''.join('dcl_%s v%d\n' % (s, i) for i, s in enumerate(sems))
                  + ''.join('mov oC%d, v%d\n' % (i, i) for i in range(len(sems))))
            for variant, t in (('orig', text), ('oracle', crs_oracle(text))):
                cases.append(Case('corpus_%s_%s_%d_%s' % (tag, base, k, variant), t, ps, elems, data, None))
    return cases


# --- runner ----------------------------------------------------------------------

REL_STORE_TOKENS = None


def assemble(text):
    """sm3_corpus.assemble, plus "REL_STORE" = mov o[aL + 1], c0 (the corpus
    assembler has no relative destination)."""
    if 'REL_STORE' not in text:
        return sm3_corpus.assemble(text)
    head, tail = text.split('REL_STORE\n')
    a = sm3_corpus.assemble(head)[:-4]   # drop the end token
    b = sm3_corpus.assemble('vs_3_0\n' + tail)[4:]
    dst = 0x80000000 | sm3_corpus.type_bits(6) | 1 | 0x2000 | (0xF << 16)
    rel = 0x80000000 | sm3_corpus.type_bits(15) | (0x00 << 16)
    src = sm3_corpus.enc_src('c0')
    ins = [0x01 | ((2 + len(src)) << 24), dst, rel] + src
    return a + struct.pack('<%dI' % len(ins), *ins) + b


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    dll = argv[0]
    legacy = '--strict' in argv
    only = argv[argv.index('--only') + 1] if '--only' in argv else ''
    corpora = [argv[i + 1] for i, a in enumerate(argv) if a == '--corpus']
    cases = [c for c in all_cases(legacy) if c.name.startswith(only)]
    for i, d in enumerate(corpora):
        cases += [c for c in corpus_cases(d, 'c%d' % i) if c.name.startswith(only)]
    if '--skip' in argv:
        skip = set(open(argv[argv.index('--skip') + 1]).read().split())
        cases = [c for c in cases if c.name not in skip]
    names = set()
    for c in cases:
        assert c.name not in names, c.name
        names.add(c.name)
    if '--list' in argv:
        for c in cases:
            print(c.name)
        return 0

    runner = WineRunner(dll, 'x86' if '--x86' in argv else 'x64')
    sdir = runner.reset_dir('s')
    files = {}

    def put(text):
        code = assemble(text)
        h = hashlib.sha1(code).hexdigest()[:16] + '.bin'
        if h not in files:
            open(os.path.join(sdir, h), 'wb').write(code)
            files[h] = text
        return 's/' + h

    lines = []
    for c in cases:
        vs, ps = put(c.vs), put(c.ps)
        stride = max([e[3] + 4 * (e[2] + 1) for e in c.elems])
        verts = ' '.join(fhex(x) for x in c.vertex)
        lines.append('case %s %s %s %d %d %s %d %s' % (
            c.name, vs, ps, stride, len(c.elems), ' '.join('%d %d %d %d' % e for e in c.elems),
            len(c.vertex), verts))
    conf = 'd3d9.floatEmulation = Strict\n' if legacy else None
    results, crashes, log = runner.run(lines, conf=conf)

    got = {}
    for idx, line in results.items():
        parts = line.split()
        got[parts[2]] = parts[3:]
    fails, crashed, passed, other = [], [], 0, []
    corpus = {}
    for i, c in enumerate(cases):
        if i in crashes:
            crashed.append(c.name)
            continue
        words = got.get(c.name)
        if not words or len(words) < 17 or not re.match(r'^[0-9a-f]{8}$', words[0]):
            other.append('%s %s' % (c.name, ' '.join(words or ['missing'])))
            continue
        w = [int(x, 16) for x in words[:17]]
        if c.expect is None:
            corpus[c.name] = w
            continue
        bad = [k for k in range(17) if not same(c.expect[k], w[k], c.ulps)]
        if bad:
            fails.append((c, w, bad))
        else:
            passed += 1

    cdiff, cmatch, cexact = [], 0, 0
    for name, w in sorted(corpus.items()):
        if not name.endswith('_orig'):
            continue
        o = corpus.get(name[:-5] + '_oracle')
        if o is None:
            continue
        if all(same(from_bits(b), a, 0) for a, b in zip(w, o)):
            cexact += 1
            cmatch += 1
        elif all(same(from_bits(b), a, 4) for a, b in zip(w, o)):
            cmatch += 1
        else:
            cdiff.append(name[:-5])
    corpus_crash = sorted(set(n.rsplit('_', 1)[0] for n in crashed if n.startswith('corpus_')))
    ref_cases = [c for c in cases if c.expect is not None]
    print('[ref] %s, float emulation %s: %d cases, %d shaders' % (
        os.path.basename(dll), 'Strict' if legacy else 'Enabled', len(ref_cases), len(files)))
    print('[ref] reference: %d pass, %d mismatch, %d crashed (%d restarts), %d no result' % (
        passed, len(fails), len([n for n in crashed if not n.startswith('corpus_')]),
        len(crashes), len([o for o in other if not o.startswith('corpus_')])))
    for c, w, bad in fails[:40]:
        print('  MISMATCH %s at %s' % (c.name, ','.join(map(str, bad))))
        print('    want %s' % ' '.join('-' if e is None else '%08x' % bits(f32(e)) for e in c.expect))
        print('    got  %s' % ' '.join('%08x' % x for x in w))
    for n in [n for n in crashed if not n.startswith('corpus_')][:20]:
        print('  CRASH %s' % n)
    for o in other[:20]:
        print('  NO RESULT %s' % o)
    if corpora:
        npairs = len([n for n in corpus if n.endswith('_orig')])
        print('[ref] corpus crs differential: %d pairs compared, %d equal (%d bit-exact), %d differ, '
              '%d crashed' % (npairs, cmatch, cexact, len(cdiff), len(corpus_crash)))
        for n in cdiff[:20]:
            print('  DIFFER %s' % n)
    by_kind = {}
    for c, _, _ in fails:
        k = '_'.join(c.name.split('_')[:3])
        by_kind[k] = by_kind.get(k, 0) + 1
    if by_kind:
        print('[ref] mismatches by kind: %s' % ', '.join('%s %d' % kv for kv in sorted(by_kind.items())))
    ok = not fails and not crashed and not other and not cdiff
    print('[ref] %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
