---
title: tur/logic -- Logic Programming Guide
category: Advanced Control Flow
description: How to use and extend tur/logic for miniKanren-style relational programming in Turmeric
---

# `tur/logic` -- Logic Programming Guide

`tur/logic` provides miniKanren-style logic programming: unification, logic
variables, goals, and a backtracking search engine.  The module lives in
`stdlib/logic.tur` and is pure Turmeric (no inline-C): terms, substitutions,
and the solution stream are `defdata` sum types, so the engine runs identically
under the compiled path and the tree-walking interpreter.

---

## Background: what is miniKanren?

miniKanren is a small, embeddable logic programming language pioneered by
William Byrd, Daniel Friedman, and collaborators.  Its central idea is that
programs are written as *relations* rather than functions, so the same
relation can be queried in multiple directions:

- given `x`, find all `y` such that `parento(x, y)` holds;
- given `y`, find all `x` such that `parento(x, y)` holds;
- enumerate all `(x, y)` pairs for which the relation holds.

The key primitives are:

| Concept | miniKanren name | `tur/logic` name |
|---------|-----------------|-----------------|
| Succeed unconditionally | `succeed` | `(succeed)` |
| Fail unconditionally | `fail` | `(fail)` |
| Unify two terms | `==` | `(lequal t1 t2)` |
| Logical AND | `fresh`/`conj` | `(conjoined g1 g2)` |
| Logical OR | `conde`/`disj` | `(disjoined g1 g2)` |
| Introduce a variable | `fresh` | `(fresh (fn [x] ...))` |
| Run a goal | `run` | `(run-logic n goal)` |

---

## Quick start

```turmeric
(import tur/logic :refer [term-int term-var term-nil term-pair
                           subs-empty
                           lequal succeed fail conjoined disjoined fresh
                           run-logic first-state reify-walk
                           term-int-val logic-walk bt-length])

;; Q: is 5 equal to 5?
(let [results (run-logic 1 (lequal (term-int 5) (term-int 5)))]
  (println (bt-length results)))   ; => 1

;; Q: what value does x have if x == 7?
(let [x      (term-var 0)
      goal   (lequal x (term-int 7))
      result (run-logic 1 goal)
      walked (reify-walk x result)]
  (println (term-int-val walked)))  ; => 7
```
```sweet-exp
import tur/logic :refer [term-int term-var term-nil term-pair
                           subs-empty
                           lequal succeed fail conjoined disjoined fresh
                           run-logic first-state reify-walk
                           term-int-val logic-walk bt-length]
;; Q: is 5 equal to 5?
let [results (run-logic 1 (lequal (term-int 5) (term-int 5)))]
  println(bt-length(results))
; => 1
;; Q: what value does x have if x == 7?
let [x      (term-var 0)
      goal   (lequal x (term-int 7))
      result (run-logic 1 goal)
      walked (reify-walk x result)]
  println(term-int-val(walked))
; => 7
```

---

## Core concepts

### Terms

A *term* is a `defdata` sum type (`Term`) with four constructors:

| Kind | Constructor (helper / raw) | Accessor(s) |
|------|----------------------------|-------------|
| integer constant | `(term-int n)` / `(TInt n)` | `(term-int-val t)` |
| logic variable | `(term-var id)` / `(TVar id)` | `(term-var-id t)` |
| pair | `(term-pair a b)` / `(TPair a b)` | `(term-pair-fst t)`, `(term-pair-snd t)` |
| nil | `(term-nil)` / `(TNil)` | -- |

Inspect a term's shape with `match`:

```turmeric
(match t
  (TInt n)    ...
  (TVar id)   ...
  (TPair a b) ...
  (TNil)      ...)
```

Pairs nest to form lists in the usual cons-cell style:

```turmeric
;; build the list [1, 2, nil]
(let [lst (term-pair (term-int 1)
                     (term-pair (term-int 2) (term-nil)))]
  ...)
```
```sweet-exp
;; build the list [1, 2, nil]
let [lst (term-pair (term-int 1)
                     (term-pair (term-int 2) (term-nil)))]
  ...
```

### Logic variables

Every variable is identified by an integer id.  Inside a goal, `(fresh ...)`
allocates the next unused id from the search state itself (the substitution's
base node carries the fresh-variable counter), so scoped variables never
collide.  For a top-level query variable, construct one by hand with ids
numbered from 0:

