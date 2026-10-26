---
title: Borrow Checking and Lifetimes Guide
category: Type Safety
description: How Turmeric's borrow checker and lifetime elision work, and how to fix the errors they report
tags:
  - borrow-checker
  - lifetimes
  - ownership
  - references
  - aliasing
---

# Borrow Checking and Lifetimes Guide

Turmeric's borrow checker prevents use-after-move, aliasing violations, and
dangling references. Lifetime elision assigns lifetimes to borrow types
(`&T`, `&mut T`) so the checker can validate them across function boundaries.

## Overview

The borrow checker is a compiler pass (`src/passes/borrow_check.c`) that
runs after elaboration and before closure conversion. It combines three
checks:

- **Move tracking** -- a value of linear or affine type may not be used
  after it has been moved.
- **Aliasing rules** -- N readers XOR 1 writer. An immutable borrow (`&T`)
  may coexist with other immutable borrows, but a mutable borrow (`&mut T`)
  is exclusive.
- **Lifetime validation** -- a borrow's referent must outlive the borrow
  itself, so a function cannot return a reference to a local that will be
  freed on return.

Lifetime elision (`src/passes/lifetime_elision.c`) applies Rust-style rules
to assign lifetime parameters when they are not written explicitly. The
always-on lifetime pass (`lifetime_check_program`) runs elision per
top-level function and rejects cyclic outlives-constraint graphs
(TUR-E0106).

## Borrow Types

Turmeric has two borrow types:

| Type | Syntax | Meaning |
|---|---|---|
| Immutable borrow | `&T` | A non-owning reference; the referent may be read but not written |
| Mutable borrow | `&mut T` | An exclusive non-owning reference; the referent may be read and written |

A borrow is created with `(& expr)` (immutable) or `(&mut expr)`
(mutable). The borrow expression kind is `EX_BORROW_IMMUT` or
`EX_BORROW_MUT` in the compiler's IR.

A borrow is read with `@r` (`(println @r)`) and written through with
`(set! @r v)` -- the `set!` form with a deref target. The prefix `@r` is the
only spelling: `(@ r)` is not a call form.

Three rules hold on every engine
([borrow-checks-gaps](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/borrow-checks-gaps.md),
resolved 2026-10-22):

