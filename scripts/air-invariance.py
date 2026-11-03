#!/usr/bin/env python3
"""air-invariance.py: the position-invariance form of AIR (scripts/test-air-invariance.sh).

  air-invariance.py [--llvm-dis P] FILE...   one JSON line of the form per FILE

A FILE is a metallib (its bitcode is disassembled with --llvm-dis) or LLVM text (.ll), d9mt's or
Apple's (`xcrun metal -S -emit-llvm`). The form:
  "attrs":     the floating-point function attributes of every defined function ("unsafe-fp-math",
               "no-nans-fp-math", the three "invariance-late-*" ...), as one sorted list; null if
               two functions disagree;
  "fmf":       every fast-math flag set on an instruction (fmul, fadd, fcmp, calls ...), sorted;
  "invariant": the vertex function's position output says air.invariant (null: no position output);
  "fast_math": air.compile_options' fast_math_enable (true) or fast_math_disable (false).
"""
import json
import re
import struct
import subprocess
import sys
import tempfile

FLAGS = ('fast', 'reassoc', 'nnan', 'ninf', 'nsz', 'arcp', 'contract', 'afn')
FMF_RE = re.compile(r'\b(?:fmul|fadd|fsub|fdiv|frem|fneg|fcmp|call|select|phi)((?: (?:%s))+)\b' % '|'.join(FLAGS))
FP_ATTR = re.compile(r'^"((?:[a-z-]*fp-math)|no-trapping-math|invariance-late-[a-z-]+)"(?:="([^"]*)")?$')


def ll_text(path, llvm_dis):
    data = open(path, 'rb').read()
    if path.endswith('.ll'):
        return data.decode()
    if data[:4] != b'MTLB':
        raise ValueError('%s: not a metallib' % path)
    bc_off, bc_size = struct.unpack_from('<QQ', data, 0x48)
    with tempfile.NamedTemporaryFile(suffix='.bc') as bc:
        bc.write(data[bc_off:bc_off + bc_size])
        bc.flush()
        return subprocess.run([llvm_dis, bc.name, '-o', '-'], capture_output=True, check=True).stdout.decode()


def attr_tokens(body):
    return re.findall(r'"[^"]*"(?:="[^"]*")?|[^\s"]+', body)


def form(t):
    groups = dict(re.findall(r'^attributes (#[0-9]+) = \{ (.*) \}$', t, re.M))
    sets = set()
    for g in re.findall(r'^define [^\n]*\) (?:[a-z_ ]*)?(#[0-9]+)[^\n]*\{$', t, re.M):
        attrs = []
        for tok in attr_tokens(groups.get(g, '')):
            m = FP_ATTR.match(tok)
            if m:
                attrs.append(m.group(1) + ('=' + m.group(2) if m.group(2) is not None else ''))
        sets.add(tuple(sorted(attrs)))
    fmf = sorted({' '.join(m.group(1).split()) for m in FMF_RE.finditer(t)})
    md = dict(re.findall(r'^(![0-9]+) = (?:distinct )?!\{(.*)\}$', t, re.M))
    invariant = None
    for v in md.values():
        if '!"air.position"' in v and '!"air.arg_name"' in v:
            # a vertex output (a fragment input's position carries air.center etc., never invariant)
            if '!"air.invariant"' in v:
                invariant = True
            elif invariant is None and 'air.center' not in v:
                invariant = False
    fast = None
    if '!"air.compile.fast_math_enable"' in t:
        fast = True
    elif '!"air.compile.fast_math_disable"' in t:
        fast = False
    return {'attrs': list(sets.pop()) if len(sets) == 1 else None, 'fmf': fmf,
            'invariant': invariant, 'fast_math': fast}


def main(argv):
    llvm_dis, files = None, []
    i = 0
    while i < len(argv):
        if argv[i] == '--llvm-dis':
            llvm_dis = argv[i + 1]; i += 2
        else:
            files.append(argv[i]); i += 1
    if not files:
        sys.exit(__doc__)
    for f in files:
        print(json.dumps(form(ll_text(f, llvm_dis)), sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