```turmeric
(let [x (term-var 0)
      y (term-var 1)]
  ...)
```
```sweet-exp
let [x (term-var 0)
      y (term-var 1)]
  ...
```

Prefer `(fresh ...)` (below) over hand-numbered ids -- it keeps variable
scope explicit and threads the counter for you.

### Substitutions

A *substitution* maps variable ids to terms.  It is a persistent `defdata`
association list (`Subst`): `(SBind var-id term rest)` binding nodes over a
base `(SNil next)` that carries the next unused fresh-variable id.  The empty
substitution is `(subs-empty)`.

You rarely manipulate substitutions directly; `run-logic` starts from the
empty one and `reify-walk` lets you look up results.

### Goals

A *goal* is an opaque `(Goal A)` handle over a function `Subst -> Stream` --
it accepts a substitution and returns zero or more extended substitutions
(the solution `Stream`).  `(apply-goal g state)` runs one directly.  `Goal`
also carries Functor / Applicative / Monad / Alternative instances, so goals
compose with `do-m` (conjunction) and `alt-or` (disjunction) as well.

The built-in goal constructors:

```turmeric
;; Primitives
(succeed)                         ; always succeeds
(fail)                            ; always fails
(lequal t1 t2)                    ; unify t1 and t2

;; Combination
(conjoined g1 g2)                 ; g1 AND g2 (sequential)
(disjoined g1 g2)                 ; g1 OR  g2 (parallel streams)

;; Variable introduction
(fresh (fn [x] body-goal))        ; create one fresh variable x
```
```sweet-exp
;; Primitives
succeed()
; always succeeds
fail()
; always fails
lequal(t1 t2)
; unify t1 and t2
;; Combination
conjoined(g1 g2)
; g1 AND g2 (sequential)
disjoined(g1 g2)
; g1 OR  g2 (parallel streams)
;; Variable introduction
fresh((fn [x] body-goal))
; create one fresh variable x
```

### Running a goal

```turmeric
(run-logic n goal)
```
```sweet-exp
run-logic(n goal)
```

Returns a solution `Stream` of at most `n` substitutions.  Use helper
functions to extract results:

```turmeric
(first-state results)             ; first substitution, or (subs-empty)
(reify-walk term results)         ; walk a term through the first substitution
(bt-length results)               ; number of solutions
```
```sweet-exp
first-state(results)
; first substitution, or (subs-empty)
reify-walk(term results)
; walk a term through the first substitution
bt-length(results)
; number of solutions
```

---

## Examples

### Unification failure

```turmeric
(let [results (run-logic 1 (lequal (term-int 1) (term-int 2)))]
  (println (bt-length results)))   ; => 0 (no solutions)
```
```sweet-exp
let [results (run-logic 1 (lequal (term-int 1) (term-int 2)))]
  println(bt-length(results))
; => 0 (no solutions)
```

### Disjunction -- multiple answers

```turmeric
(let [x   (term-var 0)
      g   (disjoined (lequal x (term-int 1))
                     (disjoined (lequal x (term-int 2))
                                (lequal x (term-int 3))))
      res (run-logic 5 g)]
  (println (bt-length res)))       ; => 3
```
```sweet-exp
let [x   (term-var 0)
      g   (disjoined (lequal x (term-int 1))
                     (disjoined (lequal x (term-int 2))
                                (lequal x (term-int 3))))
      res (run-logic 5 g)]
  println(bt-length(res))
; => 3
```

### Fresh variables

`fresh` is the idiomatic way to introduce a scoped logic variable:

```turmeric
(let [res (run-logic 1
            (fresh (fn [x]
              (conjoined
                (lequal x (term-int 42))
                (succeed)))))]
  ;; the fresh variable received id 0 (the counter starts at 0)
  (println (term-int-val (reify-walk (term-var 0) res))))  ; => 42
```
```sweet-exp
let [res (run-logic 1
            (fresh (fn [x]
              (conjoined
                (lequal x (term-int 42))
                (succeed)))))]
  ;; the fresh variable received id 0 (the counter starts at 0)
  println(term-int-val(reify-walk(term-var(0) res)))
; => 42
```

### Pair unification

