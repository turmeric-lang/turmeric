# Arithmetic on a sized integer is not wrapped to its width, and the engines disagree

**Severity: medium (silently wrong value).** Found 2026-10-19 probing the
operators for their API reference (`stdlib/forms/operators.tur`).

`+ - *` (and `bit-shr`) on `int8` / `int16` / `int32` / `uint8` ... `uint64`
are typed as returning the operand type, but the value is not reduced to that
width. Compiled, it is reduced only once it is stored into a C variable of the
type (a parameter, a return value); interpreted, never.

| Program | Compiled | Interpreted | Expected |
| --- | --- | --- | --- |
| `(let [x : uint8 (:: 200 :uint8) y : uint8 (:: 100 :uint8)] (println (+ x y)))` | 300 | 300 | 44 |
| `(defn f [a : uint8 b : uint8] : uint8 (+ a b))` then `(println (f 200 100))` | 44 | 300 | 44 |
| `(defn g [a : uint32 b : uint32] : uint32 (- a b))` then `(println (g 0 1))` | 4294967295 | 18446744073709551615 | 4294967295 |
| `(defn h [a : int8 n : int8] : int8 (bit-shr a n))` then `(println (h -16 2))` | -4 | 4611686018427387900 | 60 (`0xF0 >> 2`, logical) |

(Pass the arguments as `(:: 200 :uint8)` etc.)

## Root cause

- Compiled: `BS_VARIADIC_FOLD` emits `(a) + (b)` with no cast to the result
  type (`src/compiler/emit_core.c`, `case BS_VARIADIC_FOLD`). C promotes
  `uint8_t + uint8_t` to `int`, so the expression is 300 until something of
  type `uint8_t` receives it. `bit-shr`'s `(uint64_t)(a) >> (uint64_t)(b)`
  (`case BS_BIN_INFIX`) widens a negative `int8` with its sign first, so the
  shift is logical over 64 bits, not 8.
- Interpreted: every integer is an `int64_t` carrier; the arithmetic fold
  (`src/turi/eval.c`, `eval_builtin`'s `+`/`-`/`*` arm) and the bitwise arm
  never reduce to the declared kind -- the comment above the bitwise arm says
  so ("the kind-preserving width masking ... is a no-op here").

## Fix directions

1. Emit a cast to the result's C type around each sized-int arithmetic and
   bitwise result: `(uint8_t)((a) + (b))`; for `bit-shr`, shift through the
   unsigned type of the same width (`(int8_t)((uint8_t)(a) >> (b))`).
2. In the interpreter, reduce to the expression's kind after the operation
   (the builtin spec carries `result_type`).
3. A fixture over each sized kind, both engines, including the unstored
   `(println (+ x y))` shape.
