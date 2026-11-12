#!/usr/bin/env python3
"""cmp-hash-runs.py: compare two runs of the hash suite, base against head.

    cmp-hash-runs.py <base hashes.txt> <head hashes.txt> [--allow t1,t2] [--new t1,t2]
                     [--allow-notclean t1,t2] [--stats <base stats.txt> <head stats.txt>]

Each hashes file is scripts/run-hash-tests.sh's hashes.txt: one line per program,
"<name> rc=<exit> fails=<FAIL lines> <hash of each frame>…".

Exit status 0 when all of these hold, 1 otherwise:
- every program both runs have has the same frame hashes, in the same number. code_mtlock is
  exempt (its frames depend on thread timing, docs/testing.md), and so are the --allow names;
- no program is missing from head that base has;
- every program of head ended rc=0 with no FAIL line and with at least one frame. The
  --allow-notclean names are excused only where base ended the same way (the same exit status
  and FAIL count): a program that is unclean on base too, never one this change broke, and
  never a program only head has;
- a program only head has is named in --new (a new test of this change);
- with --stats (the two runs' stats.txt): head has no lifetime violation, no program without
  proof that d9mt drew it and no GPU-lost line, and no more Metal validation lines and
  "Invalid Metal usage" lines than base (the Definition of Done: no new ones).
The printed lines say what was found, whatever the status.
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
    # excused only when base is unclean in the same way
    excused = {t for t in allow_notclean
               if t in base and base[t][:2] == head.get(t, (None, None))[:2] and bool(base[t][2]) == bool(head[t][2])}
    bad_notclean = [t for t in notclean if t not in excused]
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


def load_stats(path):
    """The STATS line of a run's stats.txt as a dict, or None."""
    with open(path) as f:
        for line in f:
            w = line.split()
            if w and w[0] == 'STATS':
                return dict(kv.split('=', 1) for kv in w[1:] if '=' in kv)
    return None


def compare_stats(base, head):
    """Returns (ok, line): head's diagnostics against base's."""
    if base is None or head is None:
        return False, 'diagnostics: no STATS line in %s' % ('both files' if base is None and head is None
                                                            else 'the base file' if base is None else 'the head file')
    bad = []
    try:
        for key, what in (('validation', 'Metal validation lines'), ('invalid-usage', '"Invalid Metal usage" lines')):
            if int(head[key]) > int(base[key]):
                bad.append('%s: %s on head, %s on base' % (what, head[key], base[key]))
        if int(head['lifetime']) != 0:
            bad.append('lifetime violations on head: %s' % head['lifetime'])
        if int(head['gpu-lost']) != 0:
            bad.append('GPU-lost lines on head: %s' % head['gpu-lost'])
        if head['no-d9mt-proof'] != 'none':
            bad.append('no proof that d9mt drew: %s' % head['no-d9mt-proof'])
    except (KeyError, ValueError) as e:
        return False, 'diagnostics: a STATS line is missing a count (%s)' % e
    line = ('diagnostics (variant %s): validation %s (base %s), invalid usage %s (base %s), lifetime %s: %s'
            % (head.get('variant', '?'), head['validation'], base['validation'], head['invalid-usage'],
               base['invalid-usage'], head['lifetime'], '; '.join(bad) or 'nothing new'))
    return not bad, line


def main(argv):
    opts = {'--allow': [], '--new': [], '--allow-notclean': []}
    files = []
    stats = None
    i = 0
    while i < len(argv):
        if argv[i] == '--stats':
            if i + 2 >= len(argv):
                sys.exit('--stats needs the base and the head stats.txt')
            stats = (argv[i + 1], argv[i + 2])
            i += 3
        elif argv[i] in opts:
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
    if stats:
        stats_ok, line = compare_stats(load_stats(stats[0]), load_stats(stats[1]))
        lines.append(line)
        ok = ok and stats_ok
    print('\n'.join(lines))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
