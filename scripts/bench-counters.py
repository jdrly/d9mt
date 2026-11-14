#!/usr/bin/env python3
"""bench-counters.py: d9mt's per-frame counters of a base run and a head run, side by side.

    bench-counters.py <base log> <head log> [--repeat <base log 2> <head log 2>]

Each log is the stderr of one bench run with D9MT_PROFILE=1 (ci/gpu/run.sh bench, part A). From
each it takes the last PROF line (a full window: PROF is printed every 256 frames and never for a
partial one) and the last STATS line (a full second, for the same reason), and from them every
counter, never a time:
- PROF: the fields after the first "|". A field printed as <total>(<per frame>/f) gives
  total / win, which is exact; <x>/f gives x; a bare number is the window's total. Left out: the
  bucket times before the first "|", unsubWaitUs, csExec and allocWaitMs. The fields after
  "passClose/f:" are named passClose.<cause>.
- STATS: the fields after the first "|", as printed (per frame of the render thread, the flush
  requests per Present as <name>.flushed and <name>.refused). Left out: t, fps, ms, slow33 and
  slow50 before the first "|", attMs, the ms of wait=<count>/<ms>ms, and at=.

Output on stdout, tab-separated, a header first: line, counter, base, head, diff (head - base,
or "differs" for a text field), and with --repeat the column repeat: "yes" when each side's second
run printed the same value as its first, else what the second runs printed. A counter that only
one side prints (an older base) has "-" on the other side.

Exit status: 0 the table was written (whatever it says); 1 a log can't be read or has no PROF or
no STATS line (the run wasn't profiled, or didn't get through a window); 2 usage.
"""
import re
import sys

PROF_TIMES = {'unsubWaitUs', 'csExec', 'allocWaitMs'}
STATS_SKIP = {'attMs', 'at'}
NUM = r'-?\d+(?:\.\d+)?(?:e[-+]?\d+)?'


def last_line(path, tag):
    found = None
    with open(path, errors='replace') as f:
        for line in f:
            m = re.search(r'(?:^|\s)(' + re.escape(tag) + r'.*)$', line.rstrip('\r\n'))
            if m:
                found = m.group(1)
    return found


def number(text):
    v = float(text)
    return int(v) if v == int(v) and 'e' not in text and '.' not in text else v


def prof(line):
    """{counter: value} of a PROF line."""
    m = re.search(r'\bwin=(\d+)\b', line)
    win = int(m.group(1)) if m else 256
    out = {}
    prefix = ''
    for section in line.split(' | ')[1:]:
        prefix = ''
        for tok in section.split():
            if tok.endswith(':'):
                prefix = tok[:-1].split('/')[0] + '.'
                continue
            if '=' not in tok:
                continue
            key, val = tok.split('=', 1)
            if key in PROF_TIMES:
                continue
            key = prefix + key
            if m := re.fullmatch(f'({NUM})\\(({NUM})/f\\)', val):
                out[key + '/f'] = number(m.group(1)) / win
            elif m := re.fullmatch(f'({NUM})/f', val):
                out[key + '/f'] = number(m.group(1))
            elif re.fullmatch(NUM, val):
                out[key] = number(val)
            else:
                out[key] = val
    return out


def stats(line):
    """{counter: value} of a STATS line."""
    out = {}
    for section in line.split(' | ')[1:]:
        for tok in section.split():
            if '=' not in tok:
                continue
            key, val = tok.split('=', 1)
            if key in STATS_SKIP:
                continue
            if m := re.fullmatch(f'(\\d+)/{NUM}ms', val):
                out[key] = number(m.group(1))
            elif key.startswith('flush') and (m := re.fullmatch(f'({NUM})/({NUM})', val)):
                out[key + '.flushed'] = number(m.group(1))
                out[key + '.refused'] = number(m.group(2))
            elif re.fullmatch(NUM, val):
                out[key] = number(val)
            else:
                out[key] = val
    return out


def counters(path):
    try:
        p, s = last_line(path, 'PROF frame='), last_line(path, 'STATS t=')
    except OSError as e:
        sys.exit(f'bench-counters: {e}')
    missing = [n for n, l in (('PROF', p), ('STATS', s)) if l is None]
    if missing:
        print(f'bench-counters: {path} has no {" and no ".join(missing)} line', file=sys.stderr)
        sys.exit(1)
    return {'PROF': prof(p), 'STATS': stats(s)}


def show(v):
    if v is None:
        return '-'
    if isinstance(v, float):
        return f'{v:.6f}'.rstrip('0').rstrip('.')
    return str(v)


def diff(a, b):
    if a is None or b is None:
        return ''
    if isinstance(a, str) or isinstance(b, str):
        return '' if a == b else 'differs'
    return show(b - a) if b < a else '+' + show(b - a)


def main(argv):
    if len(argv) == 2:
        logs, repeat = argv, None
    elif len(argv) == 5 and argv[2] == '--repeat':
        logs, repeat = argv[:2], argv[3:]
    else:
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    base, head = (counters(p) for p in logs)
    base2, head2 = (counters(p) for p in repeat) if repeat else (None, None)
    print('line\tcounter\tbase\thead\tdiff' + ('\trepeat' if repeat else ''))
    for line in ('PROF', 'STATS'):
        keys = list(base[line]) + [k for k in head[line] if k not in base[line]]
        for k in keys:
            a, b = base[line].get(k), head[line].get(k)
            row = [line, k, show(a), show(b), diff(a, b)]
            if repeat:
                a2, b2 = base2[line].get(k), head2[line].get(k)
                row.append('yes' if (a2, b2) == (a, b) else f'no: base {show(a2)}, head {show(b2)}')
            print('\t'.join(row))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
