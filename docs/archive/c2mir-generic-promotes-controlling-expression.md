# c2mir promotes a `_Generic` controlling expression, so a sub-int type selects `int`

> **Resolved 2026-10-28** by turmeric-lang/mir#16 (`f887547f`, vendored the
> same day): the controlling expression gets lvalue conversion only. The fix
> exposed a second c2mir bug the promotion had hidden -- a character constant
> was typed `char`, so `sizeof ('a')` was 1 -- fixed in the same PR.
> `c-tests/new/generic-no-promotion.c` covers both; gcc and clang agree with
> its `.expect`.

**Severity:** low. Wrong answer with no diagnostic, but turmeric's emitter never
writes `_Generic` (`src/compiler/mangle.c` only lists it as a reserved word), so
only user inline C that switches on `char`, `short` or a bit-field type sees it,
under `tur jit` and the planned browser JIT alike. Every c2mir target is
affected, not only wasm32.

Filed 2026-10-27, found writing the `_Generic` type checks for
turmeric-lang/mir#15 (wasm-jit-plan section 6). That test goes through pointer
types instead, which c2mir handles correctly.

## Repro

With the fork's `c2m` (`make c2m` in a turmeric-lang/mir checkout), against a
native `cc`:

```c
#include <stdio.h>
int main (void) {
  printf ("%d %d %d\n",
          _Generic ((short) 0, short: 1, int: 2, default: 0),
          _Generic ((signed char) 0, signed char: 1, int: 2, default: 0),
          _Generic ((short *) 0, short *: 1, default: 0));
  return 0;
}
```

| compiler | output |
| --- | --- |
| `cc` (gcc / clang) | `1 1 1` |
| `c2m -eg`, x86-64 Linux | `2 2 1` |

C11 6.5.1.1p2 applies only lvalue conversion to the controlling expression:
qualifiers drop, arrays and functions decay, and the integer promotions do not
happen. C17 says so outright (DR 481). gcc and clang agree.

## Root cause

`external/mir/c2mir/c2mir.c:10017`, in `check`'s `N_GENERIC` case:

```c
t = *e1->type;
if (integer_type_p (&t)) t = integer_promotion (&t);
```

The promoted `t` is what each association's type is matched against.

## Fix directions

- Replace the promotion with lvalue conversion: drop top-level qualifiers, and
  turn an array into a pointer to its element and a function into a pointer to
  it. Check whether c2mir already has a helper for that (the unary-operand
  path does the decays).
- Add a c-test with the three cases above, plus a `const int` controlling
  expression, which must select `int`.
- It is a fork change, so it rides the next turmeric-lang/mir PR (the
  one-at-a-time rule in CLAUDE.md), which wasm-jit-plan section 6 lists.
