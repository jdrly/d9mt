#!/usr/bin/env python3
"""perf-compare.py — compares the variants of rotated-round bench results (scripts/perf-abba.sh).

  scripts/perf-compare.py [--ref NAME] [--control NAME] [--metric KEY] results.txt [...]

Input lines: `round=<r> v=<variant> ... <metric>=<value>` (or block=/pair=); lines starting with DIRTY (another
engine's Wine was running) or holding `RESULT FAIL` are skipped and counted. The metric is the
first of avg_ms, ms/frame, frame_median_ms, median_ns present (lower is better), or --metric.

Per variant: runs, median, min..max, the median against the reference's, the median of the
per-round paired differences (same round, so drift between rounds cancels), and "slower n/N":
in how many paired rounds the variant was slower than the reference.

--control: a variant expected to equal the reference (a byte copy of its DLL, or the same build
from another path). Its paired differences are the noise floor: a variant whose paired median
lies within the control's range is marked `~ noise`. A variant whose runs fall into two clusters
is marked `bimodal?`: read its per-run values before the median (some benches switch between two
modes per process).
"""
import argparse, re, statistics, sys

METRICS = ("avg_ms", "ms/frame", "frame_median_ms", "median_ns")


def load(paths, metric):
    vals, skipped, seen, keys_seen = {}, {"dirty": 0, "fail": 0}, set(), set()
    for path in paths:
        for n, line in enumerate(open(path), 1):
            m = re.match(r"(DIRTY )?(?:round|pair|block)=(\d+) v=(\S+)", line)
            if not m:
                continue
            # One sample per (file, round, variant): a second one means two runs were mixed into
            # one file (perf-abba.sh writes a fresh file per call), and pairing would be wrong.
            ident = (path, int(m.group(2)), m.group(3))
            if ident in seen:
                sys.exit(f"{path}:{n}: a second sample for round {ident[1]}, variant {ident[2]}: "
                         "results of different runs mixed in one file")
            seen.add(ident)
            if m.group(1):
                skipped["dirty"] += 1
                continue
            if "RESULT FAIL" in line:
                skipped["fail"] += 1
                continue
            keys_seen.update(re.findall(r"(?:^|\s)([A-Za-z_][\w/]*)=[0-9.]+", line))
            for k in ([metric] if metric else METRICS):
                mv = re.search(r"(?:^|\s)" + re.escape(k) + r"=([0-9.]+)", line)
                if mv:
                    vals.setdefault(m.group(3), {})[ident[:2]] = float(mv.group(1))
                    metric = metric or k
                    break
    return vals, skipped, metric, keys_seen


def bimodal(xs):
    """Two clusters: a gap between sorted values wider than 2.5% of the median, with at least 20%
    of the runs on each side."""
    if len(xs) < 6:
        return False
    s = sorted(xs)
    med = statistics.median(s)
    for i in range(1, len(s)):
        if s[i] - s[i - 1] > 0.025 * med and min(i, len(s) - i) >= 0.2 * len(s):
            return True
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ref")
    ap.add_argument("--control")
    ap.add_argument("--metric")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    vals, skipped, metric, keys_seen = load(a.files, a.metric)
    if not vals:
        avail = " ".join(sorted(k for k in keys_seen if k not in ("round", "pair", "block", "load")))
        sys.exit(f"no results for metric {a.metric or ' / '.join(METRICS)}"
                 f" (skipped {skipped['dirty']} DIRTY, {skipped['fail']} FAIL)"
                 + (f"; keys present: {avail} (--metric)" if avail else ""))
    ref = a.ref or next(iter(vals))
    if ref not in vals:
        sys.exit(f"no results for the reference {ref}")
    base = vals[ref]
    bmed = statistics.median(base.values())

    def paired(v):
        return [vals[v][k] - base[k] for k in vals[v] if k in base]

    noise = None
    if a.control:
        if a.control not in vals:
            sys.exit(f"no results for the control {a.control}")
        c = paired(a.control)
        noise = (min(c), max(c)) if c else None

    print(f"metric {metric} (lower is better), reference {ref}"
          + (f", control {a.control}" if a.control else "")
          + (f"; skipped: {skipped['dirty']} DIRTY, {skipped['fail']} FAIL" if any(skipped.values()) else ""))
    print(f"{'variant':22} {'n':>3} {'median':>10} {'min':>10} {'max':>10} {'vs ref':>8} "
          f"{'paired median':>22} {'slower':>8}")
    for v, d in vals.items():
        xs = list(d.values())
        med = statistics.median(xs)
        flag = " bimodal?" if bimodal(xs) else ""
        if v == ref:
            print(f"{v:22} {len(xs):3} {med:10.4f} {min(xs):10.4f} {max(xs):10.4f} {'[ref]':>8} {'':>22} {'':>8}{flag}")
            continue
        diffs = paired(v)
        if not diffs:
            print(f"{v:22} {len(xs):3} {med:10.4f}  (no rounds paired with the reference)")
            continue
        pd = statistics.median(diffs)
        slower = sum(1 for x in diffs if x > 0)
        tag = ""
        if v == a.control:
            tag = " [control]"
        elif noise and noise[0] <= pd <= noise[1]:
            tag = " ~ noise"
        print(f"{v:22} {len(xs):3} {med:10.4f} {min(xs):10.4f} {max(xs):10.4f} {(med / bmed - 1) * 100:+7.1f}% "
              f"{pd:+11.4f} ({pd / bmed * 100:+5.1f}%) {slower:>4}/{len(diffs):<3}{tag}{flag}")
    if noise:
        print(f"control's paired range: {noise[0]:+.4f} .. {noise[1]:+.4f} ({noise[0] / bmed * 100:+.1f}% .. {noise[1] / bmed * 100:+.1f}%)")


if __name__ == "__main__":
    main()
