#!/usr/bin/env python3
"""WP1: where `tur build` / `tur emit-c` spend their time, across the corpus.

Runs every program in benchmarks/audit/corpus.txt under `--timings=<json>`
(the compiler's own zone timer, docs/guides/performance-guide.md "Compiler
timings"), --reps times per command, and keeps each program's fastest run --
the same best-of-N the rest of the audit uses, for the same reason: one run
on the WP0 box can take a burst that has nothing to do with the compiler.

Usage:
  python3 benchmarks/audit/timings-corpus.py --tur ./build-release/tur [--reps 5] [--out DIR]

Writes DIR/phases.csv (one row per program, command and phase) and prints:
the per-phase share of total time summed over the corpus, the median
program's split, and the programs that spend the most in each front-end zone.
A fixture's `flags` are passed as global flags, as tests/run.sh does.
"""

import argparse
import collections
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
FRONT = ("read", "sweet", "module-load", "macro-expand", "elaborate", "typeclass",
         "kind-check", "effect-lower", "effect-row", "cps", "borrow-check",
         "refine", "mono")


def corpus_paths(path):
    out = []
    for raw in open(path):
        line = raw.partition("#")[0].split()
        if line:
            out.append(line[0])
    return out


def timed(tur, prog, cmd, scratch):
    d = os.path.dirname(os.path.join(ROOT, prog))
    flags = []
    if prog.startswith("tests/fixtures/") and os.path.exists(os.path.join(d, "flags")):
        flags = shlex.split(open(os.path.join(d, "flags")).read())
    js = os.path.join(scratch, "t.json")
    argv = [tur] + flags + [f"--timings={js}"] + cmd + [prog]
    if cmd == ["build"]:
        argv += ["-o", os.path.join(scratch, "a.out")]
    r = subprocess.run(argv, cwd=ROOT, stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    if r.returncode != 0 or not os.path.exists(js):
        return None
    doc = json.load(open(js))
    os.unlink(js)
    return doc


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tur", default=os.environ.get("TUR", "./build-release/tur"))
    ap.add_argument("--corpus", default=CORPUS)
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--only", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    tur = os.path.abspath(os.path.join(ROOT, args.tur)) if not os.path.isabs(args.tur) else args.tur

    progs = [p for p in corpus_paths(args.corpus) if re.search(args.only, p)]
    rows = []
    best = {}
    with tempfile.TemporaryDirectory() as scratch:
        for n, prog in enumerate(progs):
            for cmd in (["build"], ["emit-c"]):
                docs = [d for d in (timed(tur, prog, cmd, scratch) for _ in range(args.reps)) if d]
                if not docs:
                    print(f"  {prog}: {cmd[0]} failed", file=sys.stderr)
                    continue
                doc = min(docs, key=lambda d: d["wall_ms"])
                best[(prog, cmd[0])] = doc
                for ph in doc["phases"]:
                    rows.append({"program": prog, "command": cmd[0], "phase": ph["name"],
                                 "self_ms": ph["self_ms"], "self_pct": ph["self_pct"],
                                 "wall_ms": doc["wall_ms"], "reps": len(docs)})
            print(f"[{n + 1}/{len(progs)}] {prog}", file=sys.stderr, flush=True)

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "phases.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
            w.writeheader()
            w.writerows(rows)

    for cmd in ("build", "emit-c"):
        docs = {p: d for (p, c), d in best.items() if c == cmd}
        if not docs:
            continue
        tot = collections.Counter()
        for d in docs.values():
            for ph in d["phases"]:
                tot[ph["name"]] += ph["self_ms"]
        wall = sum(d["wall_ms"] for d in docs.values())
        print(f"\n== tur {cmd}: {len(docs)} programs, {wall:.0f} ms summed wall "
              f"(median program {statistics.median(d['wall_ms'] for d in docs.values()):.1f} ms)")
        print(f"   {'phase':14} {'sum ms':>10} {'share':>7} {'median ms':>10} {'max ms':>9}  max program")
        for name, ms in tot.most_common():
            if ms <= 0:
                continue
            per = [next(ph["self_ms"] for ph in d["phases"] if ph["name"] == name) for d in docs.values()]
            mx = max(docs, key=lambda p: next(ph["self_ms"] for ph in docs[p]["phases"] if ph["name"] == name))
            print(f"   {name:14} {ms:10.1f} {100 * ms / wall:6.1f}% {statistics.median(per):10.2f} "
                  f"{max(per):9.1f}  {mx}")
        fe = {p: sum(ph["self_ms"] for ph in d["phases"] if ph["name"] in FRONT) for p, d in docs.items()}
        print(f"   front end (read .. mono): median {statistics.median(fe.values()):.1f} ms, "
              f"max {max(fe.values()):.1f} ms ({max(fe, key=fe.get)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
