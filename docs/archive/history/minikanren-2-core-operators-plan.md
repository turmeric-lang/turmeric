---
title: "MiniKanren Part 2: Core Operators -- Plan"
category: Tutorials and Examples (Plan)
description: Plan for the second miniKanren tutorial -- implementing ==, fresh, conde, run, and constraints in Turmeric
---

# Plan: MiniKanren Part 2 -- Core Operators

This document plans the second miniKanren tutorial guide.
Part 1 ([minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md))
introduced relational programming using explicit loops and helper predicates.
Part 2 implements the real miniKanren engine inside Turmeric, giving the reader
working `==`, `fresh`, `conde`, `run`, and optional constraint operators.

Inspiration: the "Next steps" section of Part 1 and the
[webmk interactive tutorial](https://io.livecode.ch/learn/webyrd/webmk).

---

## Target audience

Readers who have completed Part 1 and want to understand how miniKanren works
from first principles and how to use it idiomatically in Turmeric.

---

## Code location

`examples/minikanren2/src/main.tur` (new example directory).

---

## Outline

### Section 1 -- Representing terms

Introduce the `Term` sum type used throughout the engine:

- `(LVar id)` -- a logic variable, identified by a unique integer
- `(Sym name)` -- an atom / symbol (a `:cstr`)
- `(Pair car cdr)` -- a cons pair (used to encode lists and structured data)
- `(Nil)` -- the empty list / null term

Show how domain values from Part 1 (integer person IDs, string names) are
lifted into `Term`.

### Section 2 -- Substitutions and walking

A *substitution* is a `Map` from `LVar` id to `Term`. Introduce:

- `(empty-subst)` -- the identity substitution
- `(walk t s)` -- chase variable bindings until a non-variable or unbound var
- `(ext-s x t s)` -- extend the substitution (fails / returns `none` on occurs check)

Explain why `walk` must loop (chains of variable-to-variable bindings).

### Section 3 -- Unification (`==`)

Unification tries to make two terms identical under a substitution:

```turmeric
;;; unify -- attempt to unify two terms under substitution s.
;;;
;;; Returns: (some subst) on success, (none) on failure.
(defn unify [u v s] ...)
```
```sweet-exp
;;; unify -- attempt to unify two terms under substitution s.
;;;
;;; Returns: (some subst) on success, (none) on failure.
defn unify [u v s]
  ...
```

Cover:
- Unifying two atoms
- Unifying a variable with a term (occurs check)
- Unifying two pairs recursively
- Unifying two already-walked values

Show the `==` goal constructor that wraps `unify`:

```turmeric
(defn == [u v] ...)
```
```sweet-exp
defn == [u v]
  ...
```

Demonstrate:
```turmeric
(run 1 (q) (== q 3))        ; => (3)
(run 1 (q) (== 4 3))        ; => ()
(run 1 (x) (fresh (y) (== x y) (== y 7)))  ; => (7)
```
```sweet-exp
run(1 q() ==(q 3))
; => (3)
run(1 q() ==(4 3))
; => ()
run(1 x() fresh(y() ==(x y) ==(y 7)))
; => (7)
```

### Section 4 -- Goals and the goal type

A *goal* is a function `Subst -> Stream<Subst>` (a lazy stream of substitutions).
Introduce the `Goal` type alias and the `Stream` type (lazy list / coroutine).

Show goal combinators:

- `(succeed)` -- always succeeds, returns the input substitution unchanged
- `(fail)` -- always fails, returns the empty stream
- `(conj g1 g2)` -- conjunction: `g1` then `g2` on each result of `g1`
- `(disj g1 g2)` -- disjunction: interleave results of `g1` and `g2`

### Section 5 -- `fresh`

`fresh` introduces new logic variables and conjoins goals over them:

```turmeric
(fresh (x y)
  (== x 1)
  (== y 2))
```
```sweet-exp
fresh(x(y) ==(x 1) ==(y 2))
```

Implement as a macro that allocates fresh `LVar` ids and builds a `conj` chain.

Demonstrate shadowing (a `fresh` variable can shadow an outer one) using the
same example from webmk where the inner `y` differs from the outer `y`.

### Section 6 -- `conde`

`conde` provides disjunction in clausal form (each clause is a conjunction):

```turmeric
(conde
  ((== q :cat))
  ((== q :dog))
  ((== q :fish)))
```
```sweet-exp
conde(==(q :cat)() ==(q :dog)() ==(q :fish)())
```

Implement as interleaved `disj` of `conj` chains. Emphasise:
- Each clause is tried independently (the substitution is "refreshed" per clause)
- Answers are interleaved, not depth-first, so diverging clauses do not starve finite ones
- Use `run N` to limit output; show `run*` causes divergence with recursive relations

Example with multiple answers:
```turmeric
(run 3 (q)
  (conde
    ((== q :a))
    ((== q :b))
    ((== q :c))))
; => (:a :b :c)
```
```sweet-exp
run(3 q() conde(==(q :a)() ==(q :b)() ==(q :c)()))
; => (:a :b :c)
```

### Section 7 -- `run` and `run*`

`run` is the top-level interface between Turmeric and the miniKanren engine:

```turmeric
(run N (q) goal ...)
```
```sweet-exp
run(N q() goal ...)
```

Steps:
1. Allocate a fresh query variable `q`
2. Build the goal as a conjunction
3. Run the goal on the empty substitution
4. Take the first `N` substitutions from the stream
5. Reify each: walk `q` fully, replace remaining unbound vars with `_.0`, `_.1`, ...

Discuss `run*` (no limit) and its interaction with diverging goals.

### Section 8 -- Reification

Reification converts an internal substitution + term into a readable answer:

- Walk the term fully under the substitution
- Replace each unbound `LVar` with a display name `_.0`, `_.1`, ...
  (numbered left-to-right within each answer, restarting from 0 per answer)

Show the reified output for:
```turmeric
(run 1 (q) succeed)           ; => (_.0)   -- q is unbound
(run 1 (q) (== q (list 1 q))) ; => ()      -- fails: occurs check
```
```sweet-exp
run(1 q() succeed)
; => (_.0)   -- q is unbound
run(1 q() ==(q list(1 q)))
; => ()      -- fails: occurs check
```

### Section 9 -- Putting it together: family graph revisited

Re-encode the Part 1 family graph using the new engine:

```turmeric
(defn parento [parent child]
  (conde
    ((== parent :abe)   (== child :homer))
    ((== parent :homer) (== child :bart))
    ((== parent :homer) (== child :lisa))
    ...))

(defn grandparento [grand child]
  (fresh (mid)
    (parento grand mid)
    (parento mid child)))
```
```sweet-exp
defn parento [parent child]
  conde(==(parent :abe)(==(child :homer)) ==(parent :homer)(==(child :bart)) ==(parent :homer)(==(child :lisa)) ...)
defn grandparento [grand child]
  fresh(mid() parento(grand mid) parento(mid child))
```

Show the same queries as Part 1, now written with `run`:
```turmeric
(run* (p) (parento p :bart))         ; parents of Bart
(run* (c) (parento :homer c))        ; children of Homer
(run* (g) (grandparento g :bart))    ; grandparents of Bart
(run* (p c) (parento p c))           ; all pairs
```
```sweet-exp
run*(p() parento(p :bart))
; parents of Bart
run*(c() parento(:homer c))
; children of Homer
run*(g() grandparento(g :bart))
; grandparents of Bart
run*(p(c) parento(p c))
; all pairs
```

Contrast with Part 1 to show the gain in expressiveness.

### Section 10 (optional / stretch) -- Constraint operators

If time permits, sketch the constraint extensions from cKanren:

- `=/=` (disequality): `(run* (p) (=/= p 1))` -- `p` is unbound but cannot be `1`
- `symbolo` / `numbero`: type constraints propagated through unification
- `absento`: ensure a tag symbol does not appear anywhere in a term

Each constraint is stored alongside the substitution and checked / simplified
whenever new bindings are added.

---

## Implementation notes

### Streams

The lazy stream type can be implemented with Turmeric's existing coroutine / effect
support or as a simple thunk-based lazy list:

```turmeric
;; A Stream<A> is one of:
;;   (Empty)
;;   (Cons head tail)    -- eager head, lazy tail (thunk)
;;   (Thunk f)           -- suspended computation
```
```sweet-exp
;; A Stream<A> is one of:
;;   (Empty)
;;   (Cons head tail)    -- eager head, lazy tail (thunk)
;;   (Thunk f)           -- suspended computation
```

Interleaving (`disj`) alternates between two streams by forcing one step at a
time -- this is the core of miniKanren's fair search.

### Occurs check

Enable the occurs check in `unify` by default to match standard miniKanren
behaviour. Provide a note that some Scheme miniKanren implementations omit it
for performance.

### Fresh variable counter

A simple monotonic integer counter (passed via a state monad or a mutable ref)
provides fresh `LVar` ids. Document the chosen approach.

---

## Files to create

| File | Purpose |
|------|---------|
| `examples/minikanren2/build.tur` | Spice manifest |
| `examples/minikanren2/src/main.tur` | Tutorial driver |
| `examples/minikanren2/src/mk/term.tur` | `Term` type + constructors |
| `examples/minikanren2/src/mk/subst.tur` | Substitution map, `walk`, `ext-s` |
| `examples/minikanren2/src/mk/unify.tur` | `unify`, `==` goal |
| `examples/minikanren2/src/mk/goal.tur` | `Goal` type, `succeed`, `fail`, `conj`, `disj` |
| `examples/minikanren2/src/mk/fresh.tur` | `fresh` macro |
| `examples/minikanren2/src/mk/conde.tur` | `conde` macro |
| `examples/minikanren2/src/mk/run.tur` | `run`, `run*`, reification |
| `examples/minikanren2/src/mk/constraints.tur` | (stretch) `=/=`, `symbolo`, `numbero`, `absento` |

---

## Cross-references

- Part 1: [minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md)
- Backtracking background: [backtracking-guide.md](backtracking-guide.md)
- Logic programming overview: [logic-programming-guide.md](logic-programming-guide.md)
- Datalog series (related relational approach): [datalog-01-concepts.md](datalog-01-concepts.md)
