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


def run(src, reclaim):
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    env["TUR_TURI_FRAME_RECLAIM"] = "1" if reclaim else "0"
    p = subprocess.Popen([TUR, "--interpret", src], stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=env)
    out = p.stdout.read().decode()
    p.stderr.read()
    _, status, ru = os.wait4(p.pid, 0)
    return out, status, ru.ru_maxrss / 1024.0   # MB (Linux reports KB)


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
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
