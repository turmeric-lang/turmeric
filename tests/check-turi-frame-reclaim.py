#!/usr/bin/env python3
"""check-turi-frame-reclaim.py -- the interpreter hands back call frames.

turi-call-frames-never-reclaimed: `tur --interpret` used to keep every call
frame and binding for the life of the process, ~4 KB a step, so a 1e5-element
loop peaked near 1 GB under the Debug tur.  An activation's frame now goes back
to a free list when it returns with nothing having captured it
(src/turi/eval.c, frame_release).

The check runs one program twice, with reclamation on and with it turned off
(TUR_TURI_FRAME_RECLAIM=0), and asserts that its growth in peak RSS -- above
what an idle r7rs program peaks at, which is the prelude plus, under ASan, the
sanitizer's own fixed cost -- drops by more than half.  A ratio, not a byte
bound, so it holds on the ASan Debug tur and a Release one alike.  The
program builds and walks a list through ordinary non-tail calls, a named let
and lets in tail position -- the shapes that leaked -- and its output is
checked too.

A second program pins turi-immutable-struct-args-copied-per-call: a symbol
and the empty list passed through 600,000 calls.  Both are `any` boxes over
values nothing can write, and the interpreter used to deep-copy each one at
every call and keep the copy forever (~50 B a call).  Its peak RSS must stay
within 16 MB of the idle program's.  ASan's free quarantine is turned off for
this run, so a Debug tur measures live memory rather than recently freed
memory (78 MB of growth before the fix, none after).

A third pins turi-call-pins-and-side-frames-not-reclaimed: a generic function
called 300,000 times.  Each call pins its type variables on its frame
(TyvarBind), and those nodes used to stay behind when the frame was handed
back -- 51 MB of growth on a Debug tur, none now.  Same 16 MB bound.

A fourth pins turi-effect-perform-keeps-its-continuation: 100,000 performs,
each resumed once by a clause that is just `(resume k 1)`.  Every perform used
to keep its continuation, a copy of the slice, the handler case frame and the
frames it captured -- ~2.8 KB a perform, 556 MB for 200,000 on a Release
tur.  The one-shot fast path runs the slice in place and frees all of it at
the resume.  Same 16 MB bound.

A fifth pins the defer snapshot from turi-call-pins-and-side-frames-not-
reclaimed: a function with a `defer` called 300,000 times.  Each defer
snapshots its captures in a frame of its own, which used to stay in the pool
after the defer fired -- 30 MB of growth on a Debug tur, none now.  Same
16 MB bound.

A sixth pins r7rs-callcc-memory-never-freed's capture epoch: the first
program again, run after ONE re-entrant `call/cc` whose continuation is
stored (and re-entered once, so the list is built and summed twice).  The
capture used to pin the interpreter -- frame_release did nothing for the rest
of the run, ~4 KB a step again (990 MB on a Debug tur against 133 without the
call/cc).  Frames made after the last capture are in no stack image, so they
are handed back as before; the program's growth over idle must stay within
twice the plain program's growth plus the same 16 MB.

usage: python3 tests/check-turi-frame-reclaim.py [TUR]   (default ./build/tur)
"""
import os
import subprocess
import sys
import tempfile

TUR = sys.argv[1] if len(sys.argv) > 1 else "./build/tur"

PROGRAM = """#lang r7rs
(import (scheme base) (scheme write))
(define (step x) (let ((y (+ x 1))) (let ((z (* y 1))) z)))
(define (build n)
  (let lp ((i 0) (acc '()))
    (if (= i n) acc (lp (step i) (cons i acc)))))
(define (sum l) (if (null? l) 0 (+ (car l) (sum (cdr l)))))
(let ((l (build 60000)))
  (display (sum l)) (newline)
  (display (length (map step l))) (newline))
"""
EXPECTED = "1799970000\n60000\n"
IDLE = "#lang r7rs\n(display 1)\n"

ARGS_PROGRAM = """#lang r7rs
(import (scheme base) (scheme write))
(define (id x) x)
(define (lp i s n) (if (= i 0) (list s n) (lp (- i 1) (id s) (id n))))
(write (lp 300000 'sym '())) (newline)
"""
ARGS_EXPECTED = "(sym ())\n"
ARGS_GROWTH_MB = 16

PINS_PROGRAM = """(defn idt [A] [x : A] : A x)
(defn lp [i : int acc : int] : int
  (if (= i 0) acc (lp (- i 1) (+ acc (idt 1)))))
(defn main [] : int (println (lp 300000 0)) 0)
"""
PINS_EXPECTED = "300000\n"
PINS_IDLE = "(defn main [] : int (println 1) 0)\n"

