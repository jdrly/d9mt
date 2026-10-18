#!/usr/bin/env python3
"""Detection test for the "D3D9 SM3 hazard" warning (d3d9_shader_analysis.cpp,
GitHub issue #8): creates every shader with the driver under Wine (nothing is
compiled: D9MT_COMPILE_THREADS=0, D9MT_PS_PREFETCH=0) and checks that the
d3d9 log has exactly the warnings an independent scan of the shader's
assembly text predicts, in creation order, and none for anything else.

  ENGINE=<engine> [FRAMEWORKS=<dir>] [WORK=<dir>] \\
    scripts/sm3_hazard_test.py <d3d9.dll> [<dir with .asm/.bin>...]

Always includes a built-in targeted set (every crs/setp write mask in VS and
PS, packed and duplicate I/O declarations, implicit declarations), whose
expectations are written out by hand as well; corpus directories add their
shaders (an .asm next to a .bin is scanned, a lone .bin must not warn).
Exit 0 when every warning matches.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sm3_corpus  # noqa: E402
from sm3_wine import WineRunner  # noqa: E402

PS_HDR = 'ps_3_0\ndef c0, 1, 2, 3, 4\ndef c1, 4, 3, 2, 1\n'
VS_HDR = 'vs_3_0\ndcl_position v0\ndcl_position o0\ndef c0, 1, 2, 3, 4\ndef c1, 4, 3, 2, 1\n'
MASKS = ['x', 'y', 'z', 'w', 'xy', 'xz', 'xw', 'yz', 'yw', 'zw', 'xyz', 'xyw', 'xzw', 'yzw', 'xyzw']


def targeted():
    """name -> (asm, hand-written expected items)"""
    cases = {}
    for st in ('ps', 'vs'):
        hdr = PS_HDR if st == 'ps' else VS_HDR
        out = 'mov oC0, r2\n' if st == 'ps' else 'mov o0, v0\nmov o0.x, r2.x\n'
        for m in MASKS:
            cases['%s_crs_%s' % (st, m)] = (
                hdr + 'mov r0, c0\nmov r1, c1\nmov r2, c0\ncrs r2.%s, r0, r1\n' % m + out,
                [] if m in ('x', 'xy', 'xyz') else ['crs-partial-mask:r2.%s' % m])
            cases['%s_setp_%s' % (st, m)] = (
                hdr + 'setp_gt p0.%s, c0, c1\n(p0) mov r2, c0\nmov r2, c1\n' % m + out,
                [] if m in ('x', 'xy', 'xyz', 'xyzw') else ['setp-partial-mask:p0.%s' % m])
    cases.update({
        'ps_packed': ('ps_3_0\ndcl_texcoord0 v0.xy\ndcl_texcoord1 v0.zw\nmov oC0, v0\n',
                      ['packed-io:v0.xy=texcoord0+v0.zw=texcoord1']),
        'ps_packed3': ('ps_3_0\ndcl_normal v0.xyz\ndcl_blendweight v0.w\ndcl_texcoord1 v1.y\n'
                       'dcl_texcoord0 v1.zw\nmov r0, v0\nadd oC0, r0, v1\n',
                       ['packed-io:v0.xyz=normal0+v0.w=blendweight0',
                        'packed-io:v1.y=texcoord1+v1.zw=texcoord0']),
        'ps_dupsem_split': ('ps_3_0\ndcl_texcoord0 v0.xy\ndcl_texcoord0 v0.zw\nmov oC0, v0\n',
                            ['packed-io:v0.xy=texcoord0+v0.zw=texcoord0']),
        'ps_dupsem_tworeg': ('ps_3_0\ndcl_texcoord0 v0.xy\ndcl_texcoord0 v1.zw\nadd oC0, v0, v1\n',
                             ['packed-io:v0.xy=texcoord0+v1.zw=texcoord0']),
        'ps_implicit_collide': ('ps_3_0\ndcl_color1 v0\nadd oC0, v0, v1\n',
                                ['packed-io:v0=color1+v1(undeclared)=color1']),
        'ps_implicit_ok': ('ps_3_0\ndcl_texcoord0 v0\nadd oC0, v0, v1\n', []),
        'ps_crs_implicit': ('ps_3_0\ndcl_color1 v0\ncrs r2.xyz, v0, v1\nmov oC0, r2\n',
                            ['packed-io:v0=color1+v1(undeclared)=color1']),
        'ps_setp_implicit': ('ps_3_0\ndef c0, 0, 0, 0, 0\ndcl_color1 v0\nsetp_gt p0, v1, v0\n'
                             '(p0) mov r0, c0\nmov oC0, r0\n',
                             ['packed-io:v0=color1+v1(undeclared)=color1']),
        'vs_crs_implicit': ('vs_3_0\ndcl_position v0\ndcl_color3 v1\ndcl_position o0\n'
                            'crs r0.yz, v1, v3\nmov o0, v0\nmov o0.yz, r0\n',
                            ['crs-partial-mask:r0.yz', 'packed-io:v1=color3+v3(undeclared)=color3']),
        'ps_vpos_position': ('ps_3_0\ndcl vPos.xy\ndcl_position v0\nadd oC0, v0, vPos.xyyy\n',
                             ['packed-io:vPos.xy=position0+v0=position0']),
        'ps_position_vpos': ('ps_3_0\ndcl_position v0\ndcl vPos.xy\nadd oC0, v0, vPos.xyyy\n', []),
        'ps_plain': ('ps_3_0\ndcl_texcoord0 v0.xy\ndcl_texcoord1 v1.zw\nadd oC0, v0, v1\n', []),
        'vs_packed_in': ('vs_3_0\ndcl_position v0.xy\ndcl_normal v0.zw\ndcl_position o0\nmov o0, v0\n',
                         ['packed-io:v0.xy=position0+v0.zw=normal0']),
        'vs_packed_out': ('vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_texcoord0 o1.xy\n'
                          'dcl_texcoord1 o1.zw\nmov o0, v0\nmov o1, v0\n',
                          ['packed-io:o1.xy=texcoord0+o1.zw=texcoord1']),
        'vs_psize_packed': ('vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_texcoord0 o1.xyz\n'
                            'dcl_psize o1.w\nmov o0, v0\nmov o1, v0\n',
                            ['packed-io:o1.xyz=texcoord0+o1.w=psize0']),
        'vs_implicit_out': ('vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_texcoord3 o1\n'
                            'mov o0, v0\nmov o1, v0\nmov o3, v0\n',
                            ['packed-io:o1=texcoord3+o3(undeclared)=texcoord3']),
        'vs_implicit_in': ('vs_3_0\ndcl_position v0\ndcl_color2 v1\ndcl_position o0\n'
                           'mov o0, v0\nadd o0, v1, v2\n',
                           ['packed-io:v1=color2+v2(undeclared)=color2']),
        'vs_plain': ('vs_3_0\ndcl_position v0\ndcl_normal v1\ndcl_position o0\ndcl_texcoord0 o1.xy\n'
                     'dcl_texcoord1 o2\nmov o0, v0\nmov o1, v1\nmov o2, v1\n', []),
    })
    # 64 input semantics (16 registers x 4 components): more than the vertex
    # input signature holds (31); creation must still succeed
    many = 'vs_3_0\n' + ''.join('dcl_%s%d v%d.%s\n' % (u, r, r, c) for r in range(16)
                                 for u, c in zip(('texcoord', 'normal', 'tangent', 'binormal'), 'xyzw'))
    many += 'dcl_position o0\nmov o0, v0\n'
    cases['vs_many_semantics'] = (many, None)
    return cases


# --- independent scan of the assembly text ------------------------------------

USAGES = ['position', 'blendweight', 'blendindices', 'normal', 'psize', 'texcoord', 'tangent',
          'binormal', 'tessfactor', 'positiont', 'color', 'fog', 'depth', 'sample']
REG = re.compile(r'^-?!?(?:1-)?(vPos|vFace|oC|oDepth|[a-zA-Z])(\d*)(\[[^\]]*\])?')


def mask_str(m, omit_full=True):
    return '' if (m == 'xyzw' and omit_full) else '.' + m


def scan(text):
    """The hazard items the driver must report, from the assembly text alone."""
    lines = [l.split(';')[0].split('//')[0].strip() for l in text.splitlines()]
    lines = [l for l in lines if l]
    stage = lines[0][:2]
    items, dcls = [], []

    def reg_name(kind, idx):
        return {'misc0': 'vPos', 'misc1': 'vFace'}.get(kind + str(idx), '%s%d' % (kind, idx))

    def add(entry, implicit):
        is_in, kind, idx, sem, mask = entry
        if implicit and any(e[0] == is_in and e[1] == kind and e[2] == idx for e in dcls):
            return
        regular = kind in ('v', 'o')
        for e in dcls:
            if e[0] != is_in:
                continue
            if (e[1] == kind and e[2] == idx) or (regular and e[3] == sem):
                items.append('packed-io:%s%s=%s+%s%s%s=%s' % (
                    reg_name(e[1], e[2]), mask_str(e[4]), e[3], reg_name(kind, idx), mask_str(mask),
                    '(undeclared)' if implicit else '', sem))
                break
        dcls.append(entry)

    for line in lines[1:]:
        line = re.sub(r'^\(!?p0(\.[xyzw])?\)\s*', '', line)
        op, _, rest = line.partition(' ')
        args = [a.strip() for a in sm3_corpus.split_args(rest)] if rest.strip() else []
        if op.startswith('dcl'):
            usage = op[4:] if op.startswith('dcl_') else ''
            if usage in ('2d', 'cube', 'volume'):
                continue
            reg, _, mask = args[0].partition('.')
            mask = mask or 'xyzw'
            m = re.match(r'^([a-z]+?)(\d*)$', usage)
            sem = '%s%d' % (m.group(1), int(m.group(2) or 0)) if usage else 'position0'
            if reg in ('vPos', 'vFace'):
                add((True, 'misc', 0 if reg == 'vPos' else 1, sem, mask), False)
            else:
                add((reg[0] == 'v', reg[0], int(reg[1:]), sem, mask), False)
            continue
        base = re.sub(r'_(sat|pp|centroid|gt|eq|ge|lt|ne|le)$', '', op)
        base = re.sub(r'_(sat|pp|centroid)$', '', base)
        if base in ('def', 'defi', 'defb'):
            continue
        has_dst = base not in ('if', 'ifc', 'else', 'endif', 'rep', 'endrep', 'loop', 'endloop',
                               'break', 'breakc', 'breakp', 'ret', 'call', 'callnz', 'label', 'nop')
        dst, srcs = (args[0], args[1:]) if has_dst and args else (None, args)
        if base in ('crs', 'setp') and dst:
            m = dst.partition('.')[2] or 'xyzw'
            ok = ('x', 'xy', 'xyz') if base == 'crs' else ('x', 'xy', 'xyz', 'xyzw')
            if m not in ok:
                items.append('%s-partial-mask:%s.%s' % (base, dst.partition('.')[0], m))
        for s in srcs:
            r = REG.match(s.replace(' ', ''))
            if r and r.group(1) == 'v' and not r.group(3):
                idx = int(r.group(2))
                add((True, 'v', idx, 'color%d' % idx, 'xyzw'), True)
        if stage == 'vs' and dst:
            r = REG.match(dst)
            if r and r.group(1) == 'o' and not r.group(3):
                idx = int(r.group(2))
                add((False, 'o', idx, 'texcoord%d' % idx, 'xyzw'), True)
    return items


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    dll = argv[0]
    runner = WineRunner(dll)
    sdir = runner.reset_dir('s')

    shaders = []  # (file name, bytecode, expected items)
    for name, (text, hand) in targeted().items():
        got = scan(text)
        if hand is None:
            hand = got  # too many items to write out; the scan is the expectation
        if got != hand:
            print('SCANNER MISMATCH %s: hand %s scan %s' % (name, hand, got))
            return 1
        shaders.append(('t_' + name + '.bin', sm3_corpus.assemble(text), hand))
    for d in argv[1:]:
        for f in sorted(os.listdir(d)):
            if not f.endswith('.bin'):
                continue
            code = open(os.path.join(d, f), 'rb').read()
            asm = os.path.join(d, f[:-4] + '.asm')
            expect = scan(open(asm).read()) if os.path.exists(asm) else []
            shaders.append(('c%d_%s' % (argv.index(d), f), code, expect))

    seen = set()
    expected_lines = []
    for f, code, expect in shaders:
        open(os.path.join(sdir, f), 'wb').write(code)
        if code in seen:
            continue  # same bytecode: the driver reuses the module, no second warning
        seen.add(code)
        if expect:
            stage = 'VS' if code[2:4] == b'\xfe\xff' else 'PS'
            expected_lines.append((f, stage, ' '.join(expect)))

    results, crashes, log = runner.run(['create s/%s' % f for f, _, _ in shaders],
                                       {'D9MT_COMPILE_THREADS': '0', 'D9MT_PS_PREFETCH': '0'})
    got = re.findall(r'D3D9 SM3 hazard: ([vp]s_\d+) (VS|PS) (.*)', log)
    failed = [i for i, l in results.items() if not l.endswith(' 00000000')]
    print('[hazard] %d shaders created (%d distinct), %d create failures, %d crashes'
          % (len(shaders), len(seen), len(failed), len(crashes)))
    print('[hazard] %d warnings expected, %d logged' % (len(expected_lines), len(got)))
    rc = 0 if not failed and not crashes else 1
    for i in range(max(len(expected_lines), len(got))):
        e = expected_lines[i] if i < len(expected_lines) else None
        g = got[i] if i < len(got) else None
        if not e or not g or e[1] != g[1] or e[2] != g[2].strip():
            print('  MISMATCH #%d expected %s got %s' % (i, e, g))
            rc = 1
    kinds = {}
    for _, _, items in expected_lines:
        for it in items.split():
            k = it.split(':')[0]
            kinds[k] = kinds.get(k, 0) + 1
    print('[hazard] items: %s' % ', '.join('%s %d' % kv for kv in sorted(kinds.items())))
    full = log.count('vertex input semantics,')
    print('[hazard] input signature full: %d warnings (expected 33)' % full)
    if full != 33:
        rc = 1
    print('[hazard] %s' % ('PASS' if rc == 0 else 'FAIL'))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
