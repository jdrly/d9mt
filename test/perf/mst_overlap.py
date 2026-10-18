#!/usr/bin/env python3
"""Vertex/fragment overlap in a Metal System Trace (docs/gpu-4k.md, candidate 1).

Usage: python3 -I test/perf/mst_overlap.py <trace>

Per frame: vertex and fragment busy time, their overlap and union. At each
render-encoder boundary: whether the next pass's vertex work starts before the
previous pass's fragment work ends. A producer barrier after Fragment before
all stages (d9mt's end-of-pass barrier at 4b7ab26) shows 0% there.
Frames are Metal frame numbers; without a drawable (gpu4k_probe) one command
buffer counts as a frame. Picks the busiest wine/swg/swtor process, else the
busiest one. Stdlib only; the XML helpers come from the issue #7 analysis
script (analyze.py).
"""
import collections, re, subprocess, sys
import xml.etree.ElementTree as ET


# ---------------------------------------------------------------- xml access
class N:
    __slots__ = ("tag", "text", "fmt", "kids")

    def __init__(s, tag, text, fmt, kids):
        s.tag, s.text, s.fmt, s.kids = tag, text, fmt, kids

    def find(s, tag):
        for k in s.kids:
            if k is None:
                continue
            if k.tag == tag:
                return k
            r = k.find(tag)
            if r is not None:
                return r
        return None


