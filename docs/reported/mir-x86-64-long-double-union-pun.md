# MIR's x86-64 generator miscompiles a long double read back through a union

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
