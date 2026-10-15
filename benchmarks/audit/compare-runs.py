#!/usr/bin/env python3
"""Compare two performance-audit runs: what got faster, slower, bigger.

The audit's numbers are comparable only with other numbers from the same box
(performance-audit-plan, decision 1), so a fix's payoff is a before/after on
the WP0 box. This takes two run-corpus.py results -- each either a sweep's
rows.csv or, better, merge-sweeps.py's merged rows.csv -- and reports, per
(program, engine, phase):

  before / after   best wall time, ms
  ratio            after / before (below 1 is faster)
  noise            the larger of the two rows' noise: merge-sweeps' `spread`
                   (how far the second-best sweep's best is from the best), or
                   run-corpus's `wall_cv` for a single sweep
  verdict          faster / slower when |ratio - 1| exceeds both --threshold
                   (default 5%; doubled for a row under 50 ms, where an A/A run
                   on the WP0 box moved a 17 ms `emit` by 5.8%) and the noise;
                   `same` inside it; `noisy` when either row is marked noisy;
                   `floor` when both runs are under --floor-ms (default 5 ms),
                   where the timer, not the program, decides

It also lists every change in `bytes` (emitted C for `emit`, the binary for
`build`) and every peak-RSS change over 10% and 1 MiB, and a geometric-mean
ratio per engine and phase over the rows that are neither noisy nor at the
floor. Informational: the exit status is 0 unless an input is unreadable.

Usage:
  python3 benchmarks/audit/compare-runs.py BEFORE AFTER [--threshold 0.05]
      [--floor-ms 5] [--all] [--csv OUT.csv]

BEFORE and AFTER are a rows.csv or a directory holding one; the WP0 baseline
is docs/artifacts/perf-audit-2026-10/wp0-baseline.csv. Writes markdown to
stdout. benchmarks/audit/remeasure.sh runs the sweeps and calls this.
"""

import argparse
import csv
import math
import os
import sys
from collections import defaultdict


def load(path):
    if os.path.isdir(path):
        path = os.path.join(path, "rows.csv")
    rows = {}
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            rows[(r["program"], r["engine"], r["phase"])] = r
    return rows


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def noise(r):
    """A row's noise as a fraction, and whether it is marked noisy."""
    if "spread" in r:            # merged: spread between the two best sweeps
        return num(r.get("spread")) or 0.0, bool(r.get("noisy"))
    cv = num(r.get("wall_cv"))   # a single sweep: the reps' CV
    return cv or 0.0, bool(cv is not None and cv > 0.10)


def short(program):
    base = os.path.basename(program)
    stem = base[:-4] if base.endswith(".tur") else base
    if stem in ("input", "main"):
        parent = os.path.basename(os.path.dirname(program))
        stem = os.path.basename(os.path.dirname(os.path.dirname(program))) if parent == "src" else parent
    return stem


