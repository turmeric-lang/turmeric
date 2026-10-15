#!/usr/bin/env python3
"""Merge several run-corpus.py sweeps into one table, and say which rows are noisy.

One sweep's best-of-N cannot tell a real number from a few seconds of host
interference: in the WP0 sweeps a burst of several seconds slowed even
sub-millisecond programs 10-200x, at a different point in each sweep
(triangle/mandel's cc run best-of-5 at 68 ms against a steady 3.4 ms;
datalog's at 150 ms against 0.6 ms). Two or more sweeps taken under the same
conditions can: each merged row is the best across sweeps, and `spread` is
how far the next-best sweep's best is from it. A row whose spread is over
--noisy (default 10%, the reproducibility rule of
performance-comparison/scripts/check_reproducibility.py and the plan's WP6)
is marked, not dropped: its number was not reproduced.

Usage:
  python3 benchmarks/audit/merge-sweeps.py OUT_DIR SWEEP_DIR [SWEEP_DIR ...]

Each SWEEP_DIR holds a run-corpus.py rows.csv and meta.json; they must share
a commit and a tur.  Writes OUT_DIR/rows.csv (one merged row per program,
engine and phase) and OUT_DIR/summary.md.
"""

import argparse
import csv
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out")
    ap.add_argument("sweeps", nargs="+")
    ap.add_argument("--noisy", type=float, default=0.10)
    args = ap.parse_args()

    metas = [json.load(open(os.path.join(d, "meta.json"))) for d in args.sweeps]
    for key in ("commit", "tur_version", "cpu", "cc"):
        vals = {m[key] for m in metas}
        if len(vals) != 1:
            sys.exit(f"merge-sweeps: sweeps disagree on {key}: {sorted(vals)}")

    sweeps = []
    for d in args.sweeps:
        rows = {}
        for r in csv.DictReader(open(os.path.join(d, "rows.csv"))):
            rows[(r["program"], r["engine"], r["phase"])] = r
        sweeps.append(rows)
    keys = list(sweeps[0])
    for s in sweeps[1:]:
        if set(s) != set(keys):
            sys.exit("merge-sweeps: sweeps cover different (program, engine, phase) rows")

    merged = []
    for k in keys:
        rs = [s[k] for s in sweeps]
        bad = [r["status"] for r in rs if r["status"] != "ok"]
        mins = [float(r["wall_ms_min"]) for r in rs]
        best = rs[mins.index(min(mins))]
        # Spread is between the two LOWEST sweep bests: "is the reported
        # number reproduced by another sweep?"  One sweep hit by interference
        # while two agree is not noise in the result.
        lo = sorted(mins)
        spread = (lo[1] - lo[0]) / lo[0] if len(lo) > 1 and lo[0] > 0 else 0.0
        sizes = {r["bytes"] for r in rs}
        merged.append({
            "program": k[0], "engine": k[1], "phase": k[2],
            "status": bad[0] if bad else "ok",
            "sweeps": len(rs),
            "wall_ms_min": round(min(mins), 2),
            "sweep_mins": " ".join(f"{m:g}" for m in mins),
            "spread": round(spread, 3),
            "noisy": "noisy" if spread > args.noisy else "",
            "user_ms": best["user_ms"], "sys_ms": best["sys_ms"],
            "maxrss_kib": max(int(r["maxrss_kib"]) for r in rs),
            "bytes": rs[0]["bytes"] if len(sizes) == 1 else "differs:" + "/".join(sorted(sizes)),
        })

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "rows.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(merged[0]))
        w.writeheader()
        w.writerows(merged)
    write_summary(os.path.join(args.out, "summary.md"), metas, merged, args.noisy)
    noisy = [r for r in merged if r["noisy"]]
    print(f"merge-sweeps: {len(merged)} rows from {len(sweeps)} sweeps, "
          f"{len(noisy)} noisy (> {args.noisy:.0%} apart)")
    for r in noisy:
        print(f"  {r['spread']:6.1%}  {r['engine']:6} {r['phase']:5} {r['sweep_mins']:>22}  {r['program']}")


def write_summary(path, metas, rows, noisy):
    by = {}
    for r in rows:
        by.setdefault(r["program"], {})[(r["engine"], r["phase"])] = r

    def cell(r, key="wall_ms_min"):
        if r is None:
            return ""
        if r["status"] != "ok":
            return r["status"]
        return f"{r[key]}{' *' if key == 'wall_ms_min' and r['noisy'] else ''}"

    m = metas[0]
    with open(path, "w") as f:
        f.write(f"commit `{m['commit'][:12]}`, {m['tur_version']}, {m['cpu']}, nproc {m['nproc']}, "
                f"{m['cc']}; {len(metas)} sweeps x best of {m['reps']} "
                f"(after {m.get('warmup', 0)} warm-up), {', '.join(x['date_utc'] for x in metas)}. "
                f"`*` = not reproduced: no other sweep's best is within {noisy:.0%}.\n\n")
        f.write("| program | emit-c ms | C bytes | cc build ms | binary bytes | cc run ms "
                "| run RSS KiB | jit ms | interp ms |\n")
        f.write("|---|---:|---:|---:|---:|---:|---:|---:|---:|\n")
        for prog, ph in by.items():
            e, b, c = ph.get(("cc", "emit")), ph.get(("cc", "build")), ph.get(("cc", "run"))
            f.write(f"| {prog} | {cell(e)} | {e['bytes'] if e else ''} | {cell(b)} | "
                    f"{b['bytes'] if b else ''} | {cell(c)} | {cell(c, 'maxrss_kib')} | "
                    f"{cell(ph.get(('jit', 'run')))} | {cell(ph.get(('interp', 'run')))} |\n")


if __name__ == "__main__":
    sys.exit(main())
