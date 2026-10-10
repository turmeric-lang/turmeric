# c2mir cannot parse `__uint128_t`, so inline C using it never reaches the JIT

**RESOLVED 2026-10-10** in the fork (turmeric-lang/mir `d9f75585`, branch
`claude/c2mir-rejects-uint128-hmemc3`) and re-synced into `external/mir/` --
see *Resolution* at the end.  c2mir now has `__int128` and `unsigned __int128`
as real types and lowers every operation on them to 64-bit MIR, so the repro
below runs under the engine with no `TUR-W0070`.

**Narrowed 2026-10-03: the aarch64 system-header half is fixed** -- the fork
declares the 16-aligned stand-in on Linux aarch64 too, and glibc's
`ucontext_t` now has gcc's layout there
([jit-xopen-source-guard-inert-on-glibc](../archive/jit-xopen-source-guard-inert-on-glibc.md),
turmeric-lang/mir#6).  What is left is the report's own subject: user inline C
that does ARITHMETIC on `__uint128_t` -- undeclared on x86-64, and a
layout-only struct on aarch64, so the repro below still falls back to cc on
both.  The `sys/user.h` baseline noise described under *Repro* is gone.

**Severity: medium.** `tur jit` declines any program whose inline C writes
`__uint128_t`: the engine's parse fails, `TUR-W0070` fires and the cc path takes
over, so the answer is still right -- just compiled the slow way, with a warning,
and with the engine silently unused for that program. On aarch64 the same gap
also makes a *system* header unparseable, which is the mechanism behind
[jit-xopen-source-guard-inert-on-glibc](../archive/jit-xopen-source-guard-inert-on-glibc.md)
and what stranded the v0.60.0 release (since fixed).

Found 2026-10-03 while diagnosing that release failure.

## Repro

On any host with the JIT engine (measured on `ubuntu-24.04-arm`, glibc 2.39,
gcc 13):

```turmeric
(defn widen [x : int] : int
  ```c
  __uint128_t v = (__uint128_t)x;
  v = v * 3u;
  return (int64_t)v;
  ```)

(defn main [] : int
  (println (widen 14))
  0)
```

```sh
./build/tur jit u128.tur
# <tur-jit>:2539:21: syntax error on identifier (expected ';'):
# <tur-jit>:2539:21: syntax error on identifier (expected '<statement>'):
# <tur-jit>:2545:1:  syntax error on int (expected '<statement>'):
# <tur-jit>:2538:33: unfinished compound statement
# tur: warning: TUR-W0070: jit engine could not compile this program; falling back to the cc path
# 42
```

The answer (42) is correct -- that is the cc path. `TUR_JIT_DUMP_C=<path>` writes
the exact text handed to c2mir, and a `<tur-jit>:LINE` maps into that file
(`src/main.c:4962`).

**Read the evidence differentially.** On aarch64 every `tur jit` run also prints
a `sys/user.h:30` error, including a plain hello-world, so that line is baseline
noise here and says nothing about this program. The four `<tur-jit>:2539`-area
errors above are what no other program produces, and they sit exactly at the
inline-C body. On a platform without the header problem the baseline is quiet and
these are the only errors.

## Root cause

`__uint128_t` is a builtin type name, so c2mir knows it only where a target's
predefined header (`external/mir/c2mir/<arch>/mirc_<arch>_linux.h`) declares
it.  Read 2026-10-03:

- **x86-64:** not declared at all -- the repro's `__uint128_t v = ...` is
  `<identifier> <identifier>`, the "expected ';'" above.
- **aarch64, Apple:** declared as a LAYOUT-ONLY stand-in,
  `typedef struct {_Alignas(16) unsigned long hi; unsigned long lo;}
  __uint128_t;` (fork commit `90633091` fixed its alignment), so SDK headers
  that embed it in structs parse with the right size -- but it is a struct,
  so `(__uint128_t)x` and `v * 3u` still fail.
- **aarch64, Linux:** the stand-in sat inside the `#elif defined(__APPLE__)`
  branch, so the name was undeclared -- the whole of
  [jit-xopen-source-guard-inert-on-glibc](../archive/jit-xopen-source-guard-inert-on-glibc.md).
  Fixed 2026-10-03: declared for every aarch64 OS, same layout-only struct as
  Apple.

MIR itself has no 128-bit integer type, so ARITHMETIC on `__uint128_t` in
inline C is a real frontend-plus-backend feature (lowering to pairs of 64-bit
operations), not a parser fix.  Declaring the stand-in for Linux aarch64 is the
cheap, separate fix the header problem needs.

## Fix directions

- The change belongs in the vendored fork under `external/mir/`, not in this
  repo's sources. `external/mir/VENDORED.md` says how to change MIR and
  `tools/update-mir.sh` re-syncs the copy; never edit `external/mir/` by hand.
- Start from the prior-art report above -- if layout and alignment are already
  handled, the gap may be confined to the c2mir parser accepting the type
  specifier.
- A fixture belongs with the fix: the program above under `tests/fixtures/`,
  exercised on the JIT path, so the gap cannot silently reopen. Note that a
  fixture asserting only *output* would pass today via the cc fallback -- it has
  to assert the absence of `TUR-W0070`, the way `release.yml`'s archive JIT check
  does.
- The aarch64 header consequence no longer depends on this: it was fixed on
  its own (the sibling report, now archived).

## Resolution

Fixed in the fork as the report's own subject asked -- a frontend-plus-backend
feature, not a parser fix -- in one commit, `d9f75585` ("c2mir: support
__int128 and unsigned __int128").  Pinned here at that branch commit; move the
pin to its merge commit once it is merged into the fork's `master`, as
`VENDORED.md` describes.

- **Front end.** `__int128` is a keyword that combines with `signed` and
  `unsigned` the way `int` does, and `mirc.h` predefines `__int128_t`,
  `__uint128_t` and `__SIZEOF_INT128__` for every target.  That replaces the
  aarch64 layout-only struct with the real 16-byte, 16-aligned type, so the
  `<sys/user.h>` and Darwin signal-context layouts the sibling report needed
  are unchanged (`c-tests/new/aarch64-linux-uint128-user-h.c` still checks
  them).  Constants fold in two 64-bit halves, so static data such as
  `((__uint128_t)hi << 64) | lo` works.
- **Generator.** An `__int128` lvalue is a 16-byte memory operand and an rvalue
  a pair of 64-bit temporaries.  Every operator is plain 64-bit MIR with no
  runtime library: add/sub with carry, the full 64x64 product from 32-bit
  halves, branching shifts that never shift by 64 or more, hardware division
  when both operands fit in 64 bits and shift-subtract otherwise, two-word
  comparisons, and correctly rounded conversions to and from float, double and
  long double.
- **Calls.** An `__int128` argument or result travels as two 64-bit ones.
  JIT code always agrees with itself, and with native code on a result and on
  an argument passed in registers -- except where the native ABI moves one
  (AAPCS64 starts it at an even register; x86-64 puts it wholly on the stack
  when only one register is left).  Turmeric never passes `__int128` across
  the native boundary: the runtime has no such function.
- **Rejected with a diagnostic, so `tur jit` falls back to cc rather than
  miscompiling:** `__int128` bit-fields, `switch` on one, `va_arg` of one or
  passing one with no parameter (variadic or unprototyped), an `__int128` enum
  base, asm register variables, and `__int128` operands of
  `__builtin_*_overflow`.

**Verification.**  The fork's new `c-tests/new/int128-ops.c` checksums every
operator over a table of edge values, at run time and constant-folded, and
`int128-misc.c` covers folding, calls, struct members, compound assignment with
mixed types and the float conversions; their expected output is gcc's, and
aarch64 gcc produces the identical files.  On x86-64 the whole c2mir suite
(1092 tests) passes with `-ei`, `-eg`, `-O0` and `-O3`, and the bootstrap tests
pass -- they now exercise the lowering for real, because `mir-hash.h` takes its
`__uint128_t` path inside the self-compiled c2mir.  A cross-built aarch64 `c2m`
under qemu-user passes both new tests with `-ei`, `-eg` and `-O3`, and the suite
apart from two x86-only tests that qemu's host `uname -m` let through.  Here,
`tests/fixtures/jit-uint128-arith` is the repro plus a high-half multiply,
128-bit division by 10, signed division and a folded constant; it is not in
`tests/jit-fallback-baseline.txt`, so `tests/run-jit.sh` fails if it ever falls
back again.

Two pre-existing c2mir/MIR bugs turned up on the way and are filed rather than
fixed there (both since resolved): [c2mir-bool-conversion-truncates](c2mir-bool-conversion-truncates.md)
and [mir-x86-64-long-double-union-pun](mir-x86-64-long-double-union-pun.md).
