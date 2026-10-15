#!/usr/bin/env python3
"""R-1 census: how many functions are CPS-colored, and why.

performance-audit-plan WP2's lead item. Runs `TUR_CPS_CENSUS=1 tur emit-c`
over a set of programs and aggregates the `cps-census` lines cps_color_program
prints (src/passes/cps.c), one per function node:

  cps-census <colored|uncolored> <cause> <ind|-> <module|-> <file> <name>

cause is `control` (the body holds a control op), `callee` (a resolved callee
reaches one, or an effectful fn-value argument force-colored it), `indirect`
(colored ONLY by the conservative rule that an unresolved call may reach a
control op), or `-` (uncolored). A function whose file is under stdlib/ is
counted as stdlib -- the auto-loaded prelude is top-level, not a module, so the
module column cannot tell it from the program's own code.

Usage:
  python3 benchmarks/audit/cps-census.py --tur ./build-release/tur [--out DIR] [paths...]

With no paths it sweeps every tests/fixtures/*/ program (not errors/), every
examples/**/*.tur and benchmarks/**/*.tur. A fixture's `flags` file is passed
as global flags, as tests/run.sh does. Writes DIR/programs.csv (one row per
program), DIR/stdlib.csv (one row per stdlib function seen colored) and prints
the summary.
"""

import argparse
import collections
import concurrent.futures
import csv
import glob
import os
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
CAUSES = ("control", "callee", "indirect")


def default_programs():
    progs = []
    for d in sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "*", ""))):
        if os.path.basename(os.path.dirname(d)) == "errors":
            continue
        f = os.path.join(d, "input.tur")
        if not os.path.exists(f):
            name = os.path.basename(os.path.dirname(d))
            f = os.path.join(d, name + ".tur")
        if os.path.exists(f):
            progs.append(f)
    for pat in ("examples/**/*.tur", "benchmarks/**/*.tur"):
        progs += sorted(p for p in glob.glob(os.path.join(ROOT, pat), recursive=True)
                        if os.path.basename(p) != "build.tur" and "/output/" not in p)
    return progs


def census_one(tur, path, timeout):
    flags = []
    d = os.path.dirname(path)
    if os.path.exists(os.path.join(d, "flags")) and "/tests/fixtures/" in path:
        flags = shlex.split(open(os.path.join(d, "flags")).read())
    env = dict(os.environ, TUR_CPS_CENSUS="1")
    try:
        r = subprocess.run([tur] + flags + ["emit-c", os.path.relpath(path, ROOT)],
                           cwd=ROOT, env=env,
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, timeout=timeout)
    except subprocess.TimeoutExpired:
        return path, "timeout", {}
    fns = {}
    for line in r.stderr.decode("utf-8", "replace").splitlines():
        if not line.startswith("cps-census "):
            continue
        parts = line.split(" ", 6)
        if len(parts) != 7:
            continue
        _, state, cause, ind, module, file, name = parts
        # One entry per (file, name): with a --dump-cps* flag the coloring runs
        # more than once; the last run is the one the emitter used.
        fns[(file, name)] = (state, cause, ind, module)
    status = "ok" if r.returncode == 0 else f"rc={r.returncode}"
    return path, status, fns


def is_stdlib(file):
    return "/stdlib/" in file or file.startswith("stdlib/")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tur", default=os.environ.get("TUR", "./build-release/tur"))
    ap.add_argument("--out", default="")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("paths", nargs="*")
    args = ap.parse_args()
    tur = os.path.abspath(os.path.join(ROOT, args.tur)) if not os.path.isabs(args.tur) else args.tur
    progs = [os.path.abspath(p) for p in args.paths] or default_programs()

    results = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for res in ex.map(lambda p: census_one(tur, p, args.timeout), progs):
            results.append(res)

    rows = []
    stdlib_colored = collections.defaultdict(lambda: collections.Counter())
    stdlib_seen = collections.Counter()
    for path, status, fns in results:
        rel = os.path.relpath(path, ROOT)
        user = collections.Counter()
        std = collections.Counter()
        n_user = 0
        for (file, name), (state, cause, ind, module) in fns.items():
            if is_stdlib(file):
                stdlib_seen[name] += 1
                if state == "colored":
                    std[cause] += 1
                    stdlib_colored[name][cause] += 1
            else:
                n_user += 1
                if state == "colored":
                    user[cause] += 1
        rows.append({"program": rel, "status": status, "user_fns": n_user,
                     **{f"user_{c}": user[c] for c in CAUSES},
                     **{f"stdlib_{c}": std[c] for c in CAUSES}})

    ok = [r for r in rows if r["status"] == "ok" and r["user_fns"] > 0]
    tot = {c: sum(r[f"user_{c}"] for r in ok) for c in CAUSES}
    user_fns = sum(r["user_fns"] for r in ok)
    colored_any = [r for r in ok if sum(r[f"user_{c}"] for c in CAUSES)]
    indirect_any = [r for r in ok if r["user_indirect"]]
    indirect_only = [r for r in colored_any
                     if r["user_indirect"] and not r["user_control"] and not r["user_callee"]]
    no_control = [r for r in colored_any if not r["user_control"] and not r["user_callee"]]

    print(f"cps-census: {len(rows)} programs, {len(ok)} compiled with user functions "
          f"({len(rows) - len([r for r in rows if r['status'] == 'ok'])} failed or timed out)")
    print(f"  user functions: {user_fns}; colored: {sum(tot.values())} "
          f"(control {tot['control']}, callee {tot['callee']}, indirect-only {tot['indirect']})")
    print(f"  programs with any colored user fn:            {len(colored_any)}")
    print(f"  programs with an indirect-only colored fn:    {len(indirect_any)}")
    print(f"  programs whose coloring is ALL indirect-only: {len(no_control)}")
    s_tot = collections.Counter()
    for name, c in stdlib_colored.items():
        # A stdlib function's cause can differ by program (a callee's coloring
        # depends on what else is linked); count its most common cause.
        s_tot[c.most_common(1)[0][0]] += 1
    print(f"  distinct stdlib functions seen: {len(stdlib_seen)}; colored in some program: "
          f"{len(stdlib_colored)} (by majority cause: control {s_tot['control']}, "
          f"callee {s_tot['callee']}, indirect-only {s_tot['indirect']})")
    print("  most indirect-only colored user fns:")
    for r in sorted(ok, key=lambda r: -r["user_indirect"])[:10]:
        print(f"    {r['user_indirect']:4} indirect  {r['user_control']:3} control  "
              f"{r['user_callee']:3} callee  of {r['user_fns']:4}  {r['program']}")

    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "programs.csv"), "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
            w.writeheader()
            w.writerows(rows)
        with open(os.path.join(args.out, "stdlib.csv"), "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["function", "programs_seen", "programs_colored"] + [f"as_{c}" for c in CAUSES])
            for name, c in sorted(stdlib_colored.items(), key=lambda kv: -sum(kv[1].values())):
                w.writerow([name, stdlib_seen[name], sum(c.values())] + [c[k] for k in CAUSES])
    return 0


if __name__ == "__main__":
    sys.exit(main())
