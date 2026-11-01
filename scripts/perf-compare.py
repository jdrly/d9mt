#!/usr/bin/env python3
"""Compare rotated-round results descriptively, with symmetric effects and direction counts.

Optional --diagnostic-band is a percentage magnitude, never a merge threshold.
No accepted metric/schedule calibration is wired here: guard status is always pending.
Paired median intervals assume independent exchangeable pairs and describe only the
sampled regime. Correlated rounds in one window do not establish between-window power.
Control ranges and bimodality are diagnostics, not proof of equivalence.
"""
import argparse, math, re, statistics, sys
from pathlib import Path

METRICS = ("avg_ms", "ms/frame", "frame_median_ms", "median_ns")
HIGHER_IS_BETTER = ("fps", "low1_fps", "frame_low1_fps")


def over_band(effect, band):
    return band is not None and effect is not None and abs(effect) > band / 100 + 1e-12


def median_interval(xs):
    """Conservative distribution-free 95% median interval via binomial order statistics."""
    xs = sorted(xs)
    n = len(xs)
    k = 0
    while k < n // 2 and sum(math.comb(n, j) for j in range(k + 1)) / 2**n <= 0.025:
        k += 1
    return (xs[k - 1], xs[n - k]) if k else (None, None)


META = ("client", "visible")
NUM = r"=([^\s]+)(?=\s|$)"   # validate the whole token, including malformed/nonfinite values


def load(paths, metric):
    vals, skipped, seen, keys_seen, clients, visible = {}, {"dirty": 0, "fail": 0, "missing": 0, "invalid": 0}, set(), set(), {}, {}
    for path in paths:
        file_samples = 0
        for n, line in enumerate(open(path), 1):
            m = re.match(r"(DIRTY )?(?:round|pair|block)=(\d+) v=(\S+)", line)
            if not m:
                continue
            file_samples += 1
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
                    try:
                        value = float(mv.group(1))
                    except ValueError:
                        value = math.nan
                    if not math.isfinite(value) or value < 0:
                        skipped["invalid"] += 1
                    else:
                        vals.setdefault(m.group(3), {})[ident[:2]] = value
                    metric = metric or k
                    break
            else:
                skipped["missing"] += 1
        if not file_samples:
            skipped["missing"] += 1
    return vals, skipped, metric, keys_seen, clients, visible


def pct(x, width=0):
    """A signed percentage, or n/a (the reference's median is 0: e.g. sd=0)."""
    s = "n/a" if x is None else f"{x * 100:+.3f}%"
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
    ap.add_argument("--diagnostic-band", type=float, metavar="PERCENT",
                    help="optional symmetric effect magnitude, never an accepted guard")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    if a.diagnostic_band is not None and (not math.isfinite(a.diagnostic_band) or a.diagnostic_band < 0):
        ap.error("--diagnostic-band must be finite and nonnegative")
    if a.metric in META:
        sys.exit(f"--metric {a.metric}: that's run metadata, not a measurement")
    a.files = [str(Path(p).resolve()) for p in a.files]
    if len(set(a.files)) != len(a.files):
        ap.error("duplicate result file")
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
    if len(vals) < 2:
        sys.exit("no comparison: need results for at least two variants")
    base = vals[ref]
    bmed = statistics.median(base.values())

    def paired(v):
        return [vals[v][k] - base[k] for k in vals[v] if k in base]

    noise = None
    if a.control:
        if a.control == ref:
            sys.exit("control must differ from reference")
        if a.control not in vals:
            sys.exit(f"no results for the control {a.control}")
        c = paired(a.control)
        if not c:
            sys.exit("control has no paired results")
        noise = (min(c), max(c))

    sign = -1 if metric in HIGHER_IS_BETTER else 1   # +: a larger value is slower
    print(f"metric {metric} ({'higher' if sign < 0 else 'lower'} is better), reference {ref}"
          + (f", control {a.control}" if a.control else "")
          + (f"; skipped: {skipped}" if any(skipped.values()) else ""))
    print("guard: pending; no accepted metric/schedule/condition calibration is wired")
    print("uncertainty: conditional 95% median interval, independent exchangeable pairs assumed; "
          "no between-window calibration or detection-power claim")
    print("variant                 n     median        min        max   vs ref")
    incomplete = any(skipped.values())
    for v, d in vals.items():
        xs = list(d.values())
        med = statistics.median(xs)
        flag = " bimodal?" if bimodal(xs) else ""
        print(f"{v:22} {len(xs):3} {med:10.4f} {min(xs):10.4f} {max(xs):10.4f} "
              f"{('[ref]' if v == ref else pct(med / bmed - 1 if bmed else None)):>8}{flag}")
        if v == ref:
            continue
        diffs = paired(v)
        if not diffs:
            print("  no rounds paired with the reference; invalid comparison")
            incomplete = True
            continue
        if set(d) != set(base):
            print("  incomplete pairing; descriptive results only")
            incomplete = True
        pd = statistics.median(diffs)
        rel = pd / bmed if bmed else None
        slower = sum(sign * x > 0 for x in diffs)
        faster = sum(sign * x < 0 for x in diffs)
        equal = len(diffs) - slower - faster
        lo, hi = median_interval(diffs)
        interval = "unbounded" if lo is None else f"{lo:+.4f} .. {hi:+.4f}"
        relative_interval = "unbounded" if lo is None or not bmed else f"{pct(lo / bmed)} .. {pct(hi / bmed)}"
        print(f"  paired {pd:+.4f} ({pct(rel)}); magnitude {abs(pd):.4f} "
              f"({pct(abs(rel)) if rel is not None else 'n/a'}); "
              f"faster {faster}/{len(diffs)}, slower {slower}/{len(diffs)}, equal {equal}/{len(diffs)}")
        print(f"  conditional median interval: {interval}; relative {relative_interval}")
        tag = " [control]" if v == a.control else ""
        if noise and noise[0] <= pd <= noise[1] and v != a.control:
            tag += " inside observed control range; equivalence unverified"
        if over_band(rel, a.diagnostic_band):
            direction = 'slower' if sign * pd > 0 else 'faster'
            tag += f" DIAGNOSTIC {direction} >{a.diagnostic_band:g}%"
        if tag:
            print(" " + tag)
    if noise:
        print(f"control's observed paired range: {noise[0]:+.4f} .. {noise[1]:+.4f}; diagnostic only")
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
            if x is None or x.startswith("?"):
                return "unknown"
            if x.startswith("pre:") and x.endswith("1"):
                return "settled before timing"
            return {"0": "occluded", "nolog": "no change logged"}.get(x, "changed")
        def vis(v):
            ks = [kind(visible.get(v, {}).get(k)) for k in vals[v]]
            return f"{v} " + ", ".join(f"{ks.count(w)} {w}" for w in ("occluded", "no change logged", "settled before timing",
                                                                        "changed", "unknown") if w in ks)
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

    return 1 if incomplete else 0


if __name__ == "__main__":
    sys.exit(main())