PERFORM_PROGRAM = """(defeffect Ask [] :int)
(defn ask-loop [n : int acc : int] : int
  (if (= n 0) acc (ask-loop (- n 1) (+ acc (perform (Ask))))))
(defn run [n : int] : int
  (handle (ask-loop n 0)
    (Ask [] k) (resume k 1)))
(defn main [] : int (println (run 100000)) 0)
"""
PERFORM_EXPECTED = "100000\n"

DEFER_PROGRAM = """(defn f [i : int] : int
  (defer (+ i 1))
  i)
(defn lp [i : int acc : int] : int
  (if (= i 0) acc (lp (- i 1) (+ acc (f i)))))
(defn main [] : int (println (lp 300000 0)) 0)
"""
DEFER_EXPECTED = "45000150000\n"

CALLCC_PROGRAM = """#lang r7rs
(import (scheme base) (scheme write))
(define saved #f)
(define hits 0)
(define (step x) (let ((y (+ x 1))) (let ((z (* y 1))) z)))
(define (build n)
  (let lp ((i 0) (acc '()))
    (if (= i n) acc (lp (step i) (cons i acc)))))
(define (sum l) (if (null? l) 0 (+ (car l) (sum (cdr l)))))
(define v (call/cc (lambda (k) (set! saved k) 0)))
(set! hits (+ hits 1))
(let ((l (build 60000)))
  (display (sum l)) (newline)
  (display (length (map step l))) (newline))
(if (< hits 2) (saved 1))
(display (list 'hits hits 'v v)) (newline)
"""
CALLCC_EXPECTED = EXPECTED + EXPECTED + "(hits 2 v 1)\n"


def run(src, reclaim, asan="detect_leaks=0"):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = asan
    env["TUR_TURI_FRAME_RECLAIM"] = "1" if reclaim else "0"
    p = subprocess.Popen([TUR, "--interpret", src], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=env)
    out = p.stdout.read().decode()
    p.stderr.read()
    _, status, ru = os.wait4(p.pid, 0)
    # ru_maxrss is KB on Linux but BYTES on macOS: the absolute growth bounds
    # below read 272 KiB of growth there as 272 MB.
    per_mb = 1024.0 * 1024.0 if sys.platform == "darwin" else 1024.0
    return out, status, ru.ru_maxrss / per_mb   # MB


