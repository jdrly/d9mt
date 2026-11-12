#!/usr/bin/env python3
"""cmp-hash-runs.py: compare two runs of the hash suite, base against head.

    cmp-hash-runs.py <base hashes.txt> <head hashes.txt> [--allow t1,t2] [--new t1,t2]
                     [--allow-notclean t1,t2]

Each file is scripts/run-hash-tests.sh's hashes.txt: one line per program,
"<name> rc=<exit> fails=<FAIL lines> <hash of each frame>…".

Exit status 0 when all of these hold, 1 otherwise:
- every program both runs have has the same frame hashes, in the same number. code_mtlock is
  exempt (its frames depend on thread timing, docs/testing.md), and so are the --allow names;
- no program is missing from head that base has;
- every program of head ended rc=0 with no FAIL line and with at least one frame, except the
  --allow-notclean names (a program that isn't clean on base either: say so in the pull
  request);
- a program only head has is named in --new (a new test of this change).
The four printed lines say what was found, whatever the status.
"""
import sys

NONDETERMINISTIC = {'code_mtlock'}


def load(path):
    runs = {}
    with open(path) as f:
        for line in f:
            w = line.split()
            if len(w) >= 3 and w[1].startswith('rc=') and w[2].startswith('fails='):
                runs[w[0]] = (w[1], w[2], w[3:])
    return runs


def compare(base, head, allow=(), new=(), allow_notclean=()):
    """Returns (ok, lines)."""
    allow = NONDETERMINISTIC | set(allow)
    common = sorted(set(base) & set(head))
    differ = [t for t in common if base[t][2] != head[t][2]]
    bad_differ = [t for t in differ if t not in allow]
    changed = ['%s: %s %s -> %s %s' % (t, base[t][0], base[t][1], head[t][0], head[t][1])
               for t in common if base[t][:2] != head[t][:2]]
    only_head = sorted(set(head) - set(base))
    only_base = sorted(set(base) - set(head))
    unannounced = [t for t in only_head if t not in set(new)]
    notclean = [t for t in sorted(head) if (head[t][0], head[t][1]) != ('rc=0', 'fails=0') or not head[t][2]]
    bad_notclean = [t for t in notclean if t not in set(allow_notclean)]
    lines = [
        'common %d programs; frames differ: %s; not allowed to: %s'
        % (len(common), ' '.join(differ) or 'none', ' '.join(bad_differ) or 'none'),
        'exit status or FAIL lines changed: %s' % ('; '.join(changed) or 'none'),
        'only in head: %s (not named in --new: %s); only in base: %s'
        % (' '.join(only_head) or 'none', ' '.join(unannounced) or 'none', ' '.join(only_base) or 'none'),
        'head not clean (rc, FAIL lines or no frame): %s; not allowed to be: %s'
        % (' '.join('%s(%s,%s,%d frames)' % (t, head[t][0], head[t][1], len(head[t][2])) for t in notclean) or 'none',
           ' '.join(bad_notclean) or 'none'),
    ]
    ok = not (bad_differ or only_base or unannounced or bad_notclean) and bool(common)
    if not common:
        lines.append('no program in common: nothing was compared')
    return ok, lines


def main(argv):
    opts = {'--allow': [], '--new': [], '--allow-notclean': []}
    files = []
    i = 0
    while i < len(argv):
        if argv[i] in opts:
            if i + 1 >= len(argv):
                sys.exit('%s needs a comma-separated list' % argv[i])
            opts[argv[i]] += argv[i + 1].split(',')
            i += 2
        elif argv[i].startswith('--'):
            sys.exit('unknown option %s' % argv[i])
        else:
            files.append(argv[i])
            i += 1
    if len(files) != 2:
        sys.exit(__doc__.strip().split('\n\n')[1])
    ok, lines = compare(load(files[0]), load(files[1]), opts['--allow'], opts['--new'], opts['--allow-notclean'])
    print('\n'.join(lines))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