def rows(trace, run, schema):
    """Yield {mnemonic: N} per row; empty if the table is missing."""
    xp = '/trace-toc/run[@number="%d"]/data/table[@schema="%s"]' % (run, schema)
    p = subprocess.Popen(["xcrun", "xctrace", "export", "--input", trace, "--xpath", xp],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    ids, cols, stack = {}, [], []
    try:
        for ev, el in ET.iterparse(p.stdout, events=("start", "end")):
            if ev == "start":
                stack.append([])
                continue
            kids = stack.pop()
            t = el.tag
            if t == "mnemonic":
                cols.append(el.text)
            ref = el.get("ref")
            if ref is not None:
                n = ids.get(ref)
            else:
                n = N(t, el.text, el.get("fmt"), kids)
                i = el.get("id")
                if i is not None:
                    ids[i] = n
            if t == "row":
                yield dict(zip(cols, kids))
                el.clear()
            elif t == "node":
                el.clear()
            elif stack:
                stack[-1].append(n)
    except ET.ParseError:
        pass
    finally:
        p.stdout.close()
        p.wait()


def txt(r, k):
    n = r.get(k)
    return None if n is None else n.text


def fmt(r, k):
    n = r.get(k)
    return None if n is None else n.fmt


def num(r, k, default=None):
    t = txt(r, k)
    try:
        return int(t)
    except (TypeError, ValueError):
        try:
            return float(t)
        except (TypeError, ValueError):
            return default


def pid_of(r, k="process"):
    n = r.get(k)
    if n is None:
        return None
    p = n.find("pid")
    if p is not None and p.text:
        return p.text
    m = re.search(r"\((\d+)\)", n.fmt or "")
    return m.group(1) if m else None


# ---------------------------------------------------------------- helpers
def union(iv):
    """Merge (start, end) intervals; return merged list."""
    out = []
    for s, e in sorted(iv):
        if out and s <= out[-1][1]:
            if e > out[-1][1]:
                out[-1][1] = e
        else:
            out.append([s, e])
    return out


def ulen(iv):
    return sum(e - s for s, e in union(iv))


def overlap(segs, ivs):
    """Total overlap between two sorted merged interval lists."""
    i = j = 0
    tot = 0
    while i < len(segs) and j < len(ivs):
        s = max(segs[i][0], ivs[j][0])
        e = min(segs[i][1], ivs[j][1])
        if e > s:
            tot += e - s
        if segs[i][1] < ivs[j][1]:
            i += 1
        else:
            j += 1
    return tot


def pct(xs, q):
    xs = sorted(xs)
    if not xs:
        return float("nan")
    return xs[min(len(xs) - 1, int(round(q * (len(xs) - 1))))]


def stat(xs, unit="", f="%.2f"):
    if not xs:
        return "n/a"
    return ("med " + f + "  p90 " + f + "  max " + f + "%s  (n=%d)") % (
        pct(xs, .5), pct(xs, .9), max(xs), unit, len(xs))


MS = 1e6


# ---------------------------------------------------------------- main

trace = sys.argv[1]
gi, per_pid, names = [], collections.Counter(), {}
for r in rows(trace, 1, "metal-gpu-intervals"):
    p = pid_of(r); st, du = num(r, "start"), num(r, "duration")
    if p is None or st is None or not du: continue
    names[p] = fmt(r, "process") or p; per_pid[p] += du
    gi.append((p, st, st + du, fmt(r, "channel-name") or "?", num(r, "frame-number"), txt(r, "cmdbuffer-id"), txt(r, "encoder-id")))
cand = [p for p in per_pid if re.search(r"wine|swg|swtor", names[p], re.I)] or list(per_pid)
pid = max(cand, key=lambda p: per_pid[p])
label = {txt(r, "object-id"): fmt(r, "label") or "" for r in rows(trace, 1, "metal-object-label") if txt(r, "pid") == pid}
g = [x for x in gi if x[0] == pid and not label.get(x[5], "").startswith("libMTLHud")]
chans = collections.Counter(x[3] for x in g)
print("channels:", dict(chans))
fr = collections.defaultdict(lambda: collections.defaultdict(list))
enc = collections.defaultdict(lambda: collections.defaultdict(list))
encframe = {}
for _, s, e, ch, f, cb, en in g:
    if f is None: f = cb  # no drawable (synthetic probe): one command buffer = one frame
    fr[f][ch].append((s, e)); enc[en][ch].append((s, e)); encframe[en] = f
keys = sorted(fr, key=lambda k: min(s for v in fr[k].values() for s, _ in v))[1:-1]
vb, fb, ov, un, sm = [], [], [], [], []
for k in keys:
    v = union(fr[k].get("Vertex", [])); f_ = union(fr[k].get("Fragment", []))
    a, b = sum(e - s for s, e in v), sum(e - s for s, e in f_)
    o = overlap(v, f_)
    vb.append(a / MS); fb.append(b / MS); ov.append(o / MS); un.append((a + b - o) / MS); sm.append((a + b) / MS)
print("vertex busy   ", stat(vb, " ms")); print("fragment busy ", stat(fb, " ms"))
print("V∩F overlap   ", stat(ov, " ms")); print("V∪F           ", stat(un, " ms")); print("V+F           ", stat(sm, " ms"))
# encoder boundaries: for consecutive render encoders (by first start), does next vertex start before prev fragment ends?
byf = collections.defaultdict(list)
for en, d in enc.items():
    if "Vertex" in d or "Fragment" in d:
        s0 = min(s for ivs in d.values() for s, _ in ivs)
        byf[encframe[en]].append((s0, en))
gaps, ovl, n_ov, n_tot = [], [], 0, 0
for k in keys:
    lst = [en for _, en in sorted(byf[k])]
    for a, b in zip(lst, lst[1:]):
        fa = enc[a].get("Fragment"); vb_ = enc[b].get("Vertex")
        if not fa or not vb_: continue
        fend = max(e for _, e in fa); vstart = min(s for s, _ in vb_)
        n_tot += 1
        if vstart < fend: n_ov += 1; ovl.append((fend - vstart) / 1e3)
        else: gaps.append((vstart - fend) / 1e3)
print("render-encoder boundaries: %d, next pass's vertex starts before prev fragment ends: %d (%.0f%%)" % (n_tot, n_ov, 100 * n_ov / max(n_tot, 1)))
print("  overlap when overlapping:", stat(ovl, " us")); print("  gap prev-frag-end -> next-vertex-start:", stat(gaps, " us"))
