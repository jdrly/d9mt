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

Run metadata, never a metric: client=<W>x<H> (the bench's window client area, perf-abba.sh) is
listed per variant (unknown for runs without it), with a warning when it isn't the back buffer's
size (client=<W>x<H>!=<back buffer>: a visible window's presents need the presenter's scaling
blit) or when a paired round compares different client areas. visible= (0: occluded throughout,
nolog: no change logged, visible throughout if the bench presented; 0>1 etc.: changed within the
run; ?: not established) is counted per variant, with a warning about runs that changed or are
unknown, and when occluded runs are mixed with others (an occluded window's presents are
skipped, so the two measure different things).
"""
import argparse, re, statistics, sys

METRICS = ("avg_ms", "ms/frame", "frame_median_ms", "median_ns")
META = ("client", "visible")
NUM = r"=([0-9.]+)(?=\s|$)"   # the whole value token is a number (not 320x240)


def load(paths, metric):
    vals, skipped, seen, keys_seen, clients, visible = {}, {"dirty": 0, "fail": 0}, set(), set(), {}, {}
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
            keys_seen.update(re.findall(r"(?:^|\s)([A-Za-z_][\w/]*)=[0-9.]+(?=\s|$)", line))
            mc = re.search(r"(?:^|\s)client=(\S+)", line)
            if mc:
                clients.setdefault(m.group(3), {})[ident[:2]] = mc.group(1)
            mv = re.search(r"(?:^|\s)visible=(\S+)", line)
            if mv:
                visible.setdefault(m.group(3), {})[ident[:2]] = mv.group(1)
            for k in ([metric] if metric else METRICS):
                mv = re.search(r"(?:^|\s)" + re.escape(k) + NUM, line)
                if mv:
                    vals.setdefault(m.group(3), {})[ident[:2]] = float(mv.group(1))
                    metric = metric or k
                    break
    return vals, skipped, metric, keys_seen, clients, visible


def pct(x, width=0):
    """A signed percentage, or n/a (the reference's median is 0: e.g. sd=0)."""
    s = "n/a" if x is None else f"{x * 100:+.1f}%"
    return f"{s:>{width}}" if width else s


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
    if a.metric in META:
        sys.exit(f"--metric {a.metric}: that's run metadata, not a measurement")
    vals, skipped, metric, keys_seen, clients, visible = load(a.files, a.metric)
    if not vals:
        avail = " ".join(sorted(k for k in keys_seen if k not in ("round", "pair", "block", "load") + META))
        sys.exit(f"no results for metric {a.metric or ' / '.join(METRICS)}"
                 f" (skipped {skipped['dirty']} DIRTY, {skipped['fail']} FAIL)"
                 + (f"; keys present: {avail} (--metric)" if avail else ""))
    # The metadata of the runs in the table only (a run without the metric isn't in it).
    clients, visible = ({v: {k: x for k, x in d.items() if k in vals.get(v, {})} for v, d in md.items()}
                        for md in (clients, visible))
    clients = {v: d for v, d in clients.items() if d}
    visible = {v: d for v, d in visible.items() if d}
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
        print(f"{v:22} {len(xs):3} {med:10.4f} {min(xs):10.4f} {max(xs):10.4f} {pct(med / bmed - 1 if bmed else None):>8} "
              f"{pd:+11.4f} ({pct(pd / bmed if bmed else None, 5)}) {slower:>4}/{len(diffs):<3}{tag}{flag}")
    if noise:
        rel = f" ({pct(noise[0] / bmed)} .. {pct(noise[1] / bmed)})" if bmed else ""
        print(f"control's paired range: {noise[0]:+.4f} .. {noise[1]:+.4f}{rel}")
    if clients:
        def sizes(v):
            c = clients.get(v, {})
            return sorted(set(c.values())) + (["unknown"] if any(k not in c for k in vals[v]) else [])
        print("client area: " + ", ".join(f"{v} {'/'.join(sizes(v))}" for v in vals))
        bad = [v for v in vals if any("!=" in x for x in clients.get(v, {}).values())]
        if bad:
            print(f"WARNING: the client area isn't the back buffer's size ({', '.join(bad)}): a visible"
                  " window's presents need the scaling blit, an extra PSO and GPU pass (test/perf/bench_window.h)")
        cref = clients.get(ref, {})
        unpaired = {v: sum(1 for k, x in clients.get(v, {}).items() if k in cref and cref[k] != x)
                    for v in vals if v != ref}
        unpaired = {v: n for v, n in unpaired.items() if n}
        if unpaired:
            print("WARNING: paired rounds compare different client areas: "
                  + ", ".join(f"{v} {n} round(s)" for v, n in unpaired.items()))
    if visible:
        def kind(x):
            return {"0": "occluded", "nolog": "no change logged", None: "unknown", "?": "unknown"}.get(x, "changed")
        def vis(v):
            ks = [kind(visible.get(v, {}).get(k)) for k in vals[v]]
            return f"{v} " + ", ".join(f"{ks.count(w)} {w}" for w in ("occluded", "no change logged", "changed", "unknown") if w in ks)
        print("window visibility: " + "; ".join(vis(v) for v in vals))
        kinds = {kind(visible.get(v, {}).get(k)) for v in vals for k in vals[v]}
        odd = sorted({x for v in vals for k in vals[v] for x in [visible.get(v, {}).get(k, "?")]
                      if kind(x) in ("changed", "unknown")})
        if odd:
            print(f"WARNING: runs whose window changed visibility or whose visibility is unknown ({', '.join(odd)}):"
                  " they don't measure one present path")
        if "occluded" in kinds and len(kinds) > 1:
            print("WARNING: occluded runs are mixed with others: an occluded window's presents are skipped,"
                  " so compare them separately")


if __name__ == "__main__":
    main()
