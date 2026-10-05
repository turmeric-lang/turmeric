---
title: Range Reader Shorthand Plan
category: Language Basics
description: Plan for #r{...} reader-level range shorthand (RR0-RR4) -- desugars to Range constructor calls at read time
---

# Range Reader Shorthand -- Plan (RR0--RR4)

> **Status:** SHIPPED. All phases (RR0-RR4, including the RR3 shadowing
> warning) are implemented -- see `read_range_literal` in
> `src/compiler/reader.c`, `stdlib/float-range.tur`, and the
> `tests/fixtures/range-reader-*` fixtures. This document is kept as the
> design reference for the grammar and desugaring tables.
> **Type:** Reader / Syntax Feature

---

## Overview

`#r{...}` is a reader-level shorthand for constructing `Range` values from
`stdlib/range.tur` (integers) and `stdlib/float-range.tur` (floats).  It
desugars completely at read time to a plain constructor call -- no new
elaborator phases, no new type-system machinery.

```turmeric
;; One-sided (unbounded on one end)
#r{n > 0}        ; => (greater-than-range 0)         -- (0, +inf)
#r{n >= 0}       ; => (at-least-range 0)              -- [0, +inf)
#r{n < 0}        ; => (less-than-range 0)             -- (-inf, 0)
#r{n <= 0}       ; => (at-most-range 0)               -- (-inf, 0]
#r{n = 0}        ; => (singleton-range 0)             -- [0, 0]

;; Two-sided (left-to-right)
#r{0 <= n < 10}  ; => (closed-open-range 0 10)        -- [0, 10)
#r{0 < n <= 10}  ; => (open-closed-range 0 10)        -- (0, 10]
#r{0 < n < 10}   ; => (open-range 0 10)               -- (0, 10)
#r{0 <= n <= 10} ; => (closed-range 0 10)             -- [0, 10]

;; Two-sided (right-to-left -- same semantics, mirrored operators)
#r{10 > n >= 0}  ; => (closed-open-range 0 10)        -- [0, 10)
#r{10 >= n > 0}  ; => (open-closed-range 0 10)        -- (0, 10]
#r{10 > n > 0}   ; => (open-range 0 10)               -- (0, 10)
#r{10 >= n >= 0} ; => (closed-range 0 10)             -- [0, 10]

;; Float bounds (uses float-range constructors)
#r{0.0 <= n < 1.0} ; => (float-closed-open-range 0.0 1.0)

;; Expression bounds (runtime values, no compile-time empty check)
#r{0 <= n < len}   ; => (closed-open-range 0 len)
#r{lo <= n <= hi}  ; => (closed-range lo hi)
```
```sweet-exp
;; One-sided (unbounded on one end)
#r{n > 0}        ; => greater-than-range(0)         -- (0, +inf)
#r{n >= 0}       ; => at-least-range(0)             -- [0, +inf)
#r{n < 0}        ; => less-than-range(0)            -- (-inf, 0)
#r{n <= 0}       ; => at-most-range(0)              -- (-inf, 0]
#r{n = 0}        ; => singleton-range(0)            -- [0, 0]

;; Two-sided (left-to-right)
#r{0 <= n < 10}  ; => closed-open-range(0 10)       -- [0, 10)
#r{0 < n <= 10}  ; => open-closed-range(0 10)       -- (0, 10]
#r{0 < n < 10}   ; => open-range(0 10)              -- (0, 10)
#r{0 <= n <= 10} ; => closed-range(0 10)            -- [0, 10]

;; Two-sided (right-to-left -- same semantics, mirrored operators)
#r{10 > n >= 0}  ; => closed-open-range(0 10)       -- [0, 10)
#r{10 >= n > 0}  ; => open-closed-range(0 10)       -- (0, 10]
#r{10 > n > 0}   ; => open-range(0 10)              -- (0, 10)
#r{10 >= n >= 0} ; => closed-range(0 10)            -- [0, 10]

;; Float bounds (uses float-range constructors)
#r{0.0 <= n < 1.0} ; => float-closed-open-range(0.0 1.0)

;; Expression bounds (runtime values, no compile-time empty check)
#r{0 <= n < len}   ; => closed-open-range(0 len)
#r{lo <= n <= hi}  ; => closed-range(lo hi)
```