def fmt_ms(v):
    if v is None:
        return "-"
    return f"{v:.1f}" if v < 1000 else f"{v / 1000:.2f} s"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--threshold", type=float, default=0.05)
    ap.add_argument("--floor-ms", type=float, default=5.0)
    ap.add_argument("--all", action="store_true", help="list every row, not just the changes")
    ap.add_argument("--csv", default="")
    args = ap.parse_args()

    before, after = load(args.before), load(args.after)
    keys = sorted(set(before) & set(after))
    only_b = sorted(set(before) - set(after))
    only_a = sorted(set(after) - set(before))

    out = []
    for k in keys:
        b, a = before[k], after[k]
        row = {"program": k[0], "engine": k[1], "phase": k[2],
               "status_before": b["status"], "status_after": a["status"]}
        tb, ta = num(b.get("wall_ms_min")), num(a.get("wall_ms_min"))
        nb, noisy_b = noise(b)
        na, noisy_a = noise(a)
        row.update(before_ms=tb, after_ms=ta, noise=max(nb, na))
        if b["status"] != "ok" or a["status"] != "ok" or not tb or not ta:
            row.update(ratio=None, verdict=f"status {b['status']} -> {a['status']}")
        else:
            ratio = ta / tb
            row["ratio"] = ratio
            if max(tb, ta) < args.floor_ms:
                row["verdict"] = "floor"
            elif noisy_b or noisy_a:
                row["verdict"] = "noisy"
            elif abs(ratio - 1) > max(args.threshold * (2 if max(tb, ta) < 50 else 1),
                                      row["noise"]):
                row["verdict"] = "faster" if ratio < 1 else "slower"
            else:
                row["verdict"] = "same"
        row["bytes_before"], row["bytes_after"] = b.get("bytes", ""), a.get("bytes", "")
        row["rss_before"], row["rss_after"] = num(b.get("maxrss_kib")), num(a.get("maxrss_kib"))
        out.append(row)

    counts = defaultdict(int)
    for r in out:
        counts[r["verdict"] if not r["verdict"].startswith("status") else "status changed"] += 1

    print("## Before / after")
    print()
    print(f"before: `{args.before}`  ")
    print(f"after: `{args.after}`  ")
    print(f"rows compared: {len(out)}; only before: {len(only_b)}; only after: {len(only_a)}. "
          f"Verdicts: " + ", ".join(f"{v} {n}" for v, n in sorted(counts.items())) + ".")
    print(f"A row is faster or slower only when it moved more than {args.threshold:.0%} "
          f"({2 * args.threshold:.0%} under 50 ms) and more than its own noise; "
          f"under {args.floor_ms:g} ms it is left to the timer.")
    print()

    print("### Geometric mean ratio (after / before), rows neither noisy nor at the floor")
    print()
    print("| engine | phase | rows | geomean | faster | slower |")
    print("| --- | --- | --: | --: | --: | --: |")
    groups = defaultdict(list)
    for r in out:
        if r["ratio"] and r["verdict"] in ("faster", "slower", "same"):
            groups[(r["engine"], r["phase"])].append(r)
    for (eng, ph), rs in sorted(groups.items()):
        g = math.exp(sum(math.log(r["ratio"]) for r in rs) / len(rs))
        print(f"| {eng} | {ph} | {len(rs)} | {g:.3f} | "
              f"{sum(r['verdict'] == 'faster' for r in rs)} | {sum(r['verdict'] == 'slower' for r in rs)} |")
    print()

    shown = [r for r in out if args.all or r["verdict"] in ("faster", "slower")
             or r["verdict"].startswith("status")]
    print("### Rows that moved" if not args.all else "### Every row")
    print()
    if not shown:
        print("Nothing moved beyond the threshold and its noise.")
    else:
        print("| program | engine | phase | before ms | after ms | ratio | noise | verdict |")
        print("| --- | --- | --- | --: | --: | --: | --: | --- |")
        for r in sorted(shown, key=lambda r: (r["ratio"] or 9e9)):
            ratio = f"{r['ratio']:.3f}" if r["ratio"] else "-"
            print(f"| `{short(r['program'])}` | {r['engine']} | {r['phase']} | {fmt_ms(r['before_ms'])} | "
                  f"{fmt_ms(r['after_ms'])} | {ratio} | {r['noise']:.1%} | {r['verdict']} |")
    print()

    sized = [r for r in out if r["bytes_before"] and r["bytes_after"]
             and r["bytes_before"] != r["bytes_after"]]
    if sized:
        print("### Size changes (`emit`: C bytes, `build`: binary bytes)")
        print()
        print("| program | engine | phase | before | after | change |")
        print("| --- | --- | --- | --: | --: | --: |")
        for r in sorted(sized, key=lambda r: r["program"]):
            bb, ba = num(r["bytes_before"]), num(r["bytes_after"])
            ch = f"{(ba - bb) / bb:+.1%}" if bb else "-"
            print(f"| `{short(r['program'])}` | {r['engine']} | {r['phase']} | {r['bytes_before']} | {r['bytes_after']} | {ch} |")
        print()

    rss = [r for r in out if r["rss_before"] and r["rss_after"]
           and abs(r["rss_after"] - r["rss_before"]) > 1024
           and abs(r["rss_after"] / r["rss_before"] - 1) > 0.10]
    if rss:
        print("### Peak RSS changes over 10% and 1 MiB")
        print()
        print("| program | engine | phase | before KiB | after KiB | change |")
        print("| --- | --- | --- | --: | --: | --: |")
        for r in sorted(rss, key=lambda r: r["rss_after"] / r["rss_before"]):
            print(f"| `{short(r['program'])}` | {r['engine']} | {r['phase']} | {int(r['rss_before'])} | "
                  f"{int(r['rss_after'])} | {r['rss_after'] / r['rss_before'] - 1:+.0%} |")
        print()

    if only_b or only_a:
        print(f"Rows in one run only -- before: {', '.join(short(k[0]) + '/' + k[1] + '/' + k[2] for k in only_b[:8]) or 'none'}; "
              f"after: {', '.join(short(k[0]) + '/' + k[1] + '/' + k[2] for k in only_a[:8]) or 'none'}.")

    if args.csv:
        with open(args.csv, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(out[0]) if out else ["program"], lineterminator="\n")
            w.writeheader()
            w.writerows(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
