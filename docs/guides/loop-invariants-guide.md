---
title: Loop Invariants Guide
category: Refinement Types
description: Proving loop properties with :invariant annotations on while loops
---

# Loop Invariants Guide

A `while` loop may carry a `:invariant` annotation that the compiler
attempts to prove statically. When the proof succeeds, the runtime checks
are elided and the invariant becomes a known fact after the loop. When the
proof fails or cannot be decided, the runtime checks remain as a safety
net.

## Overview

Loop invariants graduated to always-on in v0.62.0. The `--enable=loop-invariants`
flag is now a `TUR-W0063` no-op. Every `(while c :invariant p body...)`
form is analysed by the refinement solver -- the same in-house decision
procedure that discharges `#refine{...}` annotations.

The invariant owes two proofs:

1. **Entry** -- the facts that reach the loop (branch conditions, let
   bindings, parameter refinements, an earlier loop's post-fact) must
   imply `p` on entry.
2. **Preservation** -- from a state satisfying `p` and the loop condition
   `c`, one pass of the body must leave `p` true.

When both proofs succeed, the runtime checks are elided and the post-loop
fact `p AND (not c)` is offered to the solver as a known fact. When either
proof fails, the runtime check stays (checked on entry and after every
iteration), and the program remains safe.

## Syntax

```turmeric
(while condition :invariant predicate body...)
```

The `:invariant` keyword must directly follow the condition. At most one
`:invariant` per loop -- combine multiple conjuncts with `and`:

```turmeric no-check
(while (< i n)
  :invariant (and (>= i 0) (<= i n) (>= acc 0))
  (set! acc (+ acc i))
  (set! i (+ i 1)))
```

```sweet-exp
#lang sweet-exp

while <(i n)
  :invariant and(>=(i 0) <=(i n) >=(acc 0))
  set!(acc {acc + i})
  set!(i {i + 1})
```

## Quick Start

### A proved invariant

```turmeric no-check
;; Sum 0..n-1; the invariant is inductive
(let [i 0 acc 0]
  (while (< i n)
    :invariant (and (>= i 0) (<= i n) (>= acc 0))
    (set! acc (+ acc i))
    (set! i (+ i 1)))
  acc)
```

```sweet-exp
#lang sweet-exp

;; Sum 0..n-1; the invariant is inductive
let [i 0 acc 0]
  while <(i n)
    :invariant and(>=(i 0) <=(i n) >=(acc 0))
    set!(acc {acc + i})
    set!(i {i + 1})
  acc
```

The solver proves both obligations: `i` starts at 0 (entry), and each
iteration increments `i` by 1 while `i < n`, so `0 <= i <= n` and
`acc >= 0` are preserved. The runtime checks are elided, and after the
loop, `i >= 0 AND i <= n AND acc >= 0 AND NOT (i < n)` is a known fact.

### A non-inductive invariant

```turmeric no-check
;; acc could go negative if i is negative
(while (< i n)
  :invariant (>= acc 0)        ; not inductive: i may be negative
  (set! acc (+ acc i))
  (set! i (+ i 1)))
```

```sweet-exp
#lang sweet-exp

;; acc could go negative if i is negative
while <(i n)
  :invariant >=(acc 0)        ; not inductive: i may be negative
  set!(acc {acc + i})
  set!(i {i + 1})
```

The solver cannot prove preservation: if `i` is negative, `(+ acc i)`
could make `acc` negative. The runtime check stays, and the solver
reports the counterexample naming the variable the invariant says too
little about. The fix is a stronger invariant:

```turmeric no-check
(while (< i n)
  :invariant (and (>= acc 0) (>= i 0))   ; inductive
  (set! acc (+ acc i))
  (set! i (+ i 1)))
```

```sweet-exp
#lang sweet-exp

while <(i n)
  :invariant and(>=(acc 0) >=(i 0))     ; inductive
  set!(acc {acc + i})
  set!(i {i + 1})
```

## How It Works

### The two obligations

Each `:invariant p` on a `(while c body...)` produces two verification
conditions:

1. **Entry proof:** `premises => p` -- the facts in scope at the loop
   entry (parameter refinements, prior branch conditions, let bindings,
   earlier loop post-facts) must imply `p`.
2. **Preservation proof:** `(and p c) => p_after` -- assuming `p` holds
   and the loop condition `c` is true, executing the body once must leave
   `p` true. For a branching body, the solver checks each path.

### What the solver can prove

The in-house decision procedure handles quantifier-free linear integer
arithmetic with equality and uninterpreted functions. It runs a chain of
stages: normalization, EUF (uninterpreted functions), Fourier-Motzkin
(linear arithmetic), Nelson-Oppen (theory combination), and cube
expansion (propositional DNF). See
[refinement-solver-internals-guide.md](refinement-solver-internals-guide.md)
for the full pipeline.

### When the analysis is declined

The loop is declined (no static analysis, both runtime checks stay) when
the body:

- Assigns through a place (`(set! (.f s) v)`) or an atom.
- Can leave early (`return`, `?`, a continuation) or nests a loop that
  assigns.
- Borrows a variable the loop depends on (`(& x)`, `&mut x`), or the
  function assigns it inside a lambda or handler clause.
- Reads a field, a deref, or a mutable global in the invariant or the
  condition.
- Rebinds, in a body `let`, a name that is already in scope.

A declined loop reports `TUR-W0372` with "is not analysed statically:
<reason>". The runtime checks stay.

### Frozen regions

Loop invariants prove inside a `frozen` region. A bounded-index walk over
a container discharges both runtime checks there, and a frozen marker no
longer declines the loop. See
[stateful-refinements-guide.md](stateful-refinements-guide.md) for the
`frozen` region and `#reads` measures.

## Diagnostics

### TUR-W0372: could not be proved

When the solver returns "unknown" for an invariant obligation, the
runtime check stays. This is a sound outcome, not a miscompile. The
diagnostic reports whether the failure was on entry ("could not be proved
on entry") or preservation ("...to be preserved by the body").

Common causes:

- The predicate falls outside the supported fragment (nonlinear
  arithmetic, quantifiers).
- A nonlinear subterm was abstracted away.
- The propositional structure exceeded the small-DNF cap.

Adding an explicit refinement to a parameter usually supplies the missing
hypothesis. `--strict-refine` turns this into a hard error for builds that
want every obligation discharged statically.

### Strengthening an invariant

The usual fix for a failed preservation proof is a **stronger** invariant.
The counterexample names the variable the invariant says too little
about. In the example above, `(>= acc 0)` alone is not inductive because
`i` could be negative; adding `(>= i 0)` gives the solver the hypothesis
it needs.

## Interaction with Refinement Types

Loop invariants are part of the refinement type system. A `#refine{...}`
annotation on a parameter or let binding provides a premise the solver
can use when proving the entry obligation. After the loop, the proved
invariant and the negated condition become facts the solver can use for
downstream obligations.

```turmeric no-check
;; The parameter refinement gives the solver i >= 0 on entry
(defn sum-to [n : #refine{ v : int | (>= v 0) }] : int
  (let [i 0 acc 0]
    (while (< i n)
      :invariant (and (>= i 0) (<= i n) (>= acc 0))
      (set! acc (+ acc i))
      (set! i (+ i 1)))
    acc))
```

```sweet-exp
#lang sweet-exp

;; The parameter refinement gives the solver i >= 0 on entry
defn sum-to [n : #refine{ v : int | >=(v 0) }] : int
  let [i 0 acc 0]
    while <(i n)
      :invariant and(>=(i 0) <=(i n) >=(acc 0))
      set!(acc {acc + i})
      set!(i {i + 1})
    acc
```

See [refinement-types-guide.md](refinement-types-guide.md) for the full
refinement type system and `--strict-refine`.

## What This Guide Does Not Cover

- **Refinement types** -- the `#refine{...}` annotation, call-site
  crossings, and `--strict-refine` are covered in
  [refinement-types-guide.md](refinement-types-guide.md).
- **Stateful refinements** -- `#reads` measures and the `frozen` region
  are covered in [stateful-refinements-guide.md](stateful-refinements-guide.md).
- **Solver internals** -- the decision procedure pipeline is covered in
  [refinement-solver-internals-guide.md](refinement-solver-internals-guide.md).
- **Runtime contracts** -- `assert!`, `require!`, `ensure!`, and
  `invariant!` macros are covered in
  [error-handling-guide.md](error-handling-guide.md).
