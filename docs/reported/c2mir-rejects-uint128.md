# c2mir cannot parse `__uint128_t`, so inline C using it never reaches the JIT

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
