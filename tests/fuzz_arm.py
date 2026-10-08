"""tests/fuzz_arm.py -- arm the function-pointer type detector for a fuzzer run.

Why this exists
---------------
The recurring defect class these fuzzers hunt -- a value crossing a seam in
the emitted C at the wrong representation -- has a sub-family no model oracle
can be relied on to see: an indirect call through a function pointer whose
type disagrees with the callee's definition.  The emitter spells those calls
with an explicit cast (`((int64_t(*)(void*, int64_t))g.fn)(g.env, x)`), so cc
never warns, and when the callee really returns a 16-byte tagged `any` the
caller silently keeps the first word -- the TYPE TAG -- and prints `4` for
`2.5` (saffron-dyn-witness-fn-arity-defaults-unary, 2026-09-30).  That
program sat outside every generator's shape space, which is how each previous
member of the class was missed too: the generators enumerate shapes, and the
next defect is always in a shape nobody enumerated.

clang's `-fsanitize=function` checks every indirect call against the callee's
real signature, WHATEVER shape produced it.  Trap mode (`-fsanitize-trap=`)
needs no UBSan runtime -- the container toolchains this repo runs on ship
clang without one -- and turns a mismatch into SIGILL (exit 132 through
`tur run`), which the fuzzers classify as `BUG_fnptr_trap` (see TRAP_CLASS).

GCC has no equivalent, so this is clang-only.  When no suitable clang is
found the run proceeds unarmed and SAYS SO; it never pretends.

Proving it is armed
-------------------
A detector that silently matches nothing looks exactly like a clean run --
the lesson of docs/archive/emitted-c-pointer-integer-warnings-unwatched.md and
of the float-conversion ratchet's two false-clean disarm checks.  So arming
compiles a canary that makes one MISMATCHED indirect call and one MATCHED
one, and requires the first to trap and the second to run.  Only then is the
environment changed.

Opt out with TUR_FUZZ_FNSAN=0.  Force a specific compiler with CC=<clang>.
"""

import os
import shutil
import subprocess
import tempfile

FNSAN_FLAGS = "-fsanitize=function -fsanitize-trap=function"
# tur's own default when TUR_CC_FLAGS is unset (src/main.c); TUR_CC_FLAGS
# REPLACES it, so the armed value must restate it.
TUR_DEFAULT_CC_FLAGS = "-O2 -std=c99 -Wall -fno-strict-aliasing"

# Exit status `tur run` reports for a child killed by the sanitizer's trap.
# clang lowers `-fsanitize-trap=function` per target: `ud2` on x86-64, which
# raises SIGILL (128 + 4), and `brk` on arm64, which raises SIGTRAP (128 + 5).
# Both are the same trap, so both classify as one.
#
# Only SIGILL was listed here, while `_probe` below already accepted either
# signal -- so arming SUCCEEDED on arm64 ("canary trapped") while a real
# mismatch was then misclassified: 133 missed this arm, missed the
# `(134, 138, 139)` crash arm after it, and landed in the harness's generic
# bucket as BUG_toolchain_other. Measured on macOS arm64 against
# Homebrew clang: seed 20261003 case 376 of type-fuzz-src, which the Linux
# leg of the same seed reports as BUG_fnptr_trap.
FNSAN_TRAP_RCS = (132, 133)


def is_fnsan_trap(rc):
    """True iff `rc` is how a child killed by the fnsan trap is reported."""
    return rc in FNSAN_TRAP_RCS

# How a trap is classified.  The detector is exact -- it traps on `bool` vs
# `int64_t` and on `char *` vs `int64_t` as readily as on `double` vs
# `int64_t` -- so while the emitter still had ABI-benign mismatches of the
# first two kinds, failing a run on every trap would have made these harnesses
# red on noise.  A trap was therefore the report-only class FNPTR_TRAP until
# the corpus sweep reached zero (2026-10-02, see
# docs/archive/emitted-c-indirect-calls-are-not-type-exact.md).  It is a
# failing BUG_fnptr_trap now; TUR_FUZZ_FNSAN_STRICT=0 restores report-only
# for a session that is chasing something else.
FNSAN_STRICT = os.environ.get("TUR_FUZZ_FNSAN_STRICT", "1") != "0"
TRAP_CLASS = "BUG_fnptr_trap" if FNSAN_STRICT else "FNPTR_TRAP"

