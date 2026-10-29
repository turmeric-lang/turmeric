#!/usr/bin/env python3
"""check-numvec-vectorized.py -- the numvec loops that should vectorize, do.

simd-and-vectorization-plan N1 and N2.  The N1 kernels of stdlib/numvec.tur
are written so gcc vectorizes them at -O3 (no control flow in the loop, a fixed
eight-lane order for float reductions), and N2 lets a loop a program writes
itself over an array vectorize (unchecked reads, no panic test after a leaf
inline-C call).  Both properties are invisible in a program's output: a later
preamble or emitter change could quietly put control flow back in a loop and
every fixture would still pass.  This compiles tests/vectorize/numvec-probe.tur
with gcc and `-fopt-info-vec-optimized`, twice:

  -O2  tur's default build.  gcc's -O2 cost model vectorizes no loop whose
       trip count it cannot see, so the kernels write their eight-wide body
       out and its basic-block vectorizer takes it ("basic block part
       vectorized").  BOTH must be vectorized here.
  -O3  `--release`, the loop vectorizer.  BOTH, O3_ONLY and the N2 probes.

A report counts for a kernel only when its line falls inside the kernel's main
loop -- from `for (; i + 8 <=` up to the scalar tail (`for (; i < n`, or
`if (i < n)` where the tail is written out) -- so
setup code vectorized around a scalar loop does not pass for it.

Which loops vectorize depends on the instruction set, so the list is for
x86-64's baseline (SSE2); elsewhere, or without gcc, this skips.  Not listed,
on purpose: `-copy!` (a memmove), a u8 `-fill!` (gcc turns it into memset),
`-sum-ordered` (the strict left fold), the 64-bit integer multiplies and
min/max that SSE2 has no vector instruction for, and float min/max: gcc
vectorizes no `x < m ? x : m` reduction without -ffast-math, and the kernel's
eight scalar chains beat the spelling it did vectorize (lanes in an array, an
int NaN flag per lane) by a third.

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

# Vectorized at both -O2 and -O3.
FLOAT_KERNELS = ["fill!", "scale!", "add!", "axpy!", "sum", "dot"]
BOTH = (
    ["f64vec-" + k for k in FLOAT_KERNELS]
    + ["f32vec-" + k for k in FLOAT_KERNELS]
    + ["i64vec-" + k for k in ["fill!", "add!", "sum"]]
    + ["i32vec-" + k for k in ["fill!", "scale!", "add!", "axpy!", "sum", "min", "max"]]
    + ["u8vec-add!"]
)
# -O2's basic-block vectorizer finds these unprofitable on SSE2: eight bytes
# half-fill a register, and there is no 64-bit integer multiply.  The u8 sum
# is still 2.4x faster at -O2 than a plain loop (its 32-bit lanes).
O3_ONLY = ["i64vec-axpy!", "u8vec-sum", "u8vec-min", "u8vec-max"]
PROBES = ["probe-int-fold", "probe-int-for", "probe-float-partials"]


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
        reports = {}
        for level in ("-O2", "-O3"):
            r = subprocess.run([gcc, level, "-std=c99", "-fno-strict-aliasing", "-w",
                                "-c", c, "-o", os.devnull, "-fopt-info-vec-optimized"],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print("FAIL check-numvec-vectorized -- gcc %s failed:\n%s" % (level, r.stderr[-2000:]))
                return 1
            reports[level] = r.stderr
        src = open(c).read().split("\n")
    # each static function's line range in the emitted C, and its main loop's
    spans, loops = {}, {}
    cur = None
    for i, line in enumerate(src, 1):
        m = re.match(r"^static [^;=]*?\b([A-Za-z_]\w*)\([^;]*\)\s*\{\s*$", line)
        if m:
            cur = m.group(1)
            spans[cur] = [i, None]
        elif cur and line == "}" and spans[cur][1] is None:
            spans[cur][1] = i
            cur = None
        elif cur:
            if "for (; i + 8 <=" in line and cur not in loops:
                loops[cur] = [i, None]
            elif ("for (; i < n" in line or "if (i < n)" in line) \
                    and cur in loops and loops[cur][1] is None:
                loops[cur][1] = i
    def vectorized(level, where):
        """where: "function" (anywhere in it), "loop" (inside its main
        loop), or "head" (the main loop itself, at its `for` line)."""
        hit = set()
        for line in reports[level].split("\n"):
            m = re.match(r".*probe\.c:(\d+):\d+: optimized: "
                         r"(loop vectorized|basic block part vectorized)", line)
            if not m:
                continue
            ln = int(m.group(1))
            for fn, (lo, hi) in (spans if where == "function" else loops).items():
                if hi and (ln == lo if where == "head" else lo <= ln < hi):
                    hit.add(fn)
        return hit
    fails = total = 0
    # At -O2 the main loop itself must be vectorized (reported at its own
    # `for` line).  An inner `for (int k = 0; k < 8; k++)` lane loop -- the
    # old spelling -- is ALSO "loop vectorized" there, its trip count being
    # the constant 8, yet runs at a quarter of the speed; it is reported at
    # the inner loop's line, so it does not pass.
    for level, names, where in (("-O2", BOTH, "head"),
                                ("-O3", BOTH + O3_ONLY, "loop"),
                                ("-O3", PROBES, "function")):
        got = vectorized(level, where)
        whole = where == "function"
        for name in names:
            total += 1
            cn = mangle(name)
            if cn not in spans:
                print("FAIL %s %s -- not emitted (is it still called in %s?)" % (level, name, PROBE))
                fails += 1
            elif not whole and cn not in loops:
                print("FAIL %s %s -- no `for (; i + 8 <=` main loop found" % (level, name))
                fails += 1
            elif cn in got:
                print("PASS %s %s" % (level, name))
            else:
                print("FAIL %s %s -- its main loop was not vectorized" % (level, name))
                fails += 1
    print("check-numvec-vectorized: %d of %d vectorized" % (total - fails, total))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