```turmeric
(let [x   (term-var 0)
      y   (term-var 1)
      lhs (term-pair x (term-int 2))
      rhs (term-pair (term-int 1) y)
      res (run-logic 1 (lequal lhs rhs))]
  ;; after unification: x=1, y=2
  (println (term-int-val (reify-walk x res)))  ; => 1
  (println (term-int-val (reify-walk y res)))) ; => 2
```
```sweet-exp
let [x   (term-var 0)
      y   (term-var 1)
      lhs (term-pair x (term-int 2))
      rhs (term-pair (term-int 1) y)
      res (run-logic 1 (lequal lhs rhs))]
  ;; after unification: x=1, y=2
  println(term-int-val(reify-walk(x res)))
  ; => 1
  println(term-int-val(reify-walk(y res)))
; => 2
```

### Conjunction chain

Build a chain of goals with `conjoined`:

```turmeric
;; helper: (conjoin-all gs) folds a cons list of goals with conjoined
(defn conjoin-all [gs : int] : (Goal int)
  (if (= (tail gs) 0)
    (:: (head gs) (Goal int))
    (conjoined (:: (head gs) (Goal int)) (conjoin-all (tail gs)))))
```
```sweet-exp
;; helper: (conjoin-all gs) folds a cons list of goals with conjoined
defn conjoin-all [gs : int] : (Goal int)
  if {tail(gs) = 0}
    (:: (head gs) (Goal int))
    conjoined((:: (head gs) (Goal int)) conjoin-all(tail(gs)))
```

### Family-tree relations

```turmeric
;; Encode people as integers; 0=Alice, 1=Bob, 2=Carol, 3=Dave
(defn parento [parent : Term child : Term] : (Goal int)
  (disjoined (conjoined (lequal parent (term-int 0))
                        (lequal child  (term-int 1)))
             (disjoined (conjoined (lequal parent (term-int 0))
                                   (lequal child  (term-int 2)))
                        (conjoined (lequal parent (term-int 1))
                                   (lequal child  (term-int 3))))))

;; grandparento via fresh intermediate variable
(defn grandparento [grand : Term child : Term] : (Goal int)
  (fresh (fn [mid]
    (conjoined (parento grand mid)
               (parento mid child)))))

;; Query: who are the grandchildren of Alice (id=0)?
(let [child (term-var 0)
      res   (run-logic 10 (grandparento (term-int 0) child))]
  ;; walks each solution
  ...)
```
```sweet-exp
;; Encode people as integers; 0=Alice, 1=Bob, 2=Carol, 3=Dave
defn parento [parent : Term child : Term] : (Goal int)
  disjoined(conjoined(lequal(parent term-int(0)) lequal(child term-int(1))) disjoined(conjoined(lequal(parent term-int(0)) lequal(child term-int(2))) conjoined(lequal(parent term-int(1)) lequal(child term-int(3)))))
;; grandparento via fresh intermediate variable
defn grandparento [grand : Term child : Term] : (Goal int)
  fresh((fn [mid] conjoined(parento(grand mid) parento(mid child))))
;; Query: who are the grandchildren of Alice (id=0)?
let [child (term-var 0)
      res   (run-logic 10 (grandparento (term-int 0) child))]
  ;; walks each solution
  ...
```

---

## Extending `tur/logic`

### Custom constraints

`tur/logic` uses *syntactic unification* without an occurs check.  You can
add arithmetic constraints or domain restrictions by building goals that
inspect terms:

```turmeric
;; goal: t must walk to an integer in the range [lo, hi]
(defn range-goal [t : Term lo : int hi : int] : (Goal int)
  (:: (fn [state : Subst]
        (match (logic-walk t state)
          (TInt v) (if (and (>= v lo) (<= v hi))
                     (mreturn state)
                     (mzero))
          _        (mzero)))                    ; not ground -- fail
    :Goal))
```
```sweet-exp
;; goal: t must walk to an integer in the range [lo, hi]
defn range-goal [t : Term lo : int hi : int] : (Goal int)
  ::
    (fn [state : Subst]
      (match logic-walk(t state)
        (TInt v) (if {{v >= lo} and {v <= hi}}
                   mreturn(state)
                   mzero())
        _        mzero()))                     ; not ground -- fail
    :Goal
```

### Reification helpers

`reify-walk` follows a single substitution.  To pretty-print a full term
tree, walk recursively:

