# MIR's x86-64 generator miscompiles a long double read back through a union

**RESOLVED 2026-10-10** in the fork (turmeric-lang/mir `d81ebcc`,
turmeric-lang/mir#9) and re-synced into `external/mir/` -- see *Resolution*
at the end.  The title was a guess: the cause is in c2mir's front end, which
told the generator the two accesses could not alias; the generator only
believed it.

**Severity: medium -- a silent wrong answer, in a narrow shape.**  Storing a
`long double` into a union and reading the same bytes back as integers gives 0
in code MIR's x86-64 generator compiles; MIR's interpreter gets it right.  User
inline C doing that under `tur jit` gets wrong bits with no diagnostic.  It also
breaks MIR's own self-hosting quietly: `put_ldouble` in `external/mir/mir.c` is
exactly this shape, so a c2mir that c2mir compiled writes every non-zero long
double constant into binary MIR as 0.

Found 2026-10-10 while executing
[c2mir-rejects-uint128](../archive/c2mir-rejects-uint128.md), when the fork's
`c2mir-bootstrap-test` failed on the new long double literals in c2mir.c.  The
fix there keeps c2mir.c free of such literals (`ld_pow2`); the generator bug is
untouched.

## Repro

On the fork's `c2m` (x86-64), base `3c0d8c84` and `d9f75585` alike:

```c
#include <stdio.h>
#include <stdint.h>
static void put_ldouble (long double ld) {
  union { uint64_t u[2]; long double ld; } u;
  u.u[0] = u.u[1] = 0;
  u.ld = ld;
  u.u[1] &= 0xffffULL;
  printf ("%016llx %016llx\n", (unsigned long long) u.u[0], (unsigned long long) u.u[1]);
}
int main (void) { put_ldouble (18446744073709551616.0L); return 0; }
```

```sh
gcc ld.c && ./a.out   # 8000000000000000 000000000000403f
c2m ld.c -ei          # 8000000000000000 000000000000403f
c2m ld.c -eg          # 0000000000000000 0000000000000000   (-el the same)
```

The bootstrap consequence, from the fork's tree:

```sh
./c2m -w -DMIR_BOOTSTRAP -I. mir-gen.c c2mir/c2mir.c c2mir/c2mir-driver.c mir.c -o 1.bmir
./c2m -DMIR_BOOTSTRAP 1.bmir -el -w -DMIR_BOOTSTRAP -I. c2mir/c2mir-driver.c t.c -o 2.bmir
# t.c: long double f (void) { return 9223372036854775808.0L; }
./c2m 2.bmir -S -o 2.mir    # f returns 0.0L
```

## Root cause

Not yet localized.  The interpreter being right puts it in `mir-gen.c` or
`mir-gen-x86_64.c`, not the front end.  Suspects: the 80-bit `fstpt` store
into a slot that is then read as two 64-bit words -- alias information
(c2mir gives union members the union's alias) or a generator pass that keeps
the union in registers by member type, losing the type pun.  `-O0` is worth
trying to split optimization from instruction selection.

## Fix directions

- Reduce the repro with `c2m -dg` and compare the MIR before and after
  generation; then fix in the fork with a `c-tests/new` test and re-sync.
- Once fixed, c2mir.c may carry long double literals again (`ld_pow2` in it
  says why it exists), and the bootstrap tests would catch a regression.

## Resolution

Root cause, found with `c2m -S` on the repro: c2mir gives a union member access
the union's alias (`N_FIELD` keeps a `U...` alias), but an element of an array
member -- `u.u[0]` -- was tagged with its element type's alias (`l`), because
`N_IND` first turns the array object into an address and then builds the
element's memory operand from scratch.  So the store `ldmov ld:(fp):UAlDe` and
the load `u64:(fp, i, 8):l` carried different aliases, and MIR-gen's alias
analysis (rightly, given what it was told) let the load see the memory from
before the store.  The interpreter ignores aliases, which is why `-ei` was
right; every generating target was affected, not only x86-64.

Fixed in the fork, commit `d81ebcc` ("c2mir: keep the union's alias on an
element of an array member", turmeric-lang/mir#9, stacked on #8): `N_IND`
generates an array object as an lvalue first and, when its memory operand
carries a union alias, gives the element that alias.  With the cause gone, the
`__int128` code's `ld_pow2` workaround is removed and c2mir.c carries its long
double literals again; the bootstrap tests, which failed on exactly those
literals, pass, and the self-hosted repro now returns `9.22...e+18L`, not 0.

Covered in the fork by `c-tests/new/union-array-member-alias.c` (double and
float puns through word and byte arrays both ways, a long double round trip,
and a pun through an array in a struct in a union behind a pointer), and here
by `tests/fixtures/jit-inline-c-union-array-pun`, which printed `0` for a
double read back from its words, and failed the long double round trip,
under `tur jit` before the fix.
