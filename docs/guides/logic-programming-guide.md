---
title: Logic Programming Guide
category: Advanced Control Flow
description: Backtracking, logic programming, constraint solving with cloneable continuations
tags:
  - logic-programming
  - backtracking
  - continuations
---

# Logic Programming with Backtracking

Implementing miniKanren-style logic programming and constraint solving using cloneable continuations.

> **Status: design sketch, except where noted.** The narrative sections below
> (`choice-point`, `run` / `run*`, `do-backtrack`, `constraint`, and the bare
> `return` / `bind` spellings) describe a surface that **does not ship**. They
> are a design for a macro layer that has never been built; calling them will
> not elaborate.
>
> What *does* ship is the miniKanren engine in **`stdlib/logic.tur`** --
> `lequal`, `fresh`, `conjoined`, `disjoined`, `run-logic`, and the
> `mzero`/`mreturn`/`mplus`/`mbind` stream interface. The
> [API Summary](#api-summary) at the bottom of this page is written against
> that real module and every form in it is exercised by a fixture under
> `tests/fixtures/logic-*`. Start there, and see
> [tur-logic-guide.md](tur-logic-guide.md).
>
> The continuation primitives the sketch is built on -- `cloneable-reset` and
> `cloneable-shift` -- are real.

## Overview

Turmeric supports **multi-shot (cloneable) continuations**, enabling backtracking computation. This guide covers the design, use cases, and integration with Turmeric's ownership model.

## Motivation

Backtracking enables a declarative programming style where the system **automatically explores alternatives**:

| Use Case | Example | Benefit |
|----------|---------|---------|
| **Parsing** | Parser combinators with choice | Clean, composable grammar definitions |
| **Logic Programming** | miniKanren-style queries | Bidirectional computation |
| **Constraint Solving** | Sudoku, SAT solvers | Automatic search with pruning |
| **Probabilistic Programming** | Bayesian inference | Explore multiple execution paths |
| **Game AI** | Move exploration | Try strategies, backtrack on failure |

### One-shot vs. cloneable

Plain delimited continuations are **one-shot**: calling `continue k v` consumes `k`, matching Turmeric's ownership model but preventing backtracking. **Cloneable continuations** enable multi-shot by requiring all captured values to implement a `Clone` trait.

## Core Abstraction: Cloneable Continuations

A cloneable continuation can be resumed multiple times:

```turmeric
;; Surface syntax (sugar)
(cloneable-reset
  (fn []
    body))

(cloneable-shift [k]
  body)
```

```sweet-exp
;; Surface syntax (sugar)
cloneable-reset
  (fn []
    body)

cloneable-shift [k]
  body
```

### The Clone Trait

Every type captured by a cloneable continuation must implement `Clone`:

```turmeric
;; Clone primitives (copy semantics)
(instance Clone int64 (clone [x] x))
(instance Clone bool (clone [x] x))

;; Clone derived types (deep copy)
(instance Clone (Pair a b) [Clone a, Clone b]
  (clone [x] (Pair (clone x.first) (clone x.second))))

(instance Clone (Vector a) [Clone a]
  (clone [x] (map clone x)))
```

```sweet-exp
;; Clone primitives (copy semantics)
instance Clone int64
  clone [x] x
instance Clone bool
  clone [x] x

;; Clone derived types (deep copy)
instance Clone (Pair a b) [Clone a, Clone b]
  clone [x]
    Pair(clone(x.first) clone(x.second))

instance Clone (Vector a) [Clone a]
  clone [x]
    map(clone x)
```

**Key invariant:** Before a cloneable continuation is resumed, all values in its environment must be cloned. This means:

- Expensive for large environments, cheap for small ones.
- Limits mutation: captured `ref<T>` values should be immutable or used carefully.
- Encourages functional style: use immutable data structures.

## The Backtrack Monad

Multi-shot continuations are modeled as the **backtracking monad**:

A `Backtrack` computation over element type `A` is a **thunk that yields a
typed cons-list of results**: forcing the thunk runs the search one level, and
each `(Cons A)` element is one answer. The monad is parametric -- the same
`pure`/`mplus`/`bind` serve `int` searches and `float` searches alike.

The recurring shape `(fn [] (Cons A))` is spelled inline below because
`defalias` does not take type parameters yet -- a *monomorphic* alias like
`(defalias Backtrack (fn [] int))` works fine if you fix the element type, and
is transparent (the alias and `(fn [] int)` are the same type at every use
site, so `(fs)` applies exactly as it would without it). `deftype` is the
wrong tool for naming either shape -- it binds a recursive `TY_REC`, a
distinct nominal type that makes `(fs)` fail with `'fs' is not a function or
continuation`.

```turmeric
;; mzero -- the empty computation (no results)
(defn mzero [A] [] : (fn [] (Cons A))
  (fn [] (:: (tnil) (Cons A))))

;; pure -- a computation with exactly one result
(defn pure [A] [x : A] : (fn [] (Cons A))
  (fn [] (tcons x (tnil))))

;; mplus -- choice: all of fs's results, then all of gs's
(defn mplus [A] [^fat fs : (fn [] (Cons A)) ^fat gs : (fn [] (Cons A))] : (fn [] (Cons A))
  (fn [] (:: (list-concat (fs) (gs)) (Cons A))))

;; flat-map over a typed cons list; f returns a list per element
(defn list-flat-map [A] [xs : (Cons A) ^fat f : (fn [A] (Cons A))] : (Cons A)
  (if (tnil? xs)
    (:: (tnil) (Cons A))
    (:: (list-concat (f (thead xs))
                     (list-flat-map (:: (ttail xs) (Cons A)) f))
        (Cons A))))

;; bind -- sequence: run xs, then f on each result, concatenating
(defn bind [A] [^fat xs : (fn [] (Cons A)) ^fat f : (fn [A] (Cons A))] : (fn [] (Cons A))
  (fn [] (list-flat-map (xs) f)))
```

```sweet-exp
;; mzero -- the empty computation (no results)
defn mzero [A] [] : (fn [] (Cons A))
  (fn []
    (:: tnil() (Cons A)))

;; pure -- a computation with exactly one result
defn pure [A] [x : A] : (fn [] (Cons A))
  (fn []
    tcons(x tnil()))

;; mplus -- choice: all of fs's results, then all of gs's
defn mplus [A] [^fat fs : (fn [] (Cons A)) ^fat gs : (fn [] (Cons A))] : (fn [] (Cons A))
  (fn []
    (:: list-concat(fs() gs()) (Cons A)))

;; flat-map over a typed cons list; f returns a list per element
defn list-flat-map [A] [xs : (Cons A) ^fat f : (fn [A] (Cons A))] : (Cons A)
  if tnil?(xs)
    :: tnil() (Cons A)
    ::
      list-concat
        f(thead(xs))
        list-flat-map (:: ttail(xs) (Cons A)) f
      (Cons A)

;; bind -- sequence: run xs, then f on each result, concatenating
defn bind [A] [^fat xs : (fn [] (Cons A)) ^fat f : (fn [A] (Cons A))] : (fn [] (Cons A))
  (fn []
    list-flat-map(xs() f))
```

Driving it -- `mplus` offers both branches, `bind` maps over every result, and
the same monad instantiates at `int` and at `float` with no casts at the use
site:

```turmeric
;; Force every result and print it with p
(defn print-all [A] [xs : (Cons A) ^fat p : (fn [A] void)] : void
  (if (tnil? xs)
    nil
    (do (p (thead xs))
        (print-all (:: (ttail xs) (Cons A)) p))))

(print-all ((mplus (pure 1) (pure 2)))
           (fn [x : int] (println x)))
;; => 1
;; => 2

(print-all ((bind (mplus (pure 1) (pure 2))
                  (fn [x : int] (tcons (* x 10) (tnil)))))
           (fn [x : int] (println x)))
;; => 10
;; => 20

(print-all ((mplus (pure 7.1) (pure 2.5)))
           (fn [x : float] (println x)))
;; => 7.1
;; => 2.5
```
```sweet-exp
;; Force every result and print it with p
defn print-all [A] [xs : (Cons A) ^fat p : (fn [A] void)] : void
  if tnil?(xs)
    nil
    do
      p(thead(xs))
      print-all((:: (ttail xs) (Cons A)) p)

print-all((mplus(pure(1) pure(2)))
         (fn [x : int] println(x)))
;; => 1
;; => 2

print-all((bind(mplus(pure(1) pure(2))
                  (fn [x : int] tcons({x * 10} tnil()))))
         (fn [x : int] println(x)))
;; => 10
;; => 20

print-all((mplus(pure(7.1) pure(2.5)))
         (fn [x : float] println(x)))
;; => 7.1
;; => 2.5
```

> **Two representation facts shape the code above.**
>
> - The closures are `^fat` -- they capture, so they ride as fat
>   `{thunk, env}` values, and the parameter annotations say so.
> - List **tails** are int-carried: `ttail` returns `:int` and `list-concat`
>   is int-typed, which is why the `(:: ... (Cons A))` ascriptions appear on
>   tail recursions and around each `list-concat` result. Passing a
>   `(Cons A)` *into* an int-typed slot needs no cast (the carrier direction
>   is admitted); only the way back up is spelled out.

## Example: Parsing with Backtracking

A backtracking parser tries multiple production rules:

```turmeric
;; Token parser
(defn parse-token [t input]
  (if (= (car input) t)
    (return (cdr input))
    mzero))

;; Choice: try parseA, then parseB if parseA fails
(defn <|> [parseA parseB input]
  (mplus (parseA input) (parseB input)))

;; Sequence: parseA then parseB
(defn >> [parseA parseB input]
  (bind (fn [rest] (parseB rest))
        (parseA input)))

;; Grammar:
;; expr := term ('+' term)*
(defn parse-expr [input]
  (<|>
    (do-backtrack
      (def rest (parse-term input))
      (def rest (parse-many (parse-plus rest)))
      (return rest))
    (parse-term input)))
```

```sweet-exp
;; Token parser
defn parse-token [t input]
  if {car(input) = t}
    return(cdr(input))
    mzero

;; Choice: try parseA, then parseB if parseA fails
defn <|> [parseA parseB input]
  mplus(parseA(input) parseB(input))

;; Sequence: parseA then parseB
defn >> [parseA parseB input]
  bind
    (fn [rest] parseB(rest))
    parseA(input)

;; Grammar:
;; expr := term ('+' term)*
defn parse-expr [input]
  <|>
    do-backtrack
      def rest parse-term(input)
      def rest parse-many(parse-plus(rest))
      return(rest)
    parse-term(input)
```

## Example: Logic Programming with miniKanren

`stdlib/logic.tur` is a miniKanren: logic variables (`term-var`), unification
(`lequal`), `fresh`, conjunction (`conjoined`), interleaving disjunction
(`disjoined`), delayed recursion (`zzz`) and lazy solution streams
(`run-logic`, `st-pull`). `examples/minikanren/src/main.tur` is the worked
program; the pieces below are lifted from it.

A relation is a function from terms to a goal. A fact table is a disjunction
of unifications, and because either argument may be a variable the same
relation answers "parents of", "children of" and "every pair":

```turmeric no-check
(load "stdlib/logic.tur")

;;; fact -- the goal "p is PARENT and c is CHILD", one row of the table.
(defn fact [p : Term c : Term parent : int child : int] : (Goal int)
  (conjoined (lequal p (term-int parent)) (lequal c (term-int child))))

(defn parento [p : Term c : Term] : (Goal int)
  (disjoined (fact p c 0 1)                      ; abe   -> homer
    (disjoined (fact p c 5 1)                    ; mona  -> homer
      (disjoined (fact p c 1 2)                  ; homer -> bart
                 (fact p c 1 3)))))              ; homer -> lisa

;;; grandparento -- some m is g's child and c's parent.
(defn grandparento [g : Term c : Term] : (Goal int)
  (fresh (fn [m] (conjoined (parento g m) (parento m c)))))

;; Who are bart's grandparents?  The fresh variable is query variable 0.
(run-logic 10 (fresh (fn [g] (grandparento g (term-int 2)))))
```
```sweet-exp
load "stdlib/logic.tur"

;;; fact -- the goal "p is PARENT and c is CHILD", one row of the table.
defn fact [p : Term c : Term parent : int child : int] : (Goal int)
  conjoined(lequal(p term-int(parent)) lequal(c term-int(child)))

defn parento [p : Term c : Term] : (Goal int)
  disjoined(fact(p c 0 1)                      ; abe   -> homer
    disjoined(fact(p c 5 1)                    ; mona  -> homer
      disjoined(fact(p c 1 2)                  ; homer -> bart
                 fact(p c 1 3))))              ; homer -> lisa

;;; grandparento -- some m is g's child and c's parent.
defn grandparento [g : Term c : Term] : (Goal int)
  fresh((fn [m] conjoined(parento(g m) parento(m c))))

;; Who are bart's grandparents?  The fresh variable is query variable 0.
run-logic(10 fresh((fn [g] grandparento(g term-int(2)))))
```

`run-logic n goal` returns a lazy `Stream` of at most `n` substitutions; each
is one answer, read back by walking the query variable:

```turmeric no-check
(defn print-people [results : Stream v : int] : int
  (match (st-pull results)
    (StCons s rest)
      (do
        (println (person-name (term-int-val (logic-walk (term-var v) s))))
        (print-people rest v))
    _ 0))
```
```sweet-exp
defn print-people [results : Stream v : int] : int
  match st-pull(results)
    (StCons s rest)
    do
      println(person-name(term-int-val(logic-walk(term-var(v) s))))
      print-people(rest v)
    _
    0
```

The classic `appendo` shows the part a function cannot do. Lists are
`term-pair` / `term-nil` terms; the recursive branch is wrapped in `zzz` so
the goal can be *built* without diverging, and the search unfolds it one
step per pull:

```turmeric no-check
(defn appendo [l : Term s : Term out : Term] : (Goal int)
  (disjoined
    (conjoined (lequal l (term-nil)) (lequal s out))
    (fresh (fn [a]
      (fresh (fn [d]
        (fresh (fn [res]
          (conjoined (lequal l (term-pair a d))
            (conjoined (lequal out (term-pair a res))
                       (zzz (appendo d s res))))))))))))

;; forwards:  (1 2) ++ (3 4) = ?          -> (1 2 3 4)
(run-logic 5 (fresh (fn [out] (appendo (list2 1 2) (list2 3 4) out))))
;; backwards: ? ++ (3 4) = (1 2 3 4)      -> (1 2)
(run-logic 5 (fresh (fn [l] (appendo l (list2 3 4) (list4 1 2 3 4)))))
;; both unknown: every split of (1 2 3)   -> () ++ (1 2 3), (1) ++ (2 3), ...
(run-logic 10 (fresh (fn [l] (fresh (fn [s] (appendo l s (list3 1 2 3)))))))
```
```sweet-exp
defn appendo [l : Term s : Term out : Term] : (Goal int)
  disjoined
    conjoined(lequal(l term-nil()) lequal(s out))
    fresh((fn [a]
      fresh((fn [d]
        fresh((fn [res]
          conjoined(lequal(l term-pair(a d))
            conjoined(lequal(out term-pair(a res))
                       zzz(appendo(d s res))))))))))

;; forwards:  (1 2) ++ (3 4) = ?          -> (1 2 3 4)
run-logic(5 fresh((fn [out] appendo(list2(1 2) list2(3 4) out))))
;; backwards: ? ++ (3 4) = (1 2 3 4)      -> (1 2)
run-logic(5 fresh((fn [l] appendo(l list2(3 4) list4(1 2 3 4)))))
;; both unknown: every split of (1 2 3)   -> () ++ (1 2 3), (1) ++ (2 3), ...
run-logic(10 fresh((fn [l] fresh((fn [s] appendo(l s list3(1 2 3)))))))
```

`disjoined` interleaves, so a relation with infinitely many solutions still
yields the ones you ask for (`tests/fixtures/logic-lazy-infinite`);
`disjoined-dfs` keeps depth-first order and is only complete when the left
branch is finite. Goals are also a `Monad` / `Alternative`, so `do-m` and
`alt-or` spell conjunction and disjunction (see "Typeclass instances").

## Example: Constraint Solving (Sudoku)

```turmeric
(defn sudoku [grid]
  ;; grid is a 9x9 array with some cells filled (1-9), others unbound (lvar)
  
  ;; For each cell, either it's already bound or we bind it to 1-9
  (defn choose-domain [cell]
    (if (lvar? cell)
      (choice-point [1 2 3 4 5 6 7 8 9])
      (return cell)))
  
  (defn all-different [xs]
    ;; Constraint: all values in xs must be distinct
    (bind (fn [vs]
            (if (distinct? vs)
              (return vs)
              mzero))
          (map-backtrack choose-domain xs)))
  
  ;; Run constraints: rows, columns, 3x3 boxes all distinct
  (do-backtrack
    (def filled (map choose-domain grid))
    (map all-different (rows filled))
    (map all-different (cols filled))
    (map all-different (boxes filled))
    (return filled)))

;; Solve and get first 10 solutions
(run 10 [solution]
  (sudoku initial-grid))
```

```sweet-exp
defn sudoku [grid]
  ;; grid is a 9x9 array with some cells filled (1-9), others unbound (lvar)

  ;; For each cell, either it's already bound or we bind it to 1-9
  defn choose-domain [cell]
    if lvar?(cell)
      choice-point([1 2 3 4 5 6 7 8 9])
      return(cell)

  defn all-different [xs]
    ;; Constraint: all values in xs must be distinct
    bind
      (fn [vs]
        (if distinct?(vs)
          return(vs)
          mzero))
      map-backtrack(choose-domain xs)

  ;; Run constraints: rows, columns, 3x3 boxes all distinct
  do-backtrack
    def filled map(choose-domain grid)
    map(all-different rows(filled))
    map(all-different cols(filled))
    map(all-different boxes(filled))
    return(filled)

;; Solve and get first 10 solutions
run 10 [solution]
  sudoku(initial-grid)
```

## Integration with Turmeric's Ownership Model

### Challenge: Capturing Mutable State

Turmeric's `ref<T>` assumes linear consumption (move semantics). Cloneable continuations re-execute the captured code, trying to consume the same `ref<T>` multiple times:

```turmeric
;; ERROR: re-executing code will consume the ref twice
(cloneable-reset
  (fn []
    (let [r (ref 42)]
      (choice-point [1 2]))))  ; captures r; next backtrack tries to use r again
```

```sweet-exp
;; ERROR: re-executing code will consume the ref twice
cloneable-reset
  (fn []
    (let [r ref(42)]
      choice-point([1 2])))  ; captures r; next backtrack tries to use r again
```

**Solution:** Capture immutable data or use `rc<T>` for shared ownership:

```turmeric
;; OK: captured value is immutable
(cloneable-reset
  (fn []
    (let [x 42]  ; immutable
      (choice-point [1 2]))))

;; OK: shared ownership doesn't consume on re-entry
(cloneable-reset
  (fn []
    (let [r (rc 42)]
      (choice-point [1 2]))))
```

```sweet-exp
;; OK: captured value is immutable
cloneable-reset
  (fn []
    (let [x 42]  ; immutable
      choice-point([1 2])))

;; OK: shared ownership doesn't consume on re-entry
cloneable-reset
  (fn []
    (let [r rc(42)]
      choice-point([1 2])))
```

### Defer and Cloneable Continuations

When a cloneable continuation captures state across a `defer` boundary, each clone must re-run the deferred cleanup on entry:

```turmeric
;; When backtracking re-enters this block,
;; the file is re-opened in the cloned continuation
(cloneable-reset
  (fn []
    (defer-with-close (open-file "data.txt")
      (fn [f]
        (let [data (read f)]
          (choice-point (parse data)))))))
```

```sweet-exp
;; When backtracking re-enters this block,
;; the file is re-opened in the cloned continuation
cloneable-reset
  (fn []
    (defer-with-close open-file("data.txt")
      (fn [f]
        (let [data read(f)]
          choice-point(parse(data))))))
```

This is safe but can be expensive. Prefer immutable snapshots where possible.

## API Summary

This section is the real, shipping surface: everything below is from
`stdlib/logic.tur` and is covered by a fixture under `tests/fixtures/logic-*`.
Load it with `(load "stdlib/logic.tur")`.

### Terms

```turmeric
(term-int 42)          ; an integer term
(term-var 0)           ; a logic variable, addressed by id
(term-pair a b)        ; a cons pair of two terms
(term-nil)             ; the empty term

(term-int-val t)       ; int payload of a TInt   (0 otherwise)
(term-var-id t)        ; id of a TVar            (0 otherwise)
(term-pair-fst t)      ; first  of a TPair       (TNil otherwise)
(term-pair-snd t)      ; second of a TPair       (TNil otherwise)
```

```sweet-exp
term-int(42)           ; an integer term
term-var(0)            ; a logic variable, addressed by id
term-pair(a b)         ; a cons pair of two terms
term-nil()             ; the empty term

term-int-val(t)        ; int payload of a TInt   (0 otherwise)
term-var-id(t)         ; id of a TVar            (0 otherwise)
term-pair-fst(t)       ; first  of a TPair       (TNil otherwise)
term-pair-snd(t)       ; second of a TPair       (TNil otherwise)
```

### Goals

A goal is a `(Goal a)` -- a function from a substitution to a stream of
substitutions. `lequal` is miniKanren's `==`.

```turmeric
(lequal t1 t2)         ; unify two terms
(succeed)              ; always succeeds, one solution
(fail)                 ; always fails, zero solutions
(conjoined g1 g2)      ; both goals must hold
(disjoined g1 g2)      ; either goal may hold
(fresh (fn [x] goal))  ; introduce a new logic variable
```

```sweet-exp
lequal(t1 t2)          ; unify two terms
succeed()              ; always succeeds, one solution
fail()                 ; always fails, zero solutions
conjoined(g1 g2)       ; both goals must hold
disjoined(g1 g2)       ; either goal may hold
fresh((fn [x] goal))    ; introduce a new logic variable
```

### Running a query

```turmeric
(run-logic n goal)     ; run goal, collecting at most n solutions -> Stream
(bt-length results)    ; how many solutions came back
(stream-empty? xs)     ; true when the stream has no solutions
(first-state results)  ; the first solution's Subst
(logic-walk t subs)    ; reify a term under a substitution
```

```sweet-exp
run-logic(n goal)      ; run goal, collecting at most n solutions -> Stream
bt-length(results)     ; how many solutions came back
stream-empty?(xs)      ; true when the stream has no solutions
first-state(results)   ; the first solution's Subst
logic-walk(t subs)     ; reify a term under a substitution
```

A complete query, from `tests/fixtures/logic-query`:

```turmeric
(load "stdlib/logic.tur")

(defn main [] : int
  (let [results (run-logic 5 (disjoined (lequal (term-var 0) (term-int 1))
                                        (disjoined (lequal (term-var 0) (term-int 2))
                                                   (lequal (term-var 0) (term-int 3)))))]
    (println (bt-length results))   ; => 3
    0))
```

```sweet-exp
load("stdlib/logic.tur")

defn main [] : int
  let [results run-logic(5 disjoined(lequal(term-var(0) term-int(1))
                                     disjoined(lequal(term-var(0) term-int(2))
                                               lequal(term-var(0) term-int(3)))))]
    println $ bt-length results     ; => 3
    0
```

And reifying the answers, from `tests/fixtures/logic-reify`:

```turmeric
(defn inner-goal [x : Term y : Term] : (Goal int)
  (conjoined (lequal x (term-int 10)) (lequal y (term-int 20))))

(defn main [] : int
  (let [goal    (fresh (fn [x] (fresh (fn [y] (inner-goal x y)))))
        results (run-logic 1 goal)
        subs    (first-state results)]
    (println (term-int-val (logic-walk (term-var 0) subs)))   ; => 10
    (println (term-int-val (logic-walk (term-var 1) subs)))   ; => 20
    0))
```

```sweet-exp
defn inner-goal [x : Term y : Term] : (Goal int)
  conjoined(lequal(x term-int(10)) lequal(y term-int(20)))

defn main [] : int
  let [goal    fresh((fn [x] fresh((fn [y] inner-goal(x y)))))
       results run-logic(1 goal)
       subs    first-state(results)]
    println $ term-int-val $ logic-walk term-var(0) subs      ; => 10
    println $ term-int-val $ logic-walk term-var(1) subs      ; => 20
    0
```

### Stream (monadic) interface

The names are `mreturn` and `mbind`, not bare `return` and `bind`.

```turmeric
(mzero)                ; no solutions
(mreturn subs)         ; exactly one solution
(mplus xs ys)          ; append two solution streams
(mbind ma f)           ; flat-map a stream of substitutions
```

```sweet-exp
mzero()                ; no solutions
mreturn(subs)          ; exactly one solution
mplus(xs ys)           ; append two solution streams
mbind(ma f)            ; flat-map a stream of substitutions
```

### Substitutions

```turmeric
(subs-empty)                 ; the empty substitution
(logic-unify t1 t2 subs)     ; unify, yielding a UnifyResult
(subst-lookup vid subs)      ; look up a variable binding
```

```sweet-exp
subs-empty()                 ; the empty substitution
logic-unify(t1 t2 subs)      ; unify, yielding a UnifyResult
subst-lookup(vid subs)       ; look up a variable binding
```

### Typeclass instances

Instances are declared with `definstance`, not `instance`:

```turmeric
(definstance Clone [SearchState]
  (clone [self] ...))
```

```sweet-exp
definstance Clone [SearchState]
  clone [self]
    ...
```

## Performance Considerations

### Clone Overhead

Cloning captured environments is expensive for large states. Strategies:

1. **Minimize capture:** Use local variables; avoid capturing large structures.
2. **Functional style:** Immutable data structures (tree, trie) amortize clone cost.
3. **Lazy evaluation:** Defer constraint checking until necessary.
4. **Cut:** Add `!` operator to commit to first solution, preventing unnecessary backtracking.

### Stack vs. Heap

Unlike Prolog's stack-based choice points, Turmeric's cloneable continuations allocate on the heap. This is acceptable for bounded search spaces but may not scale to million-node search trees.

## See Also

- [Backtrackable State Guide](backtrackable-state-guide.md) -- Trailed mutable cells with mark/undo. An indexed trailed substitution beats the persistent `Subst` chain described here by 11-34x on lookup, with no crossover; the catch is that answers must be reified before the search backtracks past them
- [Effects System Guide](effects-system-guide.md) -- Algebraic effects foundation
- [Async/Await Guide](async-await-guide.md) -- Single-shot continuations for I/O
- [Checkpointing with Serializable Continuations](checkpointing-guide.md) -- Persisting computations