def main():
    if not os.access(TUR, os.X_OK):
        print("check-turi-frame-reclaim: no tur at %s" % TUR)
        return 2
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "reclaim.tur")
        with open(src, "w") as f:
            f.write(PROGRAM)
        out_on, st_on, rss_on = run(src, True)
        out_off, st_off, rss_off = run(src, False)
        idle = os.path.join(d, "idle.tur")
        with open(idle, "w") as f:
            f.write(IDLE)
        _, _, rss_idle = run(idle, False)
        args = os.path.join(d, "args.tur")
        with open(args, "w") as f:
            f.write(ARGS_PROGRAM)
        noq = "detect_leaks=0:quarantine_size_mb=0"
        out_args, st_args, rss_args = run(args, True, noq)
        _, _, rss_idle_noq = run(idle, True, noq)
        pins = os.path.join(d, "pins.tur")
        with open(pins, "w") as f:
            f.write(PINS_PROGRAM)
        pins_idle = os.path.join(d, "pins_idle.tur")
        with open(pins_idle, "w") as f:
            f.write(PINS_IDLE)
        out_pins, st_pins, rss_pins = run(pins, True, noq)
        _, _, rss_pins_idle = run(pins_idle, True, noq)
        perf = os.path.join(d, "perform.tur")
        with open(perf, "w") as f:
            f.write(PERFORM_PROGRAM)
        out_perf, st_perf, rss_perf = run(perf, True, noq)
        dfr = os.path.join(d, "defer.tur")
        with open(dfr, "w") as f:
            f.write(DEFER_PROGRAM)
        out_dfr, st_dfr, rss_dfr = run(dfr, True, noq)
        cc = os.path.join(d, "callcc.tur")
        with open(cc, "w") as f:
            f.write(CALLCC_PROGRAM)
        out_cc, st_cc, rss_cc = run(cc, True, noq)
        _, _, rss_on_noq = run(src, True, noq)
    ok = True
    for label, out, st in (("reclaim on", out_on, st_on),
                           ("reclaim off", out_off, st_off)):
        if st != 0 or out != EXPECTED:
            print("FAIL check-turi-frame-reclaim: %s printed %r (status %d), want %r"
                  % (label, out, st, EXPECTED))
            ok = False
    grow_on, grow_off = rss_on - rss_idle, rss_off - rss_idle
    msg = ("peak RSS %.0f MB with reclamation, %.0f MB without, %.0f MB idle"
           % (rss_on, rss_off, rss_idle))
    if ok and not grow_on * 2 < grow_off:
        print("FAIL check-turi-frame-reclaim: %s -- frames are not being handed back" % msg)
        ok = False
    if ok:
        print("PASS check-turi-frame-reclaim: %s" % msg)
    grow_args = rss_args - rss_idle_noq
    amsg = ("argument passing: peak RSS %.0f MB, %.0f MB idle, growth %.0f MB (bound %d)"
            % (rss_args, rss_idle_noq, grow_args, ARGS_GROWTH_MB))
    if st_args != 0 or out_args != ARGS_EXPECTED:
        print("FAIL check-turi-frame-reclaim: argument program printed %r (status %d), want %r"
              % (out_args, st_args, ARGS_EXPECTED))
        ok = False
    elif grow_args > ARGS_GROWTH_MB:
        print("FAIL check-turi-frame-reclaim: %s -- unwritable by-value arguments are being copied" % amsg)
        ok = False
    else:
        print("PASS check-turi-frame-reclaim: %s" % amsg)
    grow_pins = rss_pins - rss_pins_idle
    pmsg = ("generic calls: peak RSS %.0f MB, %.0f MB idle, growth %.0f MB (bound %d)"
            % (rss_pins, rss_pins_idle, grow_pins, ARGS_GROWTH_MB))
    if st_pins != 0 or out_pins != PINS_EXPECTED:
        print("FAIL check-turi-frame-reclaim: generic-call program printed %r (status %d), want %r"
              % (out_pins, st_pins, PINS_EXPECTED))
        ok = False
    elif grow_pins > ARGS_GROWTH_MB:
        print("FAIL check-turi-frame-reclaim: %s -- tyvar/dict pins are not handed back" % pmsg)
        ok = False
    else:
        print("PASS check-turi-frame-reclaim: %s" % pmsg)
    grow_perf = rss_perf - rss_pins_idle
    fmsg = ("performs: peak RSS %.0f MB, %.0f MB idle, growth %.0f MB (bound %d)"
            % (rss_perf, rss_pins_idle, grow_perf, ARGS_GROWTH_MB))
    if st_perf != 0 or out_perf != PERFORM_EXPECTED:
        print("FAIL check-turi-frame-reclaim: perform program printed %r (status %d), want %r"
              % (out_perf, st_perf, PERFORM_EXPECTED))
        ok = False
    elif grow_perf > ARGS_GROWTH_MB:
        print("FAIL check-turi-frame-reclaim: %s -- a one-shot resume keeps its continuation" % fmsg)
        ok = False
    else:
        print("PASS check-turi-frame-reclaim: %s" % fmsg)
    grow_dfr = rss_dfr - rss_pins_idle
    dmsg = ("defers: peak RSS %.0f MB, %.0f MB idle, growth %.0f MB (bound %d)"
            % (rss_dfr, rss_pins_idle, grow_dfr, ARGS_GROWTH_MB))
    if st_dfr != 0 or out_dfr != DEFER_EXPECTED:
        print("FAIL check-turi-frame-reclaim: defer program printed %r (status %d), want %r"
              % (out_dfr, st_dfr, DEFER_EXPECTED))
        ok = False
    elif grow_dfr > ARGS_GROWTH_MB:
        print("FAIL check-turi-frame-reclaim: %s -- a fired defer keeps its snapshot" % dmsg)
        ok = False
    else:
        print("PASS check-turi-frame-reclaim: %s" % dmsg)
    grow_cc, grow_base = rss_cc - rss_idle_noq, rss_on_noq - rss_idle_noq
    cc_bound = 2 * grow_base + ARGS_GROWTH_MB
    cmsg = ("after a call/cc: peak RSS %.0f MB, %.0f MB idle, growth %.0f MB "
            "(plain program %.0f MB; bound %.0f)"
            % (rss_cc, rss_idle_noq, grow_cc, grow_base, cc_bound))
    if st_cc != 0 or out_cc != CALLCC_EXPECTED:
        print("FAIL check-turi-frame-reclaim: call/cc program printed %r (status %d), want %r"
              % (out_cc, st_cc, CALLCC_EXPECTED))
        ok = False
    elif grow_cc > cc_bound:
        print("FAIL check-turi-frame-reclaim: %s -- a capture pins every later frame" % cmsg)
        ok = False
    else:
        print("PASS check-turi-frame-reclaim: %s" % cmsg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
