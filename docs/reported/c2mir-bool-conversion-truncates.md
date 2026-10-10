# c2mir converts to `_Bool` by truncating to 8 bits, so `bool b = 256` is false under `tur jit`

**Severity: medium-high -- a silent wrong answer.**  Under the JIT engine,
converting a value wider than a byte to `_Bool`/`bool` keeps its low 8 bits
instead of testing it against zero.  Any inline C that returns, assigns or
passes a mask, a count or a double as a `bool` can get `false` where C (and the
cc path) says `true`.  No diagnostic; the cc path is unaffected, so the two
paths disagree on the same program.

Found 2026-10-10 while executing
[c2mir-rejects-uint128](../archive/c2mir-rejects-uint128.md): the `__int128`
conversion to `_Bool` there was written to test both halves, and checking it
against the existing conversions showed they do not test at all.

## Repro

Plain C, on the fork's `c2m` (`make c2m` in turmeric-lang/mir), at its base
commit `3c0d8c84` and after `d9f75585` alike:

```c
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
static bool ret_bool (int64_t x) { return x; }
static int take_bool (bool b) { return b; }
int main (void) {
  int64_t x = 256, m = 0x100;
  bool a = x;
  bool b = (bool) x;
  bool c = x & m;
  double d = 0.5;
  bool e = d;
  printf ("%d %d %d %d %d %d\n", a, b, c, e, ret_bool (512), take_bool (x));
  return 0;
}
```

```sh
gcc bool.c && ./a.out   # 1 1 1 1 1 1
c2m bool.c -eg          # 0 0 0 0 0 0
c2m bool.c -ei          # 0 0 0 0 0 0   (the interpreter too: it is the front end)
```

Through turmeric the same happens to an inline-C body declared `: bool` that
returns, say, `x & 256`.

## Root cause

`get_mir_type` maps `_Bool` to `MIR_T_U8`, and every implicit or explicit
conversion goes through `cast` (`external/mir/c2mir/c2mir.c`), which knows only
MIR types: to `MIR_T_U8` it emits `UEXT8` -- truncation -- and from a floating
type `F2I`/`D2I` first, so 0.5 becomes 0.  C11 6.3.1.2 requires the result to
be 0 when the value compares equal to 0 and 1 otherwise.  Constant folding is
wrong the same way: `cast_value` converts through `mir_bool`, which the target
headers (`c2mir/<arch>/c<arch>.h`) define as `uint8_t`, so `(bool) 256` folds to
0 as well.

## Fix directions

- The conversion sites already learned the C types in the `__int128` change:
  `i128_conv_if` is called wherever a value of one C type is about to be cast
  to another (assignment, initializers, casts, call arguments, `return`, `?:`,
  compound assignment).  Generalize it into one C-type-aware conversion that
  also handles a `_Bool` target from any non-`_Bool` scalar: `NE`/`FNE`/`DNE`/
  `LDNE` against zero, the way `i128_conv` already does for `__int128`.
- In `cast_value`, convert a `TP_BOOL` target with `!= 0` instead of through
  `mir_bool` (the `__int128` source already does: `cast_value_i128`).
- Do it in the fork with a `c-tests/new` test like the repro, then re-sync
  (`external/mir/VENDORED.md`).  A turmeric fixture whose inline C returns
  `(x & 256)` as `: bool` would pin it on the JIT path; `tests/run-jit.sh`
  compares output, so a wrong `false` fails it.
