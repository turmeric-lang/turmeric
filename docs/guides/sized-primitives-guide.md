---
title: Sized Primitive Types
category: Type System
description: Fixed-width numeric types (int8/i8 through uint64/u64, float32/f32) -- literal syntax, coercion rules, and casting
---

# Sized Primitive Types Guide

Turmeric provides fixed-width numeric types alongside the canonical `int`
(64-bit signed) and `float` (64-bit IEEE 754) types. This guide explains
the available types, their literal syntax, the coercion rule, and how to
cast between them.

## Available Types

| Type name | Short alias | C type       | Width  | Signed |
|-----------|-------------|--------------|--------|--------|
| `int8`    | `i8`        | `int8_t`     | 8 bit  | yes    |
| `int16`   | `i16`       | `int16_t`    | 16 bit | yes    |
| `int32`   | `i32`       | `int32_t`    | 32 bit | yes    |
| `int64`   | `i64`       | `int64_t`    | 64 bit | yes    |
| `uint8`   | `u8`        | `uint8_t`    | 8 bit  | no     |
| `uint16`  | `u16`       | `uint16_t`   | 16 bit | no     |
| `uint32`  | `u32`       | `uint32_t`   | 32 bit | no     |
| `uint64`  | `u64`       | `uint64_t`   | 64 bit | no     |
| `float32` | `f32`       | `float`      | 32 bit | --     |
| `float64` | `f64`       | `double`     | 64 bit | --     |

`int` is an alias for `int64` and `float` is an alias for `float64`. All
other types are distinct numeric kinds at both the type-system and C-codegen
levels.

## Literal Syntax

Append a suffix to an integer or float literal to fix its kind:

```turmeric
42i8        ; int8   -- value 42
200u8       ; uint8  -- value 200
1000i16     ; int16
0xFFu32     ; uint32 -- hex literal
2.5f32      ; float32
3.14159f64  ; float64 (same as 3.14159)
```
```sweet-exp
42i8        ; int8   -- value 42
200u8       ; uint8  -- value 200
1000i16     ; int16
0xFFu32     ; uint32 -- hex literal
2.5f32      ; float32
3.14159f64  ; float64 (same as 3.14159)
```

Unsuffixed integer literals have type `int` (int64). Unsuffixed float
literals have type `float` (float64).

## No Implicit Coercion -- Use `(as ...)` Explicitly

**Turmeric does not implicitly widen or narrow numeric values.** Mixing
two distinct numeric kinds in an arithmetic or comparison expression is a
compile error (TUR-E0042):

```turmeric
(+ 1i8 2i16)      ; error TUR-E0042: int8 and int16 cannot be mixed
(+ 1i32 2)        ; error TUR-E0042: int32 and int (int64) cannot be mixed
(< 1.5f32 2.0)    ; error TUR-E0042: float32 and float cannot be mixed
```
```sweet-exp
{1i8 + 2i16}      ; error TUR-E0042: int8 and int16 cannot be mixed
{1i32 + 2}        ; error TUR-E0042: int32 and int (int64) cannot be mixed
{1.5f32 < 2.0}    ; error TUR-E0042: float32 and float cannot be mixed
```

To operate on values of different widths, cast one side with `(as type expr)`:

```turmeric
(+ 1i8 (as int8 2i16))      ; narrow 2i16 to int8
(+ (as int16 1i8) 2i16)     ; widen 1i8 to int16

(+ 1i32 (as int32 2))       ; narrow canonical int to int32
(+ (as int 1i32) 2)         ; widen int32 to int64

(< 1.5f32 (as float32 2.0)) ; narrow float64 literal to float32
```
```sweet-exp
{1i8 + as(int8 2i16)}       ; narrow 2i16 to int8
{as(int16 1i8) + 2i16}      ; widen 1i8 to int16

{1i32 + as(int32 2)}        ; narrow canonical int to int32
{as(int 1i32) + 2}          ; widen int32 to int64

{1.5f32 < as(float32 2.0)}  ; narrow float64 literal to float32
```

### Rationale

