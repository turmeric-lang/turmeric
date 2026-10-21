# Arithmetic on a sized integer is not wrapped to its width, and the engines disagree

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`sized-int-arith-wraps` (both engines).

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

## Resolution (2026-10-21)

Fix directions 1 and 2, behind one table predicate in `src/compiler/builtins.c`:

- `builtin_int_wrap_ctype(spec)` names the C type a builtin's result is reduced
  to -- non-NULL for the kinds narrower than 64 bits (`int8` `int16` `int32`
  `uint8` `uint16` `uint32`) on the rows whose result can leave the operand
  range: `+ - *`, `/` (`INT8_MIN / -1`), `bit-shl` and `bit-shr`.
  Comparisons, `mod` and and/or/xor stay in range; `int`, `int64` and
  `uint64` are their own width.
- Both emitters (`emit_builtin` in `emit_core.c`, `prim_expr` in
  `emit_cps_ir.c`) fold such a row in `uint64_t` and cast back:
  `(uint8_t)((uint64_t)(a) + (uint64_t)(b))`. Folding in `uint64_t` rather than
  C's `int` also removes a signed overflow the old text had at
  `uint16 * uint16` (65535 * 65535). A narrow `bit-shr` zero-extends from its
  own width, `(int8_t)((uint64_t)(uint8_t)(a) >> (uint64_t)(n))`, so
  `(bit-shr -16i8 2i8)` is 60; `bit-shl` shifts in `uint64_t` (no signed-shift
  UB) and casts back.
- The interpreter's `eval_builtin` reduces the int64 carrier with
  `builtin_int_wrap_value(spec->result_type.kind, ...)` in the `+ - *` fold, `/`
  and both shifts, so the engines agree.

That also closes the `INT32_MIN / -1` residue
[integer-mod-by-zero-and-min-div-minus-one-crash](integer-mod-by-zero-and-min-div-minus-one-crash.md)
left here: the quotient is reduced to `int32` on both engines.

Snapshot moved: `map-multiword-struct-value` (its `Num` instances for the sized
kinds now carry the cast). The `Num` instances in `stdlib/typeclass.tur` are
built on these operators, so `add`/`sub`/`mul` at a sized kind wrap too.

`sized-int-arith-wraps` covers every narrow kind unstored, through a parameter
and a return, a three-operand fold, a comparison of a wrapped sum, `/`, both
shifts, and the CPS backend (`main` and a performing function are lowered
there), on `tur run`, `tur --interpret` and `tur jit`.
