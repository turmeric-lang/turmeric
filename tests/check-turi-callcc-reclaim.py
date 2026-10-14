#!/usr/bin/env python3
"""check-turi-callcc-reclaim.py -- the interpreter frees unreachable call/cc images.

r7rs-callcc-memory-never-freed: under `tur --interpret` a re-entrant
continuation is a copy of the C stack plus the evaluator's saved state, about
2 KB a capture, and nothing used to free it -- a loop that stored each
continuation it made grew by that much per iteration for the life of the
process.  A sweep now runs at a capture once enough continuations are live,
marks everything reachable (conservatively: the C stack, the images it
reaches, the value pool through its object map) and frees the images whose
handle nothing reached (src/turi/interpreter_natives.c, r7k_maybe_sweep;
src/turi/eval.c, turi_cont_gc_*).

Two checks:

1. Soundness.  tests/fixtures/r7rs-callcc-reclaimed-images stores a
   continuation in each kind of place a program can keep one -- a vector, a
   nest of pairs, a record, a closure, a promise, a parameter, the argument
   list of a call in progress, another continuation's image, an exception
   payload, the dynamic environment, a SRFI 69 table -- makes captures it
   drops, and re-enters the stored one.  Run here with a sweep at EVERY
   capture (TUR_TURI_CONT_GC_STRESS=1) and fewer captures in between, so a
   place the marker cannot read frees an image the program still holds and
   the re-entry fails with r7rs-cont-restore__'s error.  Every sweep must
   complete (none declined) and the sweeps must free continuations.

2. Memory.  A loop that stores every continuation it captures, against the
   same loop storing a fresh closure instead -- the interpreter keeps a
   stored closure's frames for the life of the process too, so that is the
   floor.  The continuation loop's growth in peak RSS over an idle program
   must stay within twice the closure loop's plus 16 MB, and with the sweep
   off (TUR_TURI_CONT_GC=0) it must be at least twice what it is with the
   sweep on -- so it is the sweep that bounds it.  ASan's free quarantine is
   off for these runs, so a Debug tur measures live memory.

usage: python3 tests/check-turi-callcc-reclaim.py [TUR]   (default ./build/tur)
"""
import os
import subprocess
import sys
import tempfile

TUR = sys.argv[1] if len(sys.argv) > 1 else "./build/tur"
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "r7rs-callcc-reclaimed-images")

N = 40000
LOOP = """#lang r7rs
(import (scheme base) (scheme write))
(define saved #f)
(define (loop i acc)
  (if (= i %d) acc
      (loop (+ i 1) (+ acc %s))))
(write (loop 0 0))
(newline)
"""
STORED = "(call/cc (lambda (k) (set! saved k) 1))"
CLOSURE = "((lambda (k) (set! saved k) 1) (lambda (x) (+ x i)))"
IDLE = "#lang r7rs\n(display 1)\n"
GROWTH_SLACK_MB = 16

failures = []


def env_with(**extra):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0:quarantine_size_mb=0"
    for k in ("TUR_TURI_CONT_GC", "TUR_TURI_CONT_GC_STRESS", "TUR_TURI_CONT_GC_TRACE"):
        env.pop(k, None)
    env.update(extra)
    return env


def run(path, **extra):
    """Run `tur --interpret path`; return (stdout, stderr)."""
    p = subprocess.run([TUR, "--interpret", path], capture_output=True, text=True,
                       env=env_with(**extra), timeout=600)
    if p.returncode != 0:
        failures.append("%s exited %d: %s" % (path, p.returncode, p.stderr.strip()[-400:]))
    return p.stdout, p.stderr


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
    # 1. Soundness, with a sweep at every capture.
    src = open(os.path.join(FIXTURE, "input.tur")).read()
    expected = open(os.path.join(FIXTURE, "expected.stdout")).read()
    assert "(define CHURN 600)" in src and "(pending second 600 2)" in expected
    stress = os.path.join(tmp, "stress.scm")
    with open(stress, "w") as f:
        f.write(src.replace("(define CHURN 600)", "(define CHURN 40)"))
    out, err = run(stress, TUR_TURI_CONT_GC_STRESS="1", TUR_TURI_CONT_GC_TRACE="1")
    want = expected.replace("(pending second 600 2)", "(pending second 40 2)")
    if out != want:
        failures.append("stress run printed:\n%s\nexpected:\n%s\nstderr tail:\n%s"
                        % (out, want, err.strip()[-600:]))
    sweeps = [l for l in err.splitlines() if l.startswith("r7k-gc:")]
    declined = [l for l in sweeps if "none freed" in l]
    freed = sum(int(l.split(",")[1].split()[0]) for l in sweeps if "none freed" not in l)
    print("stress: %d sweeps, %d declined, %d continuations freed"
          % (len(sweeps), len(declined), freed))
    if len(sweeps) < 100:
        failures.append("only %d sweeps ran under TUR_TURI_CONT_GC_STRESS=1" % len(sweeps))
    if declined:
        failures.append("%d sweeps declined, first: %s" % (len(declined), declined[0]))
    if freed < 100:
        failures.append("the stress sweeps freed only %d continuations" % freed)

    # 2. Memory.
    paths = {}
    for name, body in (("idle", None), ("closure", CLOSURE), ("stored", STORED)):
        paths[name] = os.path.join(tmp, name + ".scm")
        with open(paths[name], "w") as f:
            f.write(IDLE if body is None else LOOP % (N, body))
    _, idle = peak_of(paths["idle"])
    out_c, closure = peak_of(paths["closure"])
    out_on, on = peak_of(paths["stored"])
    out_off, off = peak_of(paths["stored"], TUR_TURI_CONT_GC="0")
    for label, out in (("closure", out_c), ("stored", out_on), ("stored, sweep off", out_off)):
        if out != "%d\n" % N:
            failures.append("%s loop printed %r" % (label, out))
    g_closure, g_on, g_off = closure - idle, on - idle, off - idle
    print("peak RSS: idle %.0f MB; %d iterations storing a closure +%.0f MB, a continuation "
          "+%.0f MB (+%.0f MB with the sweep off)" % (idle, N, g_closure, g_on, g_off))
    if g_on > 2 * g_closure + GROWTH_SLACK_MB:
        failures.append("storing continuations grew %.0f MB, over the bound %.0f MB"
                        % (g_on, 2 * g_closure + GROWTH_SLACK_MB))
    if g_off < 2 * g_on:
        failures.append("with the sweep off the loop grew %.0f MB, not twice the %.0f MB "
                        "it grows with it on" % (g_off, g_on))

if failures:
    for f in failures:
        print("FAIL:", f)
    sys.exit(1)
print("OK")
