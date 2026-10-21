# Integer `mod` by zero, and `INT64_MIN` divided by -1, crash instead of failing cleanly

**Severity: medium (a crash with no message; on the interpreter, the host
process dies).** Found 2026-10-19 probing the operators for their API reference
(`stdlib/forms/operators.tur`).

`/` on integers guards a zero divisor: the compiled program prints
`division by zero` and aborts, the interpreter returns `eval: division by zero`.
Three neighbouring cases have no guard and are undefined behaviour in C:

| Expression | Compiled (`tur run`, gcc 13) | Interpreted (`tur --interpret`, REPL, playground) |
| --- | --- | --- |
| `(mod a 0)` | `Illegal instruction`, exit 132, no message | `eval: modulo by zero` -- fine |
| `(/ INT64_MIN -1)` | `Illegal instruction`, exit 132 | **the `tur` process** dies with SIGFPE (ASan: `FPE ... eval_builtin eval.c:5267`) |
| `(mod INT64_MIN -1)` | printed `0` here -- by luck, it is UB | **the `tur` process** dies with SIGFPE (`eval.c:5323`) |

The interpreter rows are the worse ones: a user expression takes down the REPL
or the playground's libturi with it, not just the program.

## Repro

```turmeric
(defn r [a : int b : int] : int (mod a b))
(defn q [a : int b : int] : int (/ a b))
(println (r 7 (- (q 1 1) 1)))                          ; compiled: Illegal instruction
(println (q (- 0 9223372036854775807 1) -1))           ; interpreted: SIGFPE in tur
```

## Root cause

- `mod` is `BS_BIN_INFIX` with `c_op` `"%"` (`src/compiler/builtins.c:17`, and
  the sized-int rows at `:73`, `:91`), so `emit_builtin` writes a bare
  `(a) % (b)` (`src/compiler/emit_core.c`, `case BS_BIN_INFIX`). Only `/` is
  `BS_DIV_CHECK`, whose guard tests the divisor for zero and nothing else
  (`emit_core.c`, `case BS_DIV_CHECK`).
- `-fwrapv` (on since `emitted-c-signed-overflow-no-fwrapv`) makes `+ - *`
  wrap, but does not cover division: `INT64_MIN / -1` still traps on x86-64.
- The interpreter checks zero for both (`src/turi/eval.c:5266`, `:5322`) and
  then runs the C operator on `int64_t`.

## Fix directions

1. Give `mod` the `/` guard (a `BS_DIV_CHECK`-like shape with a
   `modulo by zero` message, matching the interpreter's text).
2. Define the `INT64_MIN, -1` case on both engines the way `-fwrapv` defines
   overflow: `/` gives `INT64_MIN`, `mod` gives `0` -- a test on the divisor
   being `-1` in the guard, or a `(uint64_t)` negation.
3. Pin both engines with a fixture (`/` and `mod`, zero and `-1`, int and a
   sized kind). Every emitted `%` changes, so regenerate snapshots in the same
   change.

## Resolution (2026-10-20)

Fix directions 1 and 2, on every route an integer `/` or `mod` takes. (With
literal operands the compiled `(q INT64_MIN -1)` printed the wrapped value
before the fix -- the C compiler had folded the call -- so the fixtures build
the divisor at run time.)

- **One helper per operation, defined once in the preamble**
  (`emit_module.c`, beside `TUR_AS`): `tur_idiv` and `tur_imod` for the signed
  kinds, `tur_umod` for the unsigned `mod`. Each aborts on a zero divisor
  (`division by zero` / `modulo by zero`, the interpreter's wording). The signed
  ones answer a divisor of -1 without dividing -- the quotient is the negation,
  wrapped the way `-fwrapv` wraps `+ - *` (`INT64_MIN` stays `INT64_MIN`), the
  remainder is 0. A helper, not inline text, so each operand appears once in
  the output: a chain of divisions would otherwise double per nesting level.
- **`builtin_int_divmod_helper`** (`builtins.c`) maps a builtin row to its
  helper, and the direct emitter (`emit_core.c`) and the CPS-IR emitter
  (`emit_cps_ir.c`, `prim_expr`) both ask it. The unsigned `/` row and the
  float rows keep the `BS_DIV_CHECK` text -- an unsigned divisor cannot be -1.
- **The other `/` spellings** use the same helper: the cloneable-continuation
  arithmetic frames (`cloneable_frame_expr`, `emit_cloneable_pure_arm`), the
  serial runtime's `__sk_divr` / `__sk_divl` (`emit_dk_runtime.c`), and the
  `any` path `__tur_dyn_arith` -- an `any` int is a full int64, so
  `INT64_MIN` reached its divide too, and its `mod` by zero said
  `division by zero`.
- **The interpreter** (`src/turi/eval.c`) divides through `turi_idiv_nz` /
  `turi_imod_nz` after its zero check: `eval_builtin`'s `/` and `mod`, the
  continuation-resume frames, and `ic_apply_binop` (the mini evaluator for
  inline-C bodies). A user expression no longer takes down the REPL.

Left alone: an `int32` quotient of `INT32_MIN / -1` is 2147483648 -- the
compiled program narrows it on the way out of a `: int32` return, the
interpreter keeps the int64 carrier. That is
[sized-int-arithmetic-not-wrapped-to-width](sized-int-arithmetic-not-wrapped-to-width.md),
not a trap (resolved 2026-10-21: both engines reduce the quotient to `int32`).

Pinned by `int-div-mod-minus-one` (both engines, int / int32 / uint64 / uint8),
`saffron-any-div-mod-minus-one` (the `any` path, both engines), and
`int-div-by-zero-aborts`, `int-mod-by-zero-aborts`, `int-umod-by-zero-aborts`
(compiled: a nonzero exit and the message -- `abort()` is 134 on POSIX but
not on Windows, so the exit is not pinned). Every snapshot and the split runtime
(`src/runtime/generated/`) were regenerated in the same change: the preamble
gained the three helpers.