```turmeric
(defn reify-term [t : Term subs : Subst] : cstr
  (match (logic-walk t subs)
    (TInt n)    (int->cstr n)
    (TVar id)   "_"
    (TPair a b) (str-concat "(" (str-concat (reify-term a subs)
                                            (str-concat " . " (str-concat (reify-term b subs) ")"))))
    (TNil)      "nil"))
```
```sweet-exp
defn reify-term [t : Term subs : Subst] : cstr
  match logic-walk(t subs)
    TInt(n)
    int->cstr(n)
    TVar(id)
    "_"
    TPair(a b)
    str-concat("(" str-concat(reify-term(a subs) str-concat(" . " str-concat(reify-term(b subs) ")"))))
    TNil()
    "nil"
```

Each `str-concat` / `int->cstr` here returns a fresh "caller frees" `cstr`, and
the recursive nesting leaks every intermediate while handing back a heap buffer
the caller must remember to free. When the reified text is a value you *return*
and pass around, prefer an owned `String`: build it with `stdlib/string.tur`'s
`StringBuilder` (`builder/push-cstr!` the constant pieces, `builder/push-string!`
the recursive results, `builder/finish` to freeze), or wrap the final buffer in
`string/adopt-cstr`. The result owns its bytes and frees exactly once. See
[strings-guide.md](strings-guide.md).

### `conde` macro

The classic miniKanren `conde` is syntactic sugar over nested `disjoined` /
`conjoined`.  You can define it as a Turmeric macro:

```turmeric
;; (conde [g1 g2 ...] [g3 g4 ...] ...)
;; each clause is a conjunction; clauses are disjoined
(defmacro conde [& clauses]
  (if (= (tail clauses) 0)
    `(conjoin-all ~(head clauses))
    `(disjoined (conjoin-all ~(head clauses))
                (conde ~@(tail clauses)))))
```
```sweet-exp
;; (conde [g1 g2 ...] [g3 g4 ...] ...)
;; each clause is a conjunction; clauses are disjoined
defmacro conde [& clauses]
  if {tail(clauses) = 0}
    `(conjoin-all ~(head clauses))
    `(disjoined (conjoin-all ~(head clauses))
                (conde ~@(tail clauses)))
```

### Interleaving search

`disjoined` uses `mplus` (`st-append`), which interleaves: it hands the other
branch the next turn whenever it meets an immature stream.  This section used
to tell you to hand-write that yourself, like this:

```turmeric
;;; mplus-i -- interleaved (BFS) concatenation of two solution streams.
(defn mplus-i [xs : Stream ys : Stream] : Stream
  ;; swap xs and ys at every step so solutions alternate
  (match xs
    (StNil)         ys
    (StCons v rest) (StCons v (mplus-i ys rest))))
```
```sweet-exp
;;; mplus-i -- interleaved (BFS) concatenation of two solution streams.
defn mplus-i [xs : Stream ys : Stream] : Stream
  ;; swap xs and ys at every step so solutions alternate
  match xs
    StNil()
    ys
    StCons(v rest)
    StCons(v mplus-i(ys rest))
```

**You no longer need to define this.** As of the lazy-stream change,
`st-append` -- the `mplus` behind `disjoined` -- IS the interleaving version:

```turmeric
(defn st-append [xs : Stream ys : Stream] : Stream
  (match xs
    (StNil)         ys
    (StInc th)      (StInc (fn [] (st-append ys (st-force th))))   ;; SWAP
    (StCons v rest) (StCons v (st-append rest ys))))
```

The swap on the `StInc` arm is the whole of fair interleaving.

> **Why the hand-written `mplus-i` this section used to recommend could never
> work.** It swapped the arguments but built a strict `StCons`, whose tail is
> evaluated before the cell exists -- so it reordered a finite enumeration and
> nothing else. Interleaving is only half of miniKanren's `mplus`; the other
> half is that the result is **itself immature**, which is what `StInc` supplies.
> Without a thunk no infinite `Stream` value can exist at all, so there was
> nothing for the swap to buy.

### Recursive relations need `zzz`

`disjoined` takes its goals strictly, so a self-recursive relation would
otherwise diverge while the goal is being *built*, before any search runs:

```turmeric
(defn nats [] : (Goal int) (disjoined (succeed) (nats)))        ;; SIGSEGV
(defn nats [] : (Goal int) (disjoined (succeed) (zzz (nats))))  ;; fine
```

`zzz` is a macro and has to be: a function would evaluate its argument at the
call site, which is the divergence it exists to prevent. With it,
`(run-logic 1 (nats))` returns one solution off an infinite relation instead of
never returning.

### Tabling / memoisation

For relations that revisit the same subgoal, add a memo table keyed on
`(goal-id, substitution)`:

