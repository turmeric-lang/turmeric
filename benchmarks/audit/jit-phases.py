#!/usr/bin/env python3
"""WP4: where a `tur jit` run spends its time, across the corpus.

Runs every jit-engine program in benchmarks/audit/corpus.txt as
`tur --timings=<json> jit <prog>` with TUR_JIT_TIMING=1, --reps times, and
keeps each program's fastest run (best of N, as the rest of the audit does).
The compiler's zone timer gives the front end (everything outside its `jit`
and `run` zones); the engine's own TUR_JIT_TIMING lines split the engine's
work into c2mir, gen_init, load, link and run.  Under the default lazy
generation interface, `run` includes generating each function at its first
call.

Usage:
  python3 benchmarks/audit/jit-phases.py --tur ./build-release/tur [--reps 3] [--out FILE.csv]

Prints the median program's split and the corpus-wide share of each phase.
"""

import argparse
import csv
import json
import os
import re
import shlex
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CORPUS = os.path.join(ROOT, "benchmarks", "audit", "corpus.txt")
ENGINE = ("c2mir", "gen_init", "load", "link", "run")
COLS = ("wall_ms", "front_ms", "jit_ms", "prog_ms") + tuple(f"{p}_ms" for p in ENGINE) + (
    "prune_before_B", "prune_after_B", "maxrss_kib")


def jit_programs(path):
    for raw in open(path):
        f = raw.partition("#")[0].split()
        if len(f) >= 2 and "jit" in f[1].split(","):
            yield f[0]


def one(tur, prog, scratch):
    d = os.path.dirname(os.path.join(ROOT, prog))
    flags = []
    if prog.startswith("tests/fixtures/") and os.path.exists(os.path.join(d, "flags")):
        flags = shlex.split(open(os.path.join(d, "flags")).read())
    js = os.path.join(scratch, "t.json")
    env = dict(os.environ, TUR_JIT_TIMING="1")
    r = subprocess.run([tur] + flags + [f"--timings={js}", "jit", prog], cwd=ROOT, env=env,
                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True, timeout=600)
    if r.returncode != 0 or not os.path.exists(js):
        return None
    doc = json.load(open(js))
    os.unlink(js)
    row = {"wall_ms": doc["wall_ms"]}
    zone = {p["name"]: p["self_ms"] for p in doc["phases"]}
    row["jit_ms"] = zone.get("jit", 0.0)          # c2mir + link + teardown
    row["prog_ms"] = zone.get("run", 0.0)         # the program, incl. lazy gen
    row["front_ms"] = doc["wall_ms"] - row["jit_ms"] - row["prog_ms"]
    seen = set()
    for line in r.stderr.splitlines():
        f = line.split("\t")
        if len(f) < 4 or f[0] != "TUR_JIT_TIMING":
            continue
        if f[1] == "prune" and f[2] == "bytes":
            m = re.match(r"(\d+) -> (\d+)", f[3])
            if m:
                row["prune_before_B"], row["prune_after_B"] = int(m.group(1)), int(m.group(2))
        elif f[2] in ENGINE and f[2] not in seen:   # first engine attempt only
            seen.add(f[2])
            row[f"{f[2]}_ms"] = float(f[3])
        elif f[2] == "maxrss_kb":
            row["maxrss_kib"] = float(f[3])
    if "c2mir_ms" not in row:   # the engine declined; tur ran the cc fallback
        return None
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tur", default=os.environ.get("TUR", "./build-release/tur"))
    ap.add_argument("--corpus", default=CORPUS)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    tur = args.tur if os.path.isabs(args.tur) else os.path.abspath(os.path.join(ROOT, args.tur))

    rows = []
    with tempfile.TemporaryDirectory() as scratch:
        for prog in jit_programs(args.corpus):
            if not re.search(args.only, prog):
                continue
            runs = [x for x in (one(tur, prog, scratch) for _ in range(args.reps)) if x]
            if not runs:
                print(f"  {prog}: no engine run (failed, or fell back to cc)", file=sys.stderr)
                continue
            best = min(runs, key=lambda x: x["wall_ms"])
            best["program"], best["reps"] = prog, len(runs)
            rows.append(best)

    if args.out:
        with open(args.out, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=("program",) + COLS + ("reps",), extrasaction="ignore")
            w.writeheader()
            for x in rows:
                w.writerow({k: (round(v, 3) if isinstance(v, float) else v) for k, v in x.items()})

    def med(k):
        v = [x[k] for x in rows if k in x]
        return statistics.median(v) if v else float("nan")

    print(f"{len(rows)} programs; median program (ms):")
    for k in ("wall_ms", "front_ms", "jit_ms", "prog_ms") + tuple(f"{p}_ms" for p in ENGINE):
        print(f"  {k:12s} {med(k):9.2f}")
    tot = sum(x["wall_ms"] for x in rows)
    print("corpus-wide share of wall time:")
    for k in ("front_ms",) + tuple(f"{p}_ms" for p in ENGINE):
        print(f"  {k:12s} {100 * sum(x.get(k, 0) for x in rows) / tot:6.1f}%")
    fixed = [x for x in rows if x.get("run_ms", 1e9) < 20]
    if fixed:
        print(f"{len(fixed)} programs whose run phase is < 20 ms: median c2mir share of wall "
              f"{statistics.median(100 * x['c2mir_ms'] / x['wall_ms'] for x in fixed):.1f}%")


if __name__ == "__main__":
    main()