The variable name (`n`, `i`, `x`, ...) must be a single-letter identifier.
It guides the human reader but is not bound or referenced in the expansion.
A warning is emitted at elaboration time if the variable shadows an outer
binding (see RR3).

---

## Motivation

`closed-open-range 0 10` requires the reader to know the Turmeric range
constructor names and to mentally map the argument order to interval
boundaries.  The `#r{...}` form lets the reader write the constraint in the
same notation used in mathematical prose and most type-system papers, while
the compiler substitutes the correct constructor at read time.

Useful contexts:

- **Contract types**: `{ i : :int | (range-contains? #r{0 <= i < len} i) }`
- **Guard conditions**: `(when (range-contains? #r{1 <= score <= 5} score) ...)`
- **Seq iteration**: `(for i (seq/from-range #r{0 <= i < n}) ...)`
- **Pattern documentation**: self-describing bounds in function signatures

---

## Syntax Grammar

```
range-literal   ::= '#r{' range-body '}'
range-body      ::= two-sided-range | one-sided-range

two-sided-range ::= form op-fwd var op-fwd form   ; left-to-right: lo < n <= hi
                  | form op-rev var op-rev form   ; right-to-left: hi > n >= lo
op-fwd  ::= '<' | '<='
op-rev  ::= '>' | '>='

one-sided-range ::= var op form
                  | form op var
op      ::= '<' | '<=' | '>' | '>=' | '='

var     ::= single-letter symbol  (a-z or A-Z)
form    ::= any Turmeric form (literal or expression)
```

Notes:
- `var` must be exactly one ASCII letter.  Anything longer is a reader error.
- `form` on either bound side may be a literal (`0`, `-10`, `3.14`) or any
  arbitrary expression (`len`, `(vec-len v)`, `(+ lo offset)`).
- Mixing `op-fwd` and `op-rev` in a two-sided form is a reader error
  (`0 < n > 5` is meaningless and rejected).
- `=` is only valid in one-sided position.  Using it in a two-sided form is a
  reader error.
- Numeric type of the bounds determines which constructor family is used:
  - All-integer (or expression) bounds use `stdlib/range.tur` constructors.
  - Any float literal on either bound uses `stdlib/float-range.tur`
    constructors.  Mixing integer literals with a float literal promotes the
    integers (`0` alongside `1.0` uses float constructors).

---

## Desugaring Tables

### One-sided (var op form)

| `#r{...}` | Expansion | Interval |
|-----------|-----------|----------|
| `n = v`   | `(singleton-range v)` | `[v, v]` |
| `n < hi`  | `(less-than-range hi)` | `(-inf, hi)` |
| `n <= hi` | `(at-most-range hi)` | `(-inf, hi]` |
| `n > lo`  | `(greater-than-range lo)` | `(lo, +inf)` |
| `n >= lo` | `(at-least-range lo)` | `[lo, +inf)` |

### One-sided (form op var) -- mirrors

Flip the operator to canonical `var op form`, then apply the table above.

| Seen | Canonical | Flip rule |
|------|-----------|-----------|
| `hi > n`  | `n < hi`  | `>` -> `<` |
| `hi >= n` | `n <= hi` | `>=` -> `<=` |
| `lo < n`  | `n > lo`  | `<` -> `>` |
| `lo <= n` | `n >= lo` | `<=` -> `>=` |

### Two-sided left-to-right (form op-fwd var op-fwd form)

