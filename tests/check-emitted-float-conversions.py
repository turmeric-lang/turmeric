#!/usr/bin/env python3
"""tests/check-emitted-float-conversions.py -- find float<->integer VALUE
conversions in emitted C that nobody asked for.

Why this exists
---------------
The longest-running silent-wrong-answer family in this repo is a double that
crosses an int64 word slot by VALUE conversion where its BITS were needed:
7.1 arrives as 7, or 7.1 is read back as 3.45e-323.  docs/archive holds dozens
of instances, found one seam at a time (effect slots, generator yield, async
await, session payloads, dict clones, carrier shims, closure env capture,
union inject, ...).  Each was found by a generator that happened to reach its
SHAPE; the next one is always in a shape nobody generated.

`-Wfloat-conversion` (ratcheted in tests/run.sh) catches the IMPLICIT half.
It is blind to the other half, because the emitter spells a carrier crossing
as an EXPLICIT cast -- `(int64_t)(d)` -- and an explicit cast never warns.
But clang's AST records every conversion, explicit or not, as a
`FloatingToIntegral` / `IntegralToFloating` cast.  In emitted program code
there are exactly two legitimate sources of one:

  * a conversion the program ASKED for -- `(as int x)` / `(as float n)` --
    which the emitter spells `TUR_AS(T, x)`, a macro defined in the runtime
    preamble, so clang attributes the cast to the preamble line; and
  * an integer LITERAL converted to a float type (`((double)0)`), which is
    exact.

Hand-written inline-C is skipped -- a `defn` body named in the trailing
`/* tur:inline-c-fns: ... */` list, and a file-scope block between
`/* tur:inline-c-begin/end */` -- because a conversion an author wrote is
reviewed as code.

Everything else is a representation decision that converted a value.  This
tool reports each one with its C line, so the check does not depend on any
generator reaching the shape: it looks at the mechanism.

Usage
-----
    python3 tests/check-emitted-float-conversions.py FILE.tur [...]
    python3 tests/check-emitted-float-conversions.py --corpus [--jobs N]
    python3 tests/check-emitted-float-conversions.py --c FILE.c   # already emitted

Exit 1 if any finding, 0 if clean, 0 with an UNAVAILABLE line if clang is
missing (the check says so; it never passes silently).

`--shard i/N` (or $TUR_FCONV_SHARD) runs a round-robin slice of `--corpus`,
1-based; unset runs everything.  Every finding is independent of every other --
this is a per-program lint with no cross-program state and no baseline -- so a
shard reports exactly the findings in its own slice.  NOT $TUR_TEST_SHARD; see
tests/shard_util.py for why each script gets its own variable.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fconv_lint import (MARK, find_clang, lint_c)  # noqa: E402
from shard_util import parse_shard, shard_label, shard_slice  # noqa: E402


def lint_tur(path, tur, clang, workdir):
    # A UNIQUE file per input: inputs are linted concurrently, and two from
    # one directory once shared a name, so one program was linted as the
    # other's C (or not at all) and reported clean.
    fd, cfile = tempfile.mkstemp(suffix=".c", dir=workdir)
    os.close(fd)
    env = dict(os.environ)
    env.pop("TUR_STDLIB_DIR", None)
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    r = subprocess.run([tur, "emit-c", path], capture_output=True, text=True,
                       cwd=REPO, env=env, timeout=120)
    if r.returncode != 0 or MARK not in r.stdout:
        return path, None       # not a program this check applies to
    with open(cfile, "w") as f:
        f.write(r.stdout)
    try:
        return path, lint_c(cfile, clang)
    finally:
        try:
            os.unlink(cfile)
        except OSError:
            pass


def corpus_inputs():
    root = os.path.join(REPO, "tests", "fixtures")
    for d in sorted(os.listdir(root)):
        full = os.path.join(root, d)
        if d == "errors" or not os.path.isdir(full):
            continue
        if not os.path.exists(os.path.join(full, "expected.stdout")):
            continue
        # Fixtures the compiled suite does not run are not this check's
        # business either.
        if any(os.path.exists(os.path.join(full, m)) for m in
               ("requires.interp-only", "requires.dedicated-runner",
                "requires.spices", "requires.tsan")):
            continue
        inp = os.path.join(full, "input.tur")
        if not os.path.exists(inp):
            inp = os.path.join(full, d + ".tur")
        if os.path.exists(inp):
            yield inp


SELF_TEST_C = """#include <stdint.h>
#define TUR_AS(T, x) ((T)(x))
/* ==== tur: end of fixed runtime preamble ==== */
static int64_t carry(double d) { return (int64_t)(d); }          /* FINDING */
static double back(int64_t w) { return w; }                      /* FINDING */
static int64_t asked(double d) { return TUR_AS(int64_t, d); }    /* deliberate */
static double lit(void) { return ((double)0) + (double)(1); }    /* exact literals */
static double hand(int64_t n) {
    return (double)n;                                           /* hand-written */
}
/* tur:inline-c-begin */
static double block(int64_t n) { return (double)n; }            /* file-scope C */
/* tur:inline-c-end */
/* tur:inline-c-fns: hand */
"""


def self_test(clang):
    """A detector that matches nothing looks exactly like a clean corpus
    (docs/archive/emitted-c-pointer-integer-warnings-unwatched.md).  Prove
    this one fires on the two accidental conversions and on nothing else."""
    d = tempfile.mkdtemp(prefix="fconv-self-")
    try:
        f = os.path.join(d, "canary.c")
        with open(f, "w") as fh:
            fh.write(SELF_TEST_C)
        hits = lint_c(f, clang)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    got = sorted((k, h) for (_l, k, h, _t) in hits)
    want = [("F2I", "explicit"), ("I2F", "implicit")]
    ok = got == want
    print("check-emitted-float-conversions: self-test %s (want %s, got %s)"
          % ("PASS" if ok else "FAIL", want, got))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true",
                    help="prove the detector fires (run before --corpus)")
    ap.add_argument("files", nargs="*")
    ap.add_argument("--corpus", action="store_true")
    ap.add_argument("--c", action="store_true",
                    help="inputs are already-emitted C files")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
    ap.add_argument("--tur", default=os.path.join(REPO, "build", "tur"))
    ap.add_argument("--shard", default=os.environ.get("TUR_FCONV_SHARD"),
                    help='run a round-robin slice of --corpus, as "i/N" '
                         '(default: $TUR_FCONV_SHARD; unset runs everything)')
    args = ap.parse_args()

    # Not $TUR_TEST_SHARD: collect-suite-timings.py tags timing rows from the
    # job's environment rather than per test, so that variable in a job running
    # other suites mislabels their rows too.  See tests/shard_util.py.
    shard_index, shard_total = parse_shard(args.shard)

    clang = find_clang()
    if not clang:
        print("check-emitted-float-conversions: UNAVAILABLE -- no clang; "
              "float<->int value conversions are NOT being checked")
        return 0

    if args.self_test:
        if not self_test(clang):
            return 1
        if not (args.corpus or args.files):
            return 0

    if args.c:
        findings = [(f, lint_c(f, clang)) for f in args.files]
    else:
        # The shard slices the CORPUS enumeration only.  An explicit file list
        # is something a caller typed and expects to be honoured whole; a
        # sharded `--shard 1/4 a.tur b.tur c.tur d.tur` silently linting one of
        # the four would be a trap, not a feature.
        if args.corpus:
            inputs = shard_slice(list(corpus_inputs()), shard_index, shard_total)
        else:
            inputs = args.files
        workdir = tempfile.mkdtemp(prefix="fconv-")
        try:
            with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                findings = list(pool.map(
                    lambda p: lint_tur(p, args.tur, clang, workdir), inputs))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)

    n = 0
    checked = 0
    for path, hits in findings:
        if hits is None:
            continue
        checked += 1
        for (line, kind, how, text) in hits:
            n += 1
            print("FINDING %s -- C line %d: %s %s float<->int value conversion"
                  % (os.path.relpath(path, REPO), line, how, kind))
            print("    %s" % text)
    print("check-emitted-float-conversions: %d program(s) checked%s, %d finding(s)"
          % (checked, shard_label(shard_index, shard_total), n))
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
