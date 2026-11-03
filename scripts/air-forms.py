#!/usr/bin/env python3
"""air-forms.py: the checks of scripts/test-air-forms.sh (air/tests/forms/inventory.txt).

  air-forms.py sites <inventory.txt> <air_builder.cpp>
      Completeness: every "air.… literal in the AirBuilder source has a site or meta line, every
      site's forms are inventory forms, and every form is produced by some site.
  air-forms.py groups <inventory.txt>
      The Apple groups the forms use, one per line.
  air-forms.py compare <inventory.txt> <target> <ours dir> <apple dir> [<corpus dir>]
      <ours dir>/<kind>_<name>.ll: each fixture's disassembled bitcode (ir_math.ll, sm3_ps_texld.ll);
      <apple dir>/<group>.ll: Apple's LLVM text per forms.metal group (missing = Apple rejected it);
      <corpus dir>/*.ll: optional, every SM3 corpus shader's bitcode, whose air.* declarations
      must all be inventory forms.
      Fails when a fixture doesn't declare its form, when a form's normalized declaration differs
      from Apple's, when Apple's group lacks the form (unless apple-unmatched), or when any fixture
      or corpus shader declares an air.* function the inventory doesn't list. Prints one line per
      form and lists the apple-unmatched ones for review.

Normalization (both sides): named struct types compare by name; attribute groups (#N),
local_unnamed_addr and the parameter attributes nocapture and readonly are dropped; return and
parameter types and address spaces compare exactly.
"""
import glob
import os
import re
import sys

DECL = re.compile(r'^declare\s+(.*?)\s+@("?)([A-Za-z0-9_.$]+)\2\((.*)\)(.*)$')
DROP_PARAM_ATTRS = {'nocapture', 'readonly'}


def load_inventory(path):
    forms, sites, metas = {}, {}, set()
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        kind, rest = line.split(None, 1)
        if kind == 'form':
            unmatched = None
            if 'apple-unmatched:' in rest:
                rest, unmatched = rest.split('apple-unmatched:', 1)
                unmatched = unmatched.strip()
            tok = rest.split()
            name, fixtures = tok[0], [t for t in tok[1:] if t.startswith(('ir:', 'sm3:'))]
            groups = [t for t in tok[1:] if not t.startswith(('ir:', 'sm3:'))]
            if not fixtures or len(groups) != 1:
                sys.exit('%s:%d: a form needs fixtures and one apple group' % (path, n))
            forms[name] = {'fixtures': fixtures, 'group': groups[0], 'unmatched': unmatched}
        elif kind == 'site':
            m = re.match(r'"([^"]+)"\s+(.*)$', rest)
            sites[m.group(1)] = m.group(2).split()
        elif kind == 'meta':
            metas.add(re.match(r'"([^"]+)"', rest).group(1))
        else:
            sys.exit('%s:%d: unknown line kind %s' % (path, n, kind))
    return forms, sites, metas


def split_top(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch in '({<[':
            depth += 1
        elif ch in ')}>]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def norm_param(p):
    return ' '.join(t for t in p.split() if t not in DROP_PARAM_ATTRS)


def declarations(path):
    """{name: normalized 'ret (params)'} for every air.* declaration in an .ll file."""
    out = {}
    for line in open(path):
        m = DECL.match(line.strip())
        if not m or not m.group(3).startswith('air.'):
            continue
        ret = ' '.join(t for t in m.group(1).split() if t not in ('local_unnamed_addr',) and not t.startswith('#'))
        params = ', '.join(norm_param(p) for p in split_top(m.group(4)))
        out[m.group(3)] = '%s (%s)' % (ret, params)
    return out


def cmd_sites(inv, src):
    forms, sites, metas = load_inventory(inv)
    text = open(src).read()
    literals = set(re.findall(r'"(air\.[A-Za-z0-9_.]*)', text))
    errs = []
    for lit in sorted(literals):
        if lit not in sites and lit not in metas:
            errs.append('literal "%s" has no site or meta line (a new emitter?)' % lit)
    for lit in sorted(set(sites) | metas):
        if lit not in literals:
            errs.append('inventory names "%s", which air_builder.cpp no longer has' % lit)
    produced = set()
    for lit, fs in sites.items():
        for f in fs:
            produced.add(f)
            if f not in forms:
                errs.append('site "%s" produces %s, which is no form' % (lit, f))
    for f in forms:
        if f not in produced:
            errs.append('form %s comes from no site' % f)
    for e in errs:
        print('FAIL sites: %s' % e)
    print('sites: %d literals, %d sites, %d meta, %d forms: %s'
          % (len(literals), len(sites), len(metas), len(forms), 'FAIL' if errs else 'PASS'))
    return 1 if errs else 0


def cmd_groups(inv):
    forms, _, _ = load_inventory(inv)
    for g in sorted({f['group'] for f in forms.values()}):
        print(g)
    return 0


def cmd_compare(inv, target, ours, apple, corpus=None):
    forms, _, _ = load_inventory(inv)
    fail, unmatched = 0, []
    cache = {}

    def decls(path):
        if path not in cache:
            cache[path] = declarations(path) if os.path.exists(path) else None
        return cache[path]

    for name in sorted(forms):
        f = forms[name]
        errs, ours_decl = [], None
        for fx in f['fixtures']:
            kind, fname = fx.split(':', 1)
            d = decls(os.path.join(ours, '%s_%s.ll' % (kind, fname)))
            if d is None:
                errs.append('%s: no output' % fx)
            elif name not in d:
                errs.append('%s does not declare it' % fx)
            elif ours_decl is None:
                ours_decl = d[name]
            elif d[name] != ours_decl:
                errs.append('%s declares it differently: %s' % (fx, d[name]))
        a = decls(os.path.join(apple, '%s.ll' % f['group']))
        if f['unmatched']:
            unmatched.append('%s (%s)' % (name, f['unmatched']))
            note = 'apple-unmatched, review: %s' % f['unmatched']
        elif a is None:
            errs.append('Apple rejected group %s' % f['group'])
        elif name not in a:
            errs.append("Apple's group %s doesn't declare it" % f['group'])
        elif ours_decl is not None and a[name] != ours_decl:
            errs.append('ours %s != Apple %s' % (ours_decl, a[name]))
        if errs:
            fail = 1
            print('%-44s FAIL [%s] %s' % (name, target, '; '.join(errs)))
        else:
            print('%-44s PASS [%s] %s' % (name, target, note if f['unmatched'] else ours_decl))
    # nothing outside the inventory
    seen = {}
    for path in sorted(glob.glob(os.path.join(ours, '*.ll'))
                       + (sorted(glob.glob(os.path.join(corpus, '*.ll'))) if corpus else [])):
        for n in decls(path) or {}:
            seen.setdefault(n, os.path.basename(path))
    for n in sorted(set(seen) - set(forms)):
        fail = 1
        print('%-44s FAIL [%s] declared by %s but not in the inventory' % (n, target, seen[n]))
    if unmatched:
        print('apple-unmatched, for review: %s' % ', '.join(unmatched))
    return fail


def main(argv):
    if len(argv) >= 3 and argv[0] == 'sites':
        return cmd_sites(argv[1], argv[2])
    if len(argv) >= 2 and argv[0] == 'groups':
        return cmd_groups(argv[1])
    if len(argv) >= 5 and argv[0] == 'compare':
        return cmd_compare(*argv[1:6])
    sys.exit(__doc__)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