| `op-left` | `op-right` | Constructor |
|-----------|------------|-------------|
| `<=`      | `<=`       | `closed-range lo hi` |
| `<`       | `<`        | `open-range lo hi` |
| `<=`      | `<`        | `closed-open-range lo hi` |
| `<`       | `<=`       | `open-closed-range lo hi` |

### Two-sided right-to-left (form op-rev var op-rev form)

The form reads `hi op1 n op2 lo`.  Derive the bound inclusivity from each
operator's perspective on the variable:

| `op1` (hi op1 n) | `op2` (n op2 lo) | Derived | Constructor |
|------------------|------------------|---------|-------------|
| `>` (n < hi, excl upper) | `>` (n > lo, excl lower) | (lo, hi) | `open-range lo hi` |
| `>=` (n <= hi, incl upper) | `>=` (n >= lo, incl lower) | [lo, hi] | `closed-range lo hi` |
| `>` (n < hi, excl upper) | `>=` (n >= lo, incl lower) | [lo, hi) | `closed-open-range lo hi` |
| `>=` (n <= hi, incl upper) | `>` (n > lo, excl lower) | (lo, hi] | `open-closed-range lo hi` |

Arguments are passed as `(constructor lo hi)` (lo and hi swapped relative to
the source token order, since the source reads right-to-left).

### Float constructors

Float variants mirror the integer constructors exactly, using the
`float-` prefix:

| Integer constructor | Float constructor |
|---------------------|-------------------|
| `closed-range` | `float-closed-range` |
| `open-range` | `float-open-range` |
| `closed-open-range` | `float-closed-open-range` |
| `open-closed-range` | `float-open-closed-range` |
| `at-least-range` | `float-at-least-range` |
| `greater-than-range` | `float-greater-than-range` |
| `at-most-range` | `float-at-most-range` |
| `less-than-range` | `float-less-than-range` |
| `singleton-range` | `float-singleton-range` |

---

## Implementation Phases

### RR0 -- Reader dispatch and tokenization

**Location:** `src/compiler/reader.c`, inside `read_form`.

Add a new dispatch arm immediately before the `#fx{}` map check:

```c
/* RR0: Range literal #r{...} */
if (c == '#' && peek2(r) == 'r' && peek3(r) == '{') {
    return read_range_literal(r);
}
/* existing: #{ map */
if (c == '#' && peek2(r) == '{') {
    return read_map(r);
}
```

`read_range_literal` consumes `#`, `r`, `{`, then reads forms using the
existing `read_form` machinery until `}`.  It collects exactly three or five
tokens (see grammar).  Any other count is a reader error.

No new `FormTag` is introduced.  The function returns an `F_LIST` form
representing the desugared constructor call, indistinguishable from a
hand-written call.

**Deliverable:** `#r{...}` is recognized by the reader.  Invalid interiors
produce a clear `DIAG_ERROR`.

---

### RR1 -- One-sided and literal two-sided ranges

Implement:
- All five one-sided forms (`<`, `<=`, `>`, `>=`, `=`) in both orientations.
- Two-sided left-to-right forms with literal bounds.
- Two-sided right-to-left forms with literal bounds.
- **Compile-time empty-range warning** when both bounds are integer or float
  literals and the range is provably empty (e.g. `#r{5 <= n < 3}`).
  This is a `DIAG_WARNING`, not an error.

Variable validation:
- Token identified as `var` must be a single `F_SYM` whose name is exactly
  one ASCII letter.  Otherwise emit `DIAG_ERROR`:
  `"#r{...}: variable must be a single letter, got '<name>'"`.

**Deliverables:**
- One-sided and literal two-sided `#r{...}` forms desugar correctly.
- Fixture: `tests/fixtures/range_reader_one_sided.tur`
- Fixture: `tests/fixtures/range_reader_two_sided.tur`

---

### RR2 -- Expression bounds

Allow any Turmeric form (not just literals) as a bound.  The reader passes
the form through unchanged into the constructor call; evaluation happens at
runtime.

