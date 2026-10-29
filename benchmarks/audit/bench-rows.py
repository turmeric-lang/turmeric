#!/usr/bin/env python3
"""Turn a run-corpus.py result into nightly benchmark rows, and check them
against their own history (performance-audit-plan WP6).

Reads <dir>/rows.csv and <dir>/meta.json (run-corpus.py --corpus
benchmarks/audit/tracked.txt) and writes one JSONL row per program, engine and
metric:

    {"kind": "bench", "suite": "bench:mut-map-reassign-1m/jit/run_ms",
     "program": "benchmarks/audit/sized/mut-map-reassign-1m.tur",
     "engine": "jit", "metric": "run_ms", "value": 1771.2, "unit": "ms",
     "cv": 0.012, "noisy": false, "reps": 3, "status": "ok",
     "os": "macOS", "cc": "...", "nproc": 3, "sha": ..., "ts": ...}

Metrics: run_ms (every engine), and for cc also emit_ms (`tur emit-c`, the
front end), build_ms (`tur build`, the front end plus cc) and binary_bytes.
Times are the best of the run's reps; `cv` is check_reproducibility.py's
coefficient of variation over those reps, and `noisy` marks a row above 10%.

Tripwires, not gates: with --history (earlier rows, e.g. the nightly's
`/api/ci/bench?format=ndjson&days=60`), a row more than 1.5x the trailing median of its
own last nights on the same runner shape prints a `::warning::` -- unless the
row is noisy, which prints a `::notice::` instead. Fewer than three earlier
nights: no bound yet. The exit status is always 0.

Usage:
    python3 benchmarks/audit/bench-rows.py <run-corpus out dir> --out bench.jsonl \
        [--history bench-2026.jsonl] [--window 14] [--bound 1.5]
"""

import argparse
import csv
import json
import os
import platform
import statistics
import sys
import time

NOISY_CV = 0.10


def short_name(program):
    base = os.path.basename(program)
    stem = base[:-4] if base.endswith(".tur") else base
    if stem in ("input", "main"):   # a fixture's input.tur, an example's main.tur
        stem = os.path.basename(os.path.dirname(program))
        if stem == "src":
            stem = os.path.basename(os.path.dirname(os.path.dirname(program)))
    return stem


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def runner_os():
    name = os.environ.get("RUNNER_OS")
    if name:
        return name
    return {"Darwin": "macOS"}.get(platform.system(), platform.system())


