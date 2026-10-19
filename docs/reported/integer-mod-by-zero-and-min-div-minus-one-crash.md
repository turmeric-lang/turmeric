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