```turmeric
;; memo-table: map from (goal-name x subs-hash) -> result-list
;; Use tur/hamt for the persistent map.
(import tur/hamt :refer [hamt-empty hamt-insert hamt-lookup])

(defn tabled [name goal-fn args subs] : int
  (let [key  (hash-key name args subs)
        memo (hamt-lookup *memo-table* key)]
    (if (option-some? memo)
      (option-unwrap memo)
      (let [result (apply-goal (goal-fn args) subs)]
        (set! *memo-table* (hamt-insert *memo-table* key result))
        result))))
```
```sweet-exp
;; memo-table: map from (goal-name x subs-hash) -> result-list
;; Use tur/hamt for the persistent map.
import tur/hamt :refer [hamt-empty hamt-insert hamt-lookup]
defn tabled [name goal-fn args subs] :int
  let [key  (hash-key name args subs)
        memo (hamt-lookup *memo-table* key)]
    if option-some?(memo)
      option-unwrap(memo)
      let [result (apply-goal (goal-fn args) subs)]
        set!(*memo-table* hamt-insert(*memo-table* key result))
        result
```

---

## Integration with `tur/backtrack`

`tur/logic` defines its own copies of the backtracking monad primitives
(`mzero`, `mreturn`, `mplus`, `mbind`) over its typed `Stream`.  If you are
building tools on top of the same monad without the full logic layer,
`stdlib/backtrack.tur` exports these primitives separately.

---

## Performance notes

- Substitutions are persistent alists.  Deep chains of `conjoined` goals
  produce long alist walks.  For critical inner loops, cache `logic-walk`
  results in a local let binding.
- `run-logic n g` returns at most `n` solutions and short-circuits; use
  small `n` in interactive sessions.
- The N-Queens benchmark (`benchmarks/bench-backtrack-n-queens.tur`) and the
  pair-sum benchmark (`benchmarks/bench-logic-query.tur`) serve as
  representative workloads.

---

## Where to learn more

### The miniKanren language and theory

- **"The Reasoned Schemer" (2nd ed.)** -- Daniel P. Friedman, William E. Byrd,
  Oleg Kiselyov, Jason Hemann (MIT Press, 2018).  The canonical introduction;
  every concept in `tur/logic` maps directly to a chapter.

- **"miniKanren, Live and Untagged"** -- William Byrd et al., 2012 Workshop
  on Scheme and Functional Programming.
  <https://webyrd.net/scheme-2013/papers/HemannMuKanren2013.pdf>

- **muKanren** (micro-Kanren) -- Jason Hemann & Daniel Friedman, 2013.
  The minimal core (~40 lines of Scheme) that `tur/logic` is modeled after.
  <http://webyrd.net/scheme-2013/papers/HemannMuKanren2013.pdf>

- **miniKanren.org** -- canonical reference implementation, papers, and talks.
  <http://minikanren.org>

### Unification

- **"An Efficient Unification Algorithm"** -- Martelli & Montanari (1982),
  *ACM Transactions on Programming Languages and Systems*.
  Describes the linear-time algorithm; `tur/logic` uses the simpler quadratic
  alist walk, suitable for small substitutions.

- **"Unification: A Multidisciplinary Survey"** -- Kevin Knight (1989).
  <https://dl.acm.org/doi/10.1145/62029.62030>

### Logic programming broadly

- **"The Art of Prolog"** -- Sterling & Shapiro (MIT Press, 1994).
  Classical treatment of resolution, unification, and search strategies.

- **core.logic** (Clojure) -- a production miniKanren embedding closest in
  spirit to `tur/logic`.  Good source of idioms and constraint extensions.
  <https://github.com/clojure/core.logic>

- **Kanren** (original, Scheme) -- Byrd & Friedman.
  <https://github.com/webyrd/miniKanren>

### Related guides in this documentation

- [backtracking-guide.md](backtracking-guide.md) -- The `tur/backtrack` monad
  that `tur/logic` is built on.
- [minikanren-1-relations-and-queries.md](minikanren-1-relations-and-queries.md)
  -- A runnable worked example building a family-graph query.
- [datalog-01-concepts.md](datalog-01-concepts.md) -- Datalog, a cousin of
  miniKanren suited to database-style queries.
- [effects-system-guide.md](effects-system-guide.md) -- Algebraic effects,
  an alternative to the monad-based search strategy used here.