- **`&mut` needs a mutable place.** `(&mut x)` and `(&mut (.f x))` are refused
  unless `x` is a `^mut` local ("cannot borrow `x` as mutable: it is not
  declared `^mut`"). A place behind an `rc`, a `:heap` struct or another borrow
  is interior-mutable and needs nothing.
- **No assignment to a borrowed place while a `let` holds a live borrow of
  it.** `(set! x v)`, or `(set! (.f x) v)` when the borrow covers `.f` (or all
  of `x`), is refused while a `let`-bound borrow of `x` is used after the
  assignment. Liveness is program order: a borrow the body is done with does not
  block the write, a use earlier in an enclosing `while` counts (it runs again
  on the next turn), and a borrow a closure captured is live for good. A borrow
  of a different field (`(& (.a p))` against `(set! (.b p) 5)`) does not
  conflict. An `if` is read in program order too, so a use in the else-branch
  of an `if` whose then-branch assigns counts as a use after the write.
- **A borrow of a literal borrows a temporary.** `(let [r (& 7.25)] ...)` spills
  the literal to a temporary that lives as long as the `let`, and `&mut` of a
  literal writes that temporary.

```turmeric
(let [^mut x 1
      r (& x)]
  (println @r)
  (set! x 2)      ; fine: `r` is not used again
  (println x))

(let [^mut x 1
      r (& x)]
  (set! x 2)      ; error: cannot assign to `x` while it is borrowed by `r`
  (println @r))
```

```sweet-exp
#lang sweet-exp

let [^mut x 1
     r (& x)]
  println(@r)
  set!(x 2)      ; fine: `r` is not used again
  println(x)

let [^mut x 1
     r (& x)]
  set!(x 2)      ; error: cannot assign to `x` while it is borrowed by `r`
  println(@r)
```

On both engines a borrow is a reference to the place: a write through
`&mut (.x p)` reaches `p` (the interpreter used to borrow a copy --
[turi-borrow-is-a-copy](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-borrow-is-a-copy.md), resolved 2026-10-22).

## Move Tracking

A value of a linear type (annotated `^linear`, or `ref<T>` under
`-Xsubstructural`) must be consumed exactly once. After it is moved --
passed to a function, returned, or bound to another name -- using it again
is an error.

```turmeric
;; TUR-E0101: use-after-move
(let [fh (open-file "data.txt")]
  (read-file fh)    ; fh consumed here
  (close-file fh))  ; ERROR: fh already consumed
```

```sweet-exp
#lang sweet-exp

;; TUR-E0101: use-after-move
let [fh open-file("data.txt")]
  read-file(fh)     ; fh consumed here
  close-file(fh)    ; ERROR: fh already consumed
```

Fix: restructure so each linear value flows through exactly one code path.

## Aliasing Rules

The borrow checker enforces "N readers XOR 1 writer":

- Multiple `&T` borrows of the same value may coexist.
- One `&mut T` borrow excludes all other borrows (immutable or mutable).

This prevents data races and ensures a mutable borrow is not invalidated
by a concurrent mutation.

## Lifetime Elision

When a function signature uses borrow types without explicit lifetime
annotations, the compiler applies three Rust-style elision rules:

1. **Each input lifetime is distinct.** Every lifetime in an input type
   becomes a separate lifetime parameter.
2. **Single input lifetime flows to output.** If there is exactly one
   input lifetime, all elided output lifetimes are assigned that lifetime.
3. **`&self` / `&mut self` lifetime.** For methods with a `&self` or
   `&mut self` receiver, the receiver's lifetime is assigned to elided
   output lifetimes.

Today the surface language does not attach `'a` annotations to types, so
elision typically finds nothing to assign. The pass is wired and ready so
that lifetime-annotated signatures are checked the moment that syntax
lands. The elision and constraint-solving logic is unit-tested in
`tests/lifetime_unit.c`.

## Dangling Reference Prevention

The borrow checker validates that a function's return type does not
produce a dangling reference. If a function returns `&T`, the referent
must outlive the borrow -- the lifetime of the returned reference must be
tied to an input lifetime, not to a local that is freed on return.

```turmeric
;; Rejected: returning a borrow of a local
(defn bad [] : &int
  (let [x 42]
    (& x)))  ; ERROR: x does not outlive the borrow
```

```sweet-exp
#lang sweet-exp

;; Rejected: returning a borrow of a local
defn bad [] : &int
  let [x 42]
    (& x)  ; ERROR: x does not outlive the borrow
```

The inter-procedural check uses `result_borrow_arg` -- an index recorded
on the function type that identifies which parameter's lifetime the borrow
return is tied to. At a call site, if the returned borrow outlives the
argument it is tied to, the checker reports an error.

## Effect Handler Capture Checking

An always-on check (not gated by `borrow_check_set_enabled`) rejects
borrow-typed variables from the enclosing scope being referenced inside
effect handler case bodies. Handler case bodies are emitted as top-level
C static functions with no access to the enclosing stack frame, so a
borrow from the outer scope would be a dangling pointer at runtime.

```turmeric
;; Rejected: borrow captured by handler case
(let [x 42
      r (& x)]
  (handle
    (perform (Some))
    (Some [] k)
    (println r)))  ; ERROR: borrow 'r' cannot be captured by an effect handler case
```

This check runs unconditionally on every program.

## Cyclic Lifetime Rejection

The always-on lifetime pass rejects a function whose outlives-constraint
graph contains a cycle (TUR-E0106). A cycle means two distinct lifetimes
are each required to outlive the other, which is unsatisfiable.

```turmeric
;; TUR-E0106: cyclic lifetime constraint
;; 'a: 'b and 'b: 'a -- neither can outlive the other
```

The checker reports the two lifetimes on the cycle for diagnostics.

## Error Reference

| Code | Meaning | Fix |
|---|---|---|
| TUR-E0100 | Linear value dropped without being consumed | Call `chan-free` / `close-file` / etc. before the scope ends |
| TUR-E0101 | Linear value used after being moved or consumed | Restructure so each linear value flows through one code path |
| TUR-E0106 | Cyclic lifetime constraint graph | Remove one of the conflicting outlives constraints |

## Enabling the Full Borrow Checker

The move-tracking and aliasing checks are gated behind
`borrow_check_set_enabled` (default: disabled for incremental rollout).
The effect handler capture check and the cyclic lifetime check are
always on.

To enable the full borrow checker:

```sh
tur -Xlinear myfile.tur
tur -Xsubstructural myfile.tur
```

See [substructural-types-guide.md](substructural-types-guide.md) for the
`^linear` / `^affine` / `^relevant` disciplines and
[uniqueness-types-guide.md](uniqueness-types-guide.md) for `^unique`.

## What This Guide Does Not Cover

- **Substructural type disciplines** -- `^linear`, `^affine`, `^relevant`
  are covered in [substructural-types-guide.md](substructural-types-guide.md).
- **Ownership strategy** -- which ownership pattern to reach for
  (persistent-immutable, single-owner mutable, `rc<T>`, `weak<T>`) is
  covered in [ownership-guide.md](ownership-guide.md).
- **RC elision** -- the compiler pass that eliminates refcount inc/dec
  is an optimization detail; see [gc-guide.md](gc-guide.md) for the memory
  management model.