def make_rows(rows, meta):
    base = {
        "kind": "bench",
        "sha": os.environ.get("GITHUB_SHA") or meta.get("commit"),
        "branch": os.environ.get("GITHUB_REF_NAME"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "run_attempt": int(os.environ.get("GITHUB_RUN_ATTEMPT") or 0) or None,
        "ts": int(time.time()),
        "os": runner_os(),
        "cc": meta.get("cc"),
        "nproc": meta.get("nproc"),
        "build_type": "Release",
        "tur_version": meta.get("tur_version"),
    }
    out = []
    for r in rows:
        phase, engine = r["phase"], r["engine"]
        metrics = []
        if phase == "run":
            metrics.append(("run_ms", num(r["wall_ms_min"]), "ms"))
        elif phase == "emit":
            metrics.append(("emit_ms", num(r["wall_ms_min"]), "ms"))
        elif phase == "build":
            metrics.append(("build_ms", num(r["wall_ms_min"]), "ms"))
            metrics.append(("binary_bytes", num(r.get("bytes")), "bytes"))
        for metric, value, unit in metrics:
            cv = num(r.get("wall_cv")) if unit == "ms" else None
            ok = r["status"] == "ok" and value is not None
            row = dict(base)
            row.update({
                "suite": f"bench:{short_name(r['program'])}/{engine}/{metric}",
                "program": r["program"],
                "engine": engine,
                "metric": metric,
                "value": value if ok else None,
                "unit": unit,
                "cv": cv,
                "noisy": bool(cv is not None and cv > NOISY_CV),
                "reps": int(r["reps"]) if (r.get("reps") or "").isdigit() else None,
                "status": "ok" if ok else (r["status"] or "missing"),
            })
            out.append(row)
    return out


def load_history(paths):
    hist = []
    for p in paths or []:
        if not os.path.isfile(p):
            print(f"bench-rows: no history at {p}; no tripwire bounds this run", file=sys.stderr)
            continue
        for line in open(p):
            try:
                h = json.loads(line)
            except ValueError:
                continue   # a torn line, as the dashboard drops it
            if h.get("kind") == "bench" and h.get("status") == "ok" and h.get("value") is not None:
                hist.append(h)
    return hist


def check(rows, hist, window, bound):
    """(row, trailing median or None, nights) per row, and the annotations."""
    by = {}
    for h in hist:
        by.setdefault((h["suite"], h.get("os"), h.get("nproc")), []).append(h)
    results, notes = [], []
    for r in rows:
        past = sorted(by.get((r["suite"], r["os"], r["nproc"]), []), key=lambda h: h["ts"])[-window:]
        med = statistics.median(h["value"] for h in past) if len(past) >= 3 else None
        results.append((r, med, len(past)))
        if r["status"] != "ok":
            notes.append(f"::warning title=Benchmark did not run::{r['suite']}: {r['status']}")
            continue
        if med is None or med <= 0:
            continue
        ratio = r["value"] / med
        if ratio > bound:
            what = (f"{r['suite']}: {fmt(r['value'], r['unit'])} is {ratio:.2f}x the trailing "
                    f"median {fmt(med, r['unit'])} of {len(past)} nights (bound {bound}x)")
            if r["noisy"]:
                notes.append(f"::notice title=Benchmark over its tripwire, but noisy::{what}; "
                             f"CV {r['cv']:.0%} across reps")
            else:
                notes.append(f"::warning title=Benchmark tripwire::{what}")
    return results, notes


def fmt(v, unit):
    if v is None:
        return "-"
    if unit == "bytes":
        return f"{v / 1e3:.1f} KB" if v < 1e6 else f"{v / 1e6:.2f} MB"
    return f"{v:.1f} ms" if v < 1000 else f"{v / 1000:.2f} s"


def summary(results, bound):
    lines = ["### Nightly benchmarks (performance-audit WP6)", "",
             "| series | tonight | trailing median | ratio | CV | |",
             "|---|--:|--:|--:|--:|---|"]
    for r, med, n in results:
        ratio = (r["value"] / med) if (med and r["value"] is not None) else None
        flag = ""
        if r["status"] != "ok":
            flag = r["status"]
        elif ratio is not None and ratio > bound:
            flag = "noisy, over bound" if r["noisy"] else f"over {bound}x"
        elif r["noisy"]:
            flag = "noisy"
        cv = f"{r['cv']:.1%}" if r["cv"] is not None else ""
        lines.append(f"| `{r['suite']}` | {fmt(r['value'], r['unit'])} | "
                     f"{fmt(med, r['unit']) if med else f'({n} nights)'} | "
                     f"{f'{ratio:.2f}x' if ratio else ''} | {cv} | {flag} |")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir", help="run-corpus.py --out directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--history", action="append", default=[])
    ap.add_argument("--window", type=int, default=14, help="nights in the trailing median")
    ap.add_argument("--bound", type=float, default=1.5)
    args = ap.parse_args()

    rows = list(csv.DictReader(open(os.path.join(args.dir, "rows.csv"))))
    meta = json.load(open(os.path.join(args.dir, "meta.json")))
    out = make_rows(rows, meta)
    with open(args.out, "w") as fh:
        for r in out:
            fh.write(json.dumps(r, sort_keys=True) + "\n")

    results, notes = check(out, load_history(args.history), args.window, args.bound)
    for n in notes:
        print(n)
    text = summary(results, args.bound)
    sys.stdout.write(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as fh:
            fh.write(text + "\n")
    print(f"bench-rows: wrote {len(out)} rows to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