```turmeric
#r{0 <= n < len}        ; => (closed-open-range 0 len)
#r{lo <= n <= hi}       ; => (closed-range lo hi)
#r{(- hi 1) > n >= 0}  ; => (closed-open-range 0 (- hi 1))
```
```sweet-exp
#r{0 <= n < len}         ; => closed-open-range(0 len)
#r{lo <= n <= hi}        ; => closed-range(lo hi)
#r{{hi - 1} > n >= 0}    ; => closed-open-range(0 {hi - 1})
```

Changes from RR1:
- The compile-time empty-range check is **skipped** when either bound is not a
  numeric literal (a non-literal form has unknown value at read time).
- `singleton-range` (`n = v`) still requires `v` to be a form; the reader
  passes it through.
- No other changes to the desugaring logic.

**Deliverables:**
- Expression bounds pass through to the constructor call.
- Fixture: `tests/fixtures/range_reader_expr_bounds.tur`

---

### RR3 -- Variable shadowing warning (elaborator)

The variable name in `#r{...}` is not bound.  However, if the surrounding
scope already binds a name with the same single letter, using that letter in
the range literal is misleading -- the reader has already discarded it.

A light check in the elaborator:

- After `read_range_literal` desugars to a constructor call, the original
  variable name is lost.  To preserve it for this check, the reader annotates
  the desugared `F_LIST` with a source-level attribute (reusing the `#[...]`
  attribute form mechanism, or a simpler side-table keyed by form span).
- The elaborator, when it sees a range constructor call that carries the
  annotation, checks whether the annotated name is bound in the current
  environment.  If so, emit `DIAG_WARNING`:
  `"#r{...}: variable 'n' shadows a binding in scope; the name is not used in the expansion"`.

This phase is **optional** for initial shipping; the warning is ergonomic, not
correctness-critical.  It can be deferred until after RR2.

**Deliverable:** Shadowing warning emitted when `n` names an outer binding.
Fixture: `tests/fixtures/range_reader_shadow_warn.tur`

---

### RR4 -- Float range stdlib

**Location:** `stdlib/float-range.tur`

Mirror of `stdlib/range.tur` using `float64` bounds.  The internal
`FloatRangeBound` struct uses `double` instead of `int64_t`.

```turmeric
;; float-range constructors (same shape as stdlib/range.tur)
(defn float-closed-range [lo : float hi : float] : int ...)
(defn float-open-range [lo : float hi : float] : int ...)
(defn float-closed-open-range [lo : float hi : float] : int ...)
(defn float-open-closed-range [lo : float hi : float] : int ...)
(defn float-at-least-range [lo : float] : int ...)
(defn float-greater-than-range [lo : float] : int ...)
(defn float-at-most-range [hi : float] : int ...)
(defn float-less-than-range [hi : float] : int ...)
(defn float-singleton-range [v : float] : int ...)
(defn float-unbounded-range [] : int ...)
(defn float-range-contains? [r : int v : float] : bool ...)
```
```sweet-exp
;; float-range constructors (same shape as stdlib/range.tur)
defn float-closed-range [lo :float hi :float] :int ...
defn float-open-range [lo :float hi :float] :int ...
defn float-closed-open-range [lo :float hi :float] :int ...
defn float-open-closed-range [lo :float hi :float] :int ...
defn float-at-least-range [lo :float] :int ...
defn float-greater-than-range [lo :float] :int ...
defn float-at-most-range [hi :float] :int ...
defn float-less-than-range [hi :float] :int ...
defn float-singleton-range [v :float] :int ...
defn float-unbounded-range [] :int ...
defn float-range-contains? [r :int v :float] :bool ...
```

The reader selects float constructors when any bound token is a `F_FLOAT`
literal.  If one bound is `F_INT` and the other is `F_FLOAT`, the int is
promoted (the constructor call is emitted with the integer as a float literal,
e.g. `0` -> `0.0`).

