#!/usr/bin/env python3
"""benchmarks/repl-turn/run.py -- what a compiled REPL turn costs.

aot-compiled-repl-plan C2's latency criterion: a turn in a session of up to
200 definitions takes at most 200 ms at the median.  For each session size
this types N one-line `defn`s into `tur repl --enable=compiled-repl`, then T
expression turns calling them, and reports:

  - the median and p90 wall time of an expression turn (between consecutive
    `=>` lines on the REPL's output);
  - the median of each phase of the turn's compile, from
    TUR_REPL_TURN_TIMING (src/driver/jit.c): front = compile_to_c, prep =
    split/prune/autolink, jit = c2mir + MIR link + generation, run = the
    program;
  - the REPL's peak RSS.

  python3 benchmarks/repl-turn/run.py [--tur build-release/tur]
      [--defs 1,50,200] [--turns 30] [--interp]

--interp also times the same sessions on the interpreter, as a floor.
Measure a Release build, alone on the machine: the numbers are wall-clock.
"""
import argparse
import os
import statistics
import subprocess
import sys
import time


def session(n_defs, n_turns):
    lines = [f'(defn f{k} [x : int] : int (+ x {k}))' for k in range(n_defs)]
    lines += [f'(f{i % n_defs} {i})' for i in range(n_turns)]
    return '\n'.join(lines) + '\n'


def run(tur, n_defs, n_turns, compiled):
    args = [tur, 'repl'] + (['--enable=compiled-repl'] if compiled else [])
    env = dict(os.environ, TUR_NO_AUTO_SPICE='1', TUR_REPL_TURN_TIMING='1',
               ASAN_OPTIONS='detect_leaks=0')
    p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=env, text=True, bufsize=1)
    p.stdin.write(session(n_defs, n_turns))
    p.stdin.close()
    stamps, phases = [], {'front': [], 'prep': [], 'jit': [], 'run': []}
    for line in p.stdout:
        now = time.monotonic()
        if line.startswith('=>'):
            stamps.append(now)
        elif line.startswith('TUR_REPL_TURN_TIMING'):
            for field in line.split()[1:]:
                k, _, v = field.partition('=')
                if k in phases:
                    phases[k].append(float(v))
    _, status, ru = os.wait4(p.pid, 0)
    if len(stamps) != n_defs + n_turns:
        sys.exit(f'repl-turn: expected {n_defs + n_turns} results, got {len(stamps)} '
                 f'({n_defs} defs, compiled={compiled})')
    # Expression turns only: the gaps after the last definition's `=>`.
    turns = [(b - a) * 1000.0 for a, b in zip(stamps[n_defs - 1:], stamps[n_defs:])]
    exprs = {k: v[n_defs:] for k, v in phases.items()}
    med = lambda xs: statistics.median(xs) if xs else 0.0
    p90 = sorted(turns)[int(0.9 * (len(turns) - 1))]
    return {
        'median': med(turns), 'p90': p90,
        'front': med(exprs['front']), 'prep': med(exprs['prep']),
        'jit': med(exprs['jit']), 'run': med(exprs['run']),
        'rss': ru.ru_maxrss / 1024.0,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tur', default='build-release/tur')
    ap.add_argument('--defs', default='1,50,200')
    ap.add_argument('--turns', type=int, default=30)
    ap.add_argument('--interp', action='store_true')
    a = ap.parse_args()
    if not os.access(a.tur, os.X_OK):
        sys.exit(f'repl-turn: no tur binary at {a.tur}')

    print(f'| engine | defs | median turn | p90 | front | prep | jit | run | peak RSS |')
    print(f'| --- | --- | --- | --- | --- | --- | --- | --- | --- |')
    for n in [int(x) for x in a.defs.split(',')]:
        engines = [('compiled', True)] + ([('interpreted', False)] if a.interp else [])
        for name, compiled in engines:
            r = run(a.tur, n, a.turns, compiled)
            if compiled:
                print(f"| {name} | {n} | {r['median']:.0f} ms | {r['p90']:.0f} ms | "
                      f"{r['front']:.0f} | {r['prep']:.0f} | {r['jit']:.0f} | {r['run']:.0f} | "
                      f"{r['rss']:.0f} MB |", flush=True)
            else:
                print(f"| {name} | {n} | {r['median']:.1f} ms | {r['p90']:.1f} ms | | | | | "
                      f"{r['rss']:.0f} MB |", flush=True)


if __name__ == '__main__':
    main()