_CANARY = r"""
typedef long (*lfn)(long);
typedef double (*dfn)(double);
static long twice(long x) { return 2 * x; }
int main(int argc, char **argv) {
    (void)argv;
    lfn good = twice;
    if (good(21) != 42) return 3;
    if (argc > 1) {                 /* the mismatched call, on request */
        dfn bad = (dfn)(void *)twice;
        return (int)bad(1.5);
    }
    return 0;
}
"""


def _candidates(env):
    cc = env.get("CC", "").strip()
    if cc:
        return [cc]
    out = []
    for name in ("clang", "clang-18", "clang-19", "clang-20", "clang-21",
                 "clang-17"):
        p = shutil.which(name)
        if p and p not in out:
            out.append(p)
    return out


def _probe(cc):
    """True iff `cc` builds the canary with the sanitizer, the matched call
    runs, and the mismatched call traps."""
    d = tempfile.mkdtemp(prefix="fnsan-canary-")
    try:
        src = os.path.join(d, "c.c")
        exe = os.path.join(d, "c")
        with open(src, "w") as f:
            f.write(_CANARY)
        try:
            b = subprocess.run([cc] + TUR_DEFAULT_CC_FLAGS.split() +
                               FNSAN_FLAGS.split() + [src, "-o", exe],
                               capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.TimeoutExpired):
            return False
        if b.returncode != 0:
            return False
        try:
            ok = subprocess.run([exe], capture_output=True, timeout=20)
            bad = subprocess.run([exe, "mismatch"], capture_output=True,
                                 timeout=20)
        except (OSError, subprocess.TimeoutExpired):
            return False
        # A trap is SIGILL (-4) or, on some libcs, SIGTRAP (-5).
        return ok.returncode == 0 and bad.returncode in (-4, -5)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def arm_fnsan(env):
    """Arm `env` (a dict passed to subprocess) for the function-pointer
    detector.  Returns a one-line status for the run's banner: armed with
    which compiler, disabled on request, or unavailable (and why)."""
    if env.get("TUR_FUZZ_FNSAN", "1") == "0":
        return "fnsan: disabled (TUR_FUZZ_FNSAN=0)"
    for cc in _candidates(env):
        if _probe(cc):
            env["CC"] = cc
            base = env.get("TUR_CC_FLAGS", "").strip() or TUR_DEFAULT_CC_FLAGS
            if FNSAN_FLAGS not in base:
                base = base + " " + FNSAN_FLAGS
            env["TUR_CC_FLAGS"] = base
            return "fnsan: ARMED (%s, canary trapped)" % cc
    tried = ", ".join(_candidates(env)) or "no clang on PATH"
    return ("fnsan: UNAVAILABLE -- mismatched function-pointer calls will NOT "
            "be detected (tried: %s)" % tried)


_ARMED_CACHE = {}


def armed_env(base_env):
    """`base_env` armed once per process (the canary is not free)."""
    key = (base_env.get("CC", ""), base_env.get("TUR_CC_FLAGS", ""),
           base_env.get("TUR_FUZZ_FNSAN", "1"))
    if key not in _ARMED_CACHE:
        env = dict(base_env)
        status = arm_fnsan(env)
        _ARMED_CACHE[key] = (env.get("CC"), env.get("TUR_CC_FLAGS"), status)
    cc, flags, status = _ARMED_CACHE[key]
    env = dict(base_env)
    if cc is not None:
        env["CC"] = cc
    if flags is not None:
        env["TUR_CC_FLAGS"] = flags
    return env, status
