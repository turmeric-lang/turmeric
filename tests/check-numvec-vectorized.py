#!/usr/bin/env python3
"""check-numvec-vectorized.py -- the numvec loops that should vectorize, do.

simd-and-vectorization-plan N1 and N2.  The N1 kernels of stdlib/numvec.tur
are written so gcc vectorizes them at -O3 (no control flow in the loop, a fixed
eight-lane order for float reductions), and N2 lets a loop a program writes
itself over an array vectorize (unchecked reads, no panic test after a leaf
inline-C call).  Both properties are invisible in a program's output: a later
preamble or emitter change could quietly put control flow back in a loop and
every fixture would still pass.  This compiles tests/vectorize/numvec-probe.tur
with `gcc -O3 -fopt-info-vec-optimized` and fails if any loop listed in
EXPECTED is not reported as vectorized.

Which loops vectorize depends on the instruction set, so the list is for
x86-64's baseline (SSE2); elsewhere, or without gcc, this skips.  Not listed,
on purpose: `-copy!` (a memmove), a u8 `-fill!` (gcc turns it into memset),
`-sum-ordered` (the strict left fold), and the 64-bit integer multiplies and
min/max that SSE2 has no vector instruction for.

Usage: python3 tests/check-numvec-vectorized.py   (TUR=path to tur)
"""
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PROBE = os.path.join(ROOT, "tests", "vectorize", "numvec-probe.tur")

FLOAT_KERNELS = ["fill!", "scale!", "add!", "axpy!", "sum", "dot", "min", "max"]
EXPECTED = (
    ["f64vec-" + k for k in FLOAT_KERNELS]
    + ["f32vec-" + k for k in FLOAT_KERNELS]
    + ["i64vec-" + k for k in ["fill!", "add!", "axpy!", "sum"]]
    + ["i32vec-" + k for k in ["fill!", "scale!", "add!", "axpy!", "sum", "min", "max"]]
    + ["u8vec-" + k for k in ["add!", "sum", "min", "max"]]
    # N2 acceptance: loops written in Turmeric over an array
    + ["probe-int-fold", "probe-int-for", "probe-float-partials"]
)


def mangle(name):
    return name.replace("-", "_hy").replace("!", "_ex")


def main():
    if platform.machine() not in ("x86_64", "AMD64"):
        print("check-numvec-vectorized: SKIP (the expected set is x86-64's; host is %s)"
              % platform.machine())
        return 0
    gcc = shutil.which("gcc")
    if not gcc:
        print("check-numvec-vectorized: SKIP (no gcc)")
        return 0
    ver = subprocess.run([gcc, "--version"], capture_output=True, text=True).stdout
    if "clang" in ver.lower():
        print("check-numvec-vectorized: SKIP (gcc is clang; -fopt-info is gcc's)")
        return 0
    tur = os.environ.get("TUR", os.path.join(ROOT, "build", "tur"))
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "probe.c")
        with open(c, "w") as out:
            r = subprocess.run([tur, "emit-c", PROBE], stdout=out,
                               stderr=subprocess.PIPE, text=True, cwd=ROOT)
        if r.returncode != 0:
            print("FAIL check-numvec-vectorized -- emit-c failed:\n" + r.stderr)
            return 1
        r = subprocess.run([gcc, "-O3", "-std=c99", "-fno-strict-aliasing", "-w",
                            "-c", c, "-o", os.devnull, "-fopt-info-vec-optimized"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print("FAIL check-numvec-vectorized -- gcc failed:\n" + r.stderr[-2000:])
            return 1
        src = open(c).read().split("\n")
    # each static function's line range in the emitted C
    spans = {}
    cur = None
    for i, line in enumerate(src, 1):
        m = re.match(r"^static [^;=]*?\b([A-Za-z_]\w*)\([^;]*\)\s*\{\s*$", line)
        if m:
            cur = m.group(1)
            spans[cur] = [i, None]
        elif line == "}" and cur and spans[cur][1] is None:
            spans[cur][1] = i
            cur = None
    vectorized = set()
    for line in r.stderr.split("\n"):
        m = re.match(r".*probe\.c:(\d+):\d+: optimized: loop vectorized", line)
        if not m:
            continue
        ln = int(m.group(1))
        for fn, (lo, hi) in spans.items():
            if hi and lo <= ln <= hi:
                vectorized.add(fn)
    fails = 0
    for name in EXPECTED:
        cn = mangle(name)
        if cn not in spans:
            print("FAIL %s -- not emitted (is it still called in %s?)" % (name, PROBE))
            fails += 1
        elif cn in vectorized:
            print("PASS %s" % name)
        else:
            print("FAIL %s -- its loop was not vectorized at -O3" % name)
            fails += 1
    print("check-numvec-vectorized: %d of %d vectorized" % (len(EXPECTED) - fails, len(EXPECTED)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