Implicit numeric coercion makes it easy to accidentally widen a
computation to 64 bits where a 32-bit or narrower representation was
intended. It also obscures the ABI boundary: a function declared to return
`:int32` should not silently widen every intermediate value to `int64_t` in
the emitted C. Requiring explicit `(as ...)` makes the intent visible in
source and keeps the generated C free of unintended `int64_t` round-trips.

## Arithmetic Operators

All standard arithmetic and comparison operators are available for each
numeric kind. Both operands must have the same kind:

```turmeric
(+ 10i8 20i8)       ; int8 + int8   -> int8
(mod 100u32 7u32)   ; uint32 mod uint32 -> uint32
(< 1i32 2i32)       ; int32 < int32  -> bool
(= 42u8 42u8)       ; uint8 = uint8  -> bool
(+ 1.5f32 2.5f32)   ; float32 + float32 -> float32
```
```sweet-exp
{10i8 + 20i8}       ; int8 + int8   -> int8
mod(100u32 7u32)   ; uint32 mod uint32 -> uint32
{1i32 < 2i32}       ; int32 < int32  -> bool
{42u8 = 42u8}       ; uint8 = uint8  -> bool
{1.5f32 + 2.5f32}   ; float32 + float32 -> float32
```

## Casting with `(as ...)`

`(as TargetType expr)` converts a value to a different numeric kind. The
conversion is the same as a C explicit cast -- no overflow checking is
performed:

```turmeric
(as int8  300)          ; wraps: 300 mod 256 = 44
(as uint8 -1)           ; wraps: 255
(as float32 1i32)       ; integer -> float
(as int32 3.7f32)       ; truncates: 3
```
```sweet-exp
as(int8 300)       ; wraps: 300 mod 256 = 44
as(uint8 -1)       ; wraps: 255
as(float32 1i32)   ; integer -> float
as(int32 3.7f32)   ; truncates: 3
```

Casts between signed and unsigned types of the same width reinterpret the
bit pattern at the C level.

## Struct Fields

Struct fields can use any numeric kind:

```turmeric
(defstruct Pixel
  [r : uint8
   g : uint8
   b : uint8
   a : uint8])

(defstruct Vec3f
  [x : float32
   y : float32
   z : float32])
```
```sweet-exp
defstruct Pixel
  [r :uint8
   g :uint8
   b :uint8
   a :uint8]

defstruct Vec3f
  [x :float32
   y :float32
   z :float32]
```

The generated C struct uses the corresponding `<stdint.h>` types
(`uint8_t`, `float`, etc.) directly, with no `int64_t` boxing for fields
of fully-known width.

## Bitwise Operations

`bit-and`, `bit-or`, `bit-xor`, `bit-shl`, and `bit-shr` are
**kind-preserving**: they operate on any integer kind, both operands must
have the same kind, and the result has that kind -- no round-trip through
`int` is needed:

```turmeric
(bit-and 0xF0u8 0x0Fu8)          ; uint8 & uint8 -> 0u8
(bit-or  65280u16 255u16)        ; uint16 -> 65535u16
(bit-shl 1u8 3u8)                ; uint8 -> 8u8
(+ (bit-and 100u16 255u16) 1u16) ; result is uint16, usable in uint16 ops
```
```sweet-exp
bit-and(0xF0u8 0x0Fu8)          ; uint8 & uint8 -> 0u8
bit-or(65280u16 255u16)         ; uint16 -> 65535u16
bit-shl(1u8 3u8)                ; uint8 -> 8u8
{bit-and(100u16 255u16) + 1u16}  ; result is uint16, usable in uint16 ops
```

`bit-shr` on an unsigned kind is a logical shift (0-fills the high bits).
Mixing kinds is rejected like any other mixed-width arithmetic; cast one
side with `(as ...)` first.

## Test Surface

The `numeric-types-cast` fixture covers the main casting scenarios.  The
`sized-mixed-arith-error` fixture covers every operator that rejects
mixed-width operands and expects `TUR-E0042` on each.  The
`sized-bitwise-narrow` fixture covers kind-preserving bitwise operations on
narrow integer kinds.