If a bound is a non-literal expression, the reader cannot infer the numeric
type.  In that case it defaults to the integer constructor family and emits a
`DIAG_NOTE` if the other bound is a float literal:
`"#r{...}: mixed literal types; using float constructors -- ensure the expression produces a float"`.

**Deliverables:**
- `stdlib/float-range.tur` with all constructors and `float-range-contains?`.
- Reader emits float constructors when float literals are present.
- Fixture: `tests/fixtures/range_reader_float.tur`

---

## Error Messages

| Situation | Message |
|-----------|---------|
| Interior is empty | `#r{} requires a range expression` |
| Wrong token count | `#r{...} expects 'var op form' or 'form op var op form', got N tokens` |
| Variable is not a single letter | `#r{...}: variable must be a single letter, got '<name>'` |
| No symbol found (both sides are forms) | `#r{...}: expected a single-letter variable on one side` |
| Mixed `op-fwd`/`op-rev` in two-sided | `#r{...}: cannot mix '<'/'<=' and '>'/>=' in a two-sided range` |
| `=` in two-sided position | `#r{...}: '=' is only valid in a one-sided range` |
| Empty range (literal bounds) | `#r{...}: range is provably empty` (warning) |
| Unclosed brace | `#r{...}: unterminated range literal (missing '}')` |
| Unknown operator token | `#r{...}: expected a comparison operator, got '<tok>'` |

---

## Affected Files

| File | Change |
|------|--------|
| `src/compiler/reader.c` | Add `read_range_literal`; dispatch arm in `read_form` |
| `src/compiler/reader.h` | No public API changes |
| `stdlib/float-range.tur` | New file (RR4) |
| `tests/fixtures/range_reader_one_sided.tur` | New fixture |
| `tests/fixtures/range_reader_two_sided.tur` | New fixture |
| `tests/fixtures/range_reader_expr_bounds.tur` | New fixture |
| `tests/fixtures/range_reader_shadow_warn.tur` | New fixture (RR3) |
| `tests/fixtures/range_reader_float.tur` | New fixture (RR4) |
| `stdlib/range.tur` | No changes (constructors already present) |
| `docs/api/` | Regenerate after RR4 adds `float-range.tur` docstrings |

---

## Example Usage

```turmeric no-check
;; Index bounds check with expression upper bound
(defn safe-get [v :int i :int] :int
  (require! (range-contains? #r{0 <= i < (vec-len v)} i)
            "index out of bounds")
  (vec-get v i))

;; Contract type
(defn classify-score [score : { s : :int | (range-contains? #r{1 <= s <= 5} s) }] :cstr
  (cond
    ((= score 5) "excellent")
    ((= score 4) "good")
    ("average")))

;; Seq iteration over an integer range
(for i (seq/from-range #r{0 <= i < 10})
  (println i))

;; Float range membership test
(defn unit-clamp [x :float] :float
  (if (float-range-contains? #r{0.0 <= x <= 1.0} x) x
      (if (< x 0.0) 0.0 1.0)))

;; Right-to-left syntax (same semantics)
(when (range-contains? #r{100 > n >= 0} score)
  (println "in range"))
```
```sweet-exp
;; Index bounds check with expression upper bound
defn safe-get [v :int i :int] :int
  require!(range-contains?(#r{0 <= i < vec-len(v)} i)
           "index out of bounds")
  vec-get(v i)

;; Contract type
defn classify-score [score : { s : :int | range-contains?(#r{1 <= s <= 5} s) }] :cstr
  cond
    {score = 5} "excellent"
    {score = 4} "good"
    "average"

;; Seq iteration over an integer range
for i seq/from-range(#r{0 <= i < 10})
  println(i)

;; Float range membership test
defn unit-clamp [x :float] :float
  if float-range-contains?(#r{0.0 <= x <= 1.0} x)
    x
    if {x < 0.0}
      0.0
      1.0

;; Right-to-left syntax (same semantics)
when range-contains?(#r{100 > n >= 0} score)
  println("in range")
```
