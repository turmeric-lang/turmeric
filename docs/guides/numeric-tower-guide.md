---
title: Numeric Tower Guide
category: Language Basics
description: Exact Rational and hand-written Complex arithmetic, the #rat{...} / #cx{...} literals, and Num-typeclass operator overloading
---

# Numeric Tower: `Rational` and `Complex`

Turmeric ships two numeric types above the machine primitives:

| Type | Module | Representation | Literal |
|---|---|---|---|
| `Rational` | [`stdlib/rational.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/rational.tur) | `num`/`den` pair over `int` (int64), always normalized | `#rat{3/4}` |
| `Complex` | [`stdlib/complex.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/complex.tur) | `re`/`im` pair over `float` (double) | `#cx{3.25 -1.5}` |

Both are `:copy` single-variant record products, so each lowers to a 16-byte C
struct of two `int64_t` / two `double`. Neither carries an owning field, so
neither has drop glue and neither interacts with the rc/ref/borrow machinery at
all.

```turmeric no-check
(load "stdlib/rational.tur")
(load "stdlib/complex.tur")

(+ #rat{1/3} #rat{1/6})     ; => 1/2, exactly
(* #cx{3.25 1.5} #cx{0.5 2.0})
```

## Operators come from the `Num` typeclass

`+`, `-`, `*`, `/` are builtin operator rows keyed by `TypeKind` that emit a C
infix operator. That shape cannot express `Rational + Rational` -- there is no C
`+` for a struct, and the spec table has no way to name a specific ADT.

So when **no builtin row matches the argument types**, the call falls back to
`Num` typeclass dispatch. This is the general mechanism, not a special case for
these two types: any user numeric type with a `Num` instance gets the operators.

```turmeric no-check
(defstruct V2 :copy [x : float y : float])

(definstance Num [V2]
  (add [a b] (V2 (+ (.x a) (.x b)) (+ (.y a) (.y b))))
  (sub [a b] (V2 (- (.x a) (.x b)) (- (.y a) (.y b))))
  (mul [a b] (V2 (* (.x a) (.x b)) (* (.y a) (.y b))))
  (div [a b] (V2 (/ (.x a) (.x b)) (/ (.y a) (.y b))))
  (neg [a]   (V2 (- 0.0 (.x a)) (- 0.0 (.y a)))))

(+ (V2 3.25 1.5) (V2 0.5 2.5))     ; => V2 3.75 4.0
(- (V2 3.25 1.5))                   ; => neg
(+ a b c)                           ; left-folds: (add (add a b) c)
```

Notes:

- **Primitive arithmetic is untouched.** The builtin row still wins whenever it
  matches, so there is no codegen drift on existing programs and no dictionary
  in the hot path.
- **Variadic calls left-fold** into nested binary method calls, matching what
  `BS_VARIADIC_FOLD` does for the primitive rows.
- **Unary `-` reaches `neg`.** `(- x)` on a type with a `Num` instance is
  `(neg x)`; on a primitive it is still the arity error it always was.

The `Num` class is closed over its instance type:

```turmeric no-check
(defclass Num [a]
  (add [x : a y : a] : a)
  (sub [x : a y : a] : a)
  (mul [x : a y : a] : a)
  (div [x : a y : a] : a)
  (neg [x : a]       : a))
```

Every method returns `a`, not `:int`. That is what lets `(add r1 r2)` on two
Rationals be a Rational at the call site rather than an integer.

## `Rational`

### Construction and normalization

Every Rational is normalized: `den > 0` and `gcd(|num|, den) = 1`. That makes
structural equality *be* mathematical equality, so `Eq` and `Hash` are trivial.

```turmeric no-check
(rat/of! 6 8)      ; => 3/4
(rat/of  6 8)      ; => (ok 3/4)
(rat/of  1 0)      ; => (err (DivByZero))
(rat/from-int 7)   ; => 7/1
#rat{6/8}          ; => 3/4, normalized at READ time
```

`rat/of` returns `(Result Rational ArithError)`; `rat/of!` panics on a zero
denominator. `ArithError` is a real ADT -- `(DivByZero)` or `(Overflow)` -- not
an int status code.

`#rat{n/0}` is a **read-time** error, so the literal form never needs the result
wrapper.

### Arithmetic and overflow

`num` and `den` are int64. Arbitrary precision is out of scope -- there is no
bignum in the tree.

Two mitigations, both standard:

- **Cross-cancel before multiplying.** `rat/add` reduces by `g = gcd(b, d)` and
  computes `(a*(d/g) + c*(b/g)) / (b*(d/g))`, which keeps the common case in
  range far longer than the naive `a*d + c*b` over `b*d`. `rat/mul`
  cross-cancels each numerator against the other denominator.
- **Checked variants.** `rat/try-add`, `rat/try-sub`, `rat/try-mul`,
  `rat/try-div` return `(Result Rational ArithError)` and detect the overflow
  *before* it happens, via explicit comparisons written in Turmeric (so every
  engine agrees).

The unchecked forms -- the ones behind `+` -- **wrap** on overflow, matching what
`int` already does. A program that cares uses the `try-` forms.

```turmeric no-check
(rat/try-add #rat{1/3} #rat{1/6})                              ; => ok 1/2
(rat/try-mul (rat/from-int 4294967296)
             (rat/from-int 4294967296))                        ; => err Overflow
(rat/try-div #rat{1/2} #rat{0/1})                              ; => err DivByZero
```

### Comparison, conversion, printing

`Ord [Rational]` compares `a*(d/g)` against `c*(b/g)` for `g = gcd(b, d)`, so the
cross-products stay small. `rat->float` is the lossy exit from the exact tower.
`Show`/`rat->string` renders `3/4`, `-3/4`, and `5` when `den == 1`, so the
output round-trips through `#rat{...}`.

## `Complex`

### No `_Complex`, ever

`Complex` is a plain two-`double` struct with hand-written arithmetic. The
emitted C never contains `_Complex`, `<complex.h>`, or the `__mul*c3` /
`__div*c3` family, deliberately:

- `c2mir` (the JIT front end) implements a C11 subset that does **not** include
  the optional `_Complex` feature.
- Even on a compiler that supports it, `a * b` and `a / b` on
  `double _Complex` do not compile to arithmetic -- they compile to calls into
  `__muldc3` / `__divdc3` in the compiler runtime. Those would become external
  symbols the JIT must resolve through `MIR_load_external`, i.e. entries in the
  runtime symbol boundary, in exchange for nothing that ~15 lines of Turmeric
  does not already provide.

### Division uses Smith's algorithm

The textbook form `(a+bi)/(c+di) = ((ac+bd) + (bc-ad)i)/(c^2+d^2)` computes
`c^2 + d^2` as an intermediate, and that intermediate is where it breaks: for `c`
near `1e200` it is `+inf`, and for `c` near `1e-200` it is `0` -- in both cases
the true quotient is perfectly ordinary and representable.

`complex/div` divides through by the larger-magnitude denominator component
first, so every intermediate stays near the magnitude of the answer:

```
|c| >= |d|:  r = d/c, den = c + d*r, x/y = ((a + b*r) + (b - a*r)i)/den
|c| <  |d|:  r = c/d, den = c*r + d, x/y = ((a*r + b) + (b*r - a)i)/den
```

`complex/abs` scales the same way (`|a| * sqrt(1 + (b/a)^2)`), so `|z|` is finite
where `re^2 + im^2` is not.

### Surface

```turmeric no-check
(complex/of 3.25 -1.5)     ; also #cx{3.25 -1.5}
(complex/re z) (complex/im z)
(complex/from-float 2.5) (complex/zero) (complex/one) (complex/i)
(complex/add x y) (complex/sub x y) (complex/mul x y) (complex/div x y)
(complex/neg x) (complex/conj x) (complex/scale x 2.0)
(complex/abs z) (complex/abs2 z) (complex/arg z) (complex/exp z)
(complex/eq? x y) (complex->string z)
```

`Show` renders `3.25+4.5i` / `3.25-4.5i`, with the sign of the imaginary part
always explicit so the output is unambiguous.

### There is deliberately no `Ord [Complex]`

The complex numbers are not an ordered field. `(lt? z1 z2)` is a `TUR-E0015`
type error, and that is the right answer -- do not invent a lexicographic
instance to fill the table.

## Three engines, one implementation

Both modules are written in ordinary Turmeric with **no inline C**. That is not
an aesthetic choice: the tree-walking interpreter cannot execute inline C and
carries hand-written native overrides to compensate, so a numeric tower written
in Turmeric needs *zero* new natives and is byte-identical under `tur`, `turi`,
and `tur jit` by construction.  Every rational/complex fixture -- the error
negatives included -- passes under the MIR engine with zero cc-fallbacks, and
`tests/run.sh` carries a standing check (`tests/check-no-c-complex.sh`) that
`_Complex`, `<complex.h>`, and the `__mul*c3`/`__div*c3` compiler-runtime
family never appear in emitter source or generated C.

The one dependency on inline C is the scalar libm layer in
[`stdlib/math.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/math.tur) -- `sqrt`, `fabs`, `exp`, `sin`,
`cos`, `atan2` -- which `complex/abs`, `complex/arg`, and `complex/exp` are built
on. Each of those has a matching interpreter native so the two engines agree.

Fixtures live under `tests/fixtures/rational-*` and `tests/fixtures/complex-*`
and run on both the compiled and the interpreted harness against the *same*
`expected.stdout`. If that ever diverges, the divergence is itself the bug worth
finding.

## Crossing between `int` and `float`

Below the exact tower sits the ordinary machine pair, and moving between its two
halves is **two different operations that must not share a spelling**:

| you mean | int -> float | float -> int | from |
| --- | --- | --- | --- |
| the NUMBER -- convert | `(int->float 7)` => `7` | `(float->int 7.1)` => `7` | `stdlib/math.tur` |
| the BIT PATTERN -- reinterpret | `(bits->float 7)` => `3.45846e-323` | `(float->bits 7.1)` => `4619679907765970534` | `stdlib/bits.tur` |

`::` spells **neither**. An ascription between an integer kind and a float kind
is a hard error, and the diagnostic names both replacements:

```turmeric no-check
(defstruct Mixed [n : int d : float])
(defn mixedfold [m : Mixed] : float
  (+ (:: (.n m) :float) (.d m)))   ; error: `::` between an integer and a
                                   ; float kind is ambiguous
```

The reason is that **both operands are statically `:int`** in the two cases that
mean opposite things. `(.n m)` above is a genuine integer wanting conversion.
`(list-head c)` on a `Cons[float]` is float bits that reached an `:int`-typed
slot through the erased carrier -- typed slots, variadic rest collection and the
cons/HAMT carriers all round-trip floats that way -- and wants reinterpretation.
Nothing in the types separates them, so whichever reading `::` picked, the other
one's existing code would keep compiling and start returning silently wrong
answers. The reinterpret reading is the one that shipped, and its failure mode
is nasty: a small integer read as IEEE bits is a denormal, so a wrong
`mixedfold` returns `0.25` rather than `3.25` -- a *dropped* term, not a garbage
one.

An integer **literal** is exempt and still converts (`(:: 7 :float)` is `7.0`):
the author wrote a constant, so there is no carried value whose bits could be
meant. All four functions behave identically compiled and interpreted. See
[docs/archive/ascribe-int-to-float-expression-ambiguity.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ascribe-int-to-float-expression-ambiguity.md).

## Related

- [data-literals-guide.md](data-literals-guide.md) -- the `#rat{...}` and
  `#cx{...}` reader dispatches alongside `#map{...}` / `#set{...}`
- [typeclass-guide.md](typeclass-guide.md) -- `defclass` / `definstance` and
  dictionary passing
- [typing-handles-callbacks-results-guide.md](typing-handles-callbacks-results-guide.md)
  -- why `ArithError` is an ADT and not an int status code
