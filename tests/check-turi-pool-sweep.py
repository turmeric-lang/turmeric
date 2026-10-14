#!/usr/bin/env python3
"""check-turi-pool-sweep.py -- the interpreter hands back what nothing reaches.

turi-dropped-closures-kept-until-teardown: under `tur --interpret` a closure,
the frame it captured and the bindings in it, and a small struct (a pair, a
record) used to stay in the env's value pool until the env was freed, so a
`#lang r7rs` program that made them in a loop -- a `guard`, a `lambda` passed
to a procedure, `map` -- grew for the life of the process.  The pool sweep now
runs whenever the pool has grown by its budget: it marks everything reachable
(the continuation-image sweep's marker, src/turi/eval.c turi_cont_gc_*) and
hands every unmarked frame, binding, closure and struct back to its free list
(turi_cont_gc_reclaim; src/turi/interpreter_natives.c r7k_maybe_sweep).

Two checks:

1. Soundness.  tests/fixtures/r7rs-pool-sweep-places keeps a closure in each
   kind of place a program can keep one -- a vector, a list, a record, a
   closure, a promise, a parameter, a SRFI 69 table, an alist, the arguments
   of a call in progress, a parameterize, a dynamic-wind, a raised payload, a
   continuation, a deep recursion -- while it makes garbage, then calls each.
   Run here with a sweep every 64 KB and every object a sweep hands back
   poisoned instead of reused (TUR_TURI_POOL_GC_QUARANTINE=1), so under a
   Debug (ASan) tur a place the marker cannot read is a use-after-poison
   report at the first use.  The output must be the fixture's, and sweeps
   must have run and handed objects back.

2. Memory.  Three loops that make garbage each time round -- a `guard`, a
   `lambda` passed to a procedure, a `map` over a short list -- run at N and
   4N iterations.  With the sweep their peak RSS must not grow by more than
   8 MB from N to 4N; with it off (TUR_TURI_POOL_GC=0) the same loops must
   grow by more, so it is the sweep that bounds them.  ASan's free quarantine
   is off for these runs, so a Debug tur measures live memory.

usage: python3 tests/check-turi-pool-sweep.py [TUR]   (default ./build/tur)
"""
import os
import re
import subprocess
import sys
import tempfile

TUR = sys.argv[1] if len(sys.argv) > 1 else "./build/tur"
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "r7rs-pool-sweep-places")

HEAD = "#lang r7rs\n(import (scheme base) (scheme write))\n"
LOOPS = {
    "guard": (10000, HEAD + """(define (loop i acc)
  (if (= i %d) acc
      (loop (+ i 1) (+ acc (guard (e (#t 1)) 1)))))
(write (loop 0 0))
(newline)
"""),
    "lambda": (20000, HEAD + """(define (apply1 f) (f 1))
(define (loop i acc)
  (if (= i %d) acc
      (loop (+ i 1) (+ acc (apply1 (lambda (x) (+ x i)))))))
(write (loop 0 0))
(newline)
"""),
    "map": (3000, HEAD + """(define l (list 1 2 3 4 5 6 7 8 9 10))
(define (loop i acc)
  (if (= i %d) acc
      (loop (+ i 1) (+ acc (apply + (map (lambda (x) (* x 2)) l))))))
(write (loop 0 0))
(newline)
"""),
}
FLAT_MB = 8      # growth from N to 4N allowed with the sweep on
OFF_MIN_MB = 8   # and the least it must grow by with the sweep off

failures = []


def env_with(**extra):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:quarantine_size_mb=0"
    for k in ("TUR_TURI_CONT_GC", "TUR_TURI_CONT_GC_STRESS", "TUR_TURI_CONT_GC_TRACE",
              "TUR_TURI_POOL_GC", "TUR_TURI_POOL_GC_STRESS", "TUR_TURI_POOL_GC_QUARANTINE"):
        env.pop(k, None)
    env.update(extra)
    return env


def peak_of(path, **extra):
    """Peak RSS of one run, measured in a fresh child so earlier runs do not
    mask it (RUSAGE_CHILDREN keeps the maximum over every child)."""
    code = ("import resource,subprocess,sys,os;"
            "p=subprocess.run(sys.argv[1:],capture_output=True,text=True);"
            "sys.stdout.write(p.stdout);"
            "sys.stderr.write('%d %d\\n' % (p.returncode, "
            "resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss))")
    p = subprocess.run([sys.executable, "-c", code, TUR, "--interpret", path],
                       capture_output=True, text=True, env=env_with(**extra), timeout=600)
    rc, kb = p.stderr.strip().split("\n")[-1].split()
    if rc != "0":
        failures.append("%s exited %s" % (path, rc))
    return p.stdout, int(kb) / 1024.0


with tempfile.TemporaryDirectory() as tmp:
    # 1. Soundness: a sweep every 64 KB, what it hands back poisoned.
    expected = open(os.path.join(FIXTURE, "expected.stdout")).read()
    p = subprocess.run([TUR, "--interpret", os.path.join(FIXTURE, "input.tur")],
                       capture_output=True, text=True, timeout=600,
                       env=env_with(TUR_TURI_POOL_GC_STRESS="65536",
                                    TUR_TURI_POOL_GC_QUARANTINE="1",
                                    TUR_TURI_CONT_GC_TRACE="1"))
    if p.returncode != 0 or p.stdout != expected:
        failures.append("the places fixture under stress exited %d, printed:\n%s\nexpected:\n%s"
                        "\nstderr tail:\n%s" % (p.returncode, p.stdout, expected,
                                                p.stderr.strip()[-1500:]))
    sweeps = [l for l in p.stderr.splitlines() if l.startswith("r7k-gc (pool):")]
    ran = [l for l in sweeps if "none freed" not in l]
    handed = sum(int(m.group(1)) for m in (re.search(r"(\d+) KB of frames", l) for l in ran) if m)
    print("stress: %d pool sweeps, %d completed, %d KB handed back"
          % (len(sweeps), len(ran), handed))
    if len(ran) < 20:
        failures.append("only %d pool sweeps completed under TUR_TURI_POOL_GC_STRESS" % len(ran))
    if handed < 1024:
        failures.append("the stress sweeps handed back only %d KB" % handed)

    # 2. Memory: flat with the sweep, growing without it.
    for name, (n, src) in LOOPS.items():
        peaks = {}
        for label, iters, extra in (("small", n, {}), ("large", 4 * n, {}),
                                    ("small, off", n, {"TUR_TURI_POOL_GC": "0"}),
                                    ("large, off", 4 * n, {"TUR_TURI_POOL_GC": "0"})):
            path = os.path.join(tmp, "%s-%d.scm" % (name, iters))
            with open(path, "w") as f:
                f.write(src % iters)
            out, peaks[label] = peak_of(path, **extra)
            if not out.strip().isdigit():
                failures.append("%s loop (%s) printed %r" % (name, label, out))
        on = peaks["large"] - peaks["small"]
        off = peaks["large, off"] - peaks["small, off"]
        print("%-6s %6d -> %6d iterations: peak RSS %+.0f MB (%+.0f MB with the sweep off)"
              % (name, n, 4 * n, on, off))
        if on > FLAT_MB:
            failures.append("the %s loop grew %.0f MB with the sweep on (allowed %d)"
                            % (name, on, FLAT_MB))
        if off < OFF_MIN_MB or off < 2 * on:
            failures.append("with the sweep off the %s loop grew only %.0f MB -- the check "
                            "no longer shows what the sweep bounds" % (name, off))

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print("OK")
