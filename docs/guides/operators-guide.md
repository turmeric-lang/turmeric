---
title: Operators
category: Language Basics
description: Arithmetic, comparison, logical and bitwise operators -- which types each takes, how integers wrap and divide, and the float-only forms
---

# Operators

Every program has the operators; there is nothing to import. They are
compiler builtins, documented one by one in
[tur/forms/operators](../html/api/tur-forms-operators.html). This guide is the
rules that apply to all of them.

## One type per call, chosen by the first argument

An operator picks its overload from the type of its **first** argument, and
every other argument must have that type. There is no implicit int-to-float
promotion: `(+ 1 2.5)` is TUR-E0042. Convert first with `int->float` /
`float->int` (`tur/math`), or write the literal in the right kind (`1.0`).

The numeric kinds are `int` (64-bit signed), `float` (64-bit), `float32`, and
the sized integers `int8` `int16` `int32` `uint8` `uint16` `uint32` `uint64`
([sized-primitives-guide](sized-primitives-guide.md)). For a type with no
builtin row -- `Rational`, `Complex`, your own -- `+ - * /` fall back to its
`Num` instance ([numeric-tower-guide](numeric-tower-guide.md)).

| Operators | Arguments | Types |
|---|---|---|
| `+ - *` | two or more, left to right | any numeric kind |
| `-` | one: negation | any numeric kind |
| `/` | exactly two | any numeric kind |
| `mod` | exactly two | integer kinds only |
| `= not=` | exactly two | numeric kinds, `bool`, `Sym` |
| `< > <= >=` | exactly two | numeric kinds |
| `and or` | two or more, short-circuiting | `bool` |
| `not` | one | `bool` |
| `+. -. *. /.` | `+.`/`-.`/`*.`: two or more; `/.`: two; `-.` also one (negation) | `float` only |
| `bit-and bit-or bit-xor bit-shl bit-shr` | exactly two | integer kinds |

```turmeric
(defn main [] : int
  (println (+ 1 2 3))          ; 6
  (println (- 10 3 2))         ; 5  -- (10 - 3) - 2
  (println (- 7.1))            ; -7.1
  (println (/ 7.5 2.0))        ; 3.75
  (println (and (< 1 2) (< 2 3)))   ; true
  0)
```

```sweet-exp
#lang sweet-exp

defn main [] : int
  println(+(1 2 3))          ; 6
  println(-(10 3 2))         ; 5  -- (10 - 3) - 2
  println(-(7.1))            ; -7.1
  println({7.5 / 2.0})       ; 3.75
  println(and({1 < 2} {2 < 3}))   ; true
  0
```

A few calls you might expect to work do not:

- `(< 1 2 3)` is TUR-E0006: comparisons take exactly two arguments.
- `=` is not defined on `cstr` (use `cstr-eq?`) or on structs and ADTs
  (`=` does not dispatch to `Eq`; call `eq?`).
- `and`, `or` and `not` take only `bool`. An `int` is not truthy: `(and 1 0)`
  is TUR-E0006.

## Integers wrap, and division truncates

Integer arithmetic wraps on overflow, two's complement, **at the kind's own
width** -- `(+ 200u8 100u8)` is 44 -- whether or not the result is stored.
Negation is no exception: `(- 1u8)` is 255, and `(- x)` on the smallest `int`
is that same value.

`/` on integers truncates toward zero, and `mod` takes the sign of the
dividend, as C's `%` does:

| Expression | Result |
|---|---|
| `(/ 7 2)` | 3 |
| `(/ -7 2)` | -3 |
| `(mod -7 3)` | -1, not 2 |
| `(/ x 0)` | prints `division by zero` and aborts |
| `(mod x 0)` | prints `modulo by zero` and aborts (an error under `--interpret`) |
| `(/ min-int -1)` | wraps to the smallest `int`, no trap |
| `(mod x -1)` | 0, for every `x` |

Float division follows IEEE 754 instead: dividing by zero gives `inf`, `-inf`
or `nan`, with no check, and a `nan` is `=` to nothing, itself included.

To negate a float, write `(- x)`, not `(- 0.0 x)`. They differ at zero:
`(- 0.0)` is -0.0, while `(- 0.0 0.0)` is +0.0, and the sign shows the moment
something divides by it (`(/ 1.0 (- 0.0))` is `-inf`).

## `and` and `or` short-circuit

They evaluate left to right and stop at the first argument that decides the
answer, so the rest are not evaluated:

```turmeric
(defn safe-ratio-above? [a : int b : int limit : int] : bool
  (and (not= b 0) (> (/ a b) limit)))   ; never divides by zero

(defn main [] : int
  (println (safe-ratio-above? 10 0 1))  ; false
  (println (safe-ratio-above? 10 2 1))  ; true
  0)
```

```sweet-exp
#lang sweet-exp

defn safe-ratio-above? [a : int b : int limit : int] : bool
  and(not=(b 0) {{a / b} > limit})   ; never divides by zero

defn main [] : int
  println(safe-ratio-above?(10 0 1))  ; false
  println(safe-ratio-above?(10 2 1))  ; true
  0
```

## The float-only forms `+.` `-.` `*.` `/.`

`+.` is `+` restricted to `float`: an `int` or `float32` argument is TUR-E0006
instead of quietly selecting integer arithmetic. Reach for them where the
checker should insist the operands are floats -- typically inside a generic
helper, or after a refactor that could turn a `float` into an `int` without
anyone noticing. They compute exactly what `+ - * /` compute on floats.

```turmeric
(defn average [a : float b : float] : float
  (/. (+. a b) 2.0))

(defn main [] : int
  (println (average 1.25 2.5))   ; 1.875
  0)
```

```sweet-exp
#lang sweet-exp

defn average [a : float b : float] : float
  /.(+.(a b) 2.0)

defn main [] : int
  println(average(1.25 2.5))   ; 1.875
  0
```

## Bitwise operators

`bit-and`, `bit-or`, `bit-xor` and `bit-shl` work on every integer kind, both
arguments of the same kind. `bit-shl` loses the bits shifted past the top.
`bit-shr` is a **logical** right shift -- it shifts zeros in, whatever the sign
-- so `(bit-shr -16 60)` is 15, not -1. A shift count that is negative or not
less than the width is undefined, as in C.

```turmeric
(defn main [] : int
  (println (bit-and 12 10))     ; 8
  (println (bit-or 12 10))      ; 14
  (println (bit-xor 12 10))     ; 6
  (println (bit-shl 1 4))       ; 16
  (println (bit-shr -16 60))    ; 15 -- logical, zeros shifted in
  0)
```

```sweet-exp
#lang sweet-exp

defn main [] : int
  println(bit-and(12 10))     ; 8
  println(bit-or(12 10))      ; 14
  println(bit-xor(12 10))     ; 6
  println(bit-shl(1 4))       ; 16
  println(bit-shr(-16 60))    ; 15 -- logical, zeros shifted in
  0
```

`tur/bits` also has `float->bits` / `bits->float`, which reinterpret a float's
IEEE-754 bit pattern as an `int` and back.

## Related

- [numeric-tower-guide](numeric-tower-guide.md) -- `Rational`, `Complex`, and
  operators through `Num`
- [sized-primitives-guide](sized-primitives-guide.md) -- the sized kinds and
  their literals
- [syntax-guide](syntax-guide.md) -- curly-infix `{a + b}`
