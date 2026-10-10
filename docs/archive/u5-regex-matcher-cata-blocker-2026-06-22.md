---
title: Track C U5 (regex matcher as one cata) -- re-verified on turmeric v0.46.0
category: Spice-uplift feasibility analysis (Track C / U5)
status: UNBLOCKED -- the function-typed-carrier segfault is gone. A narrower emitter defect survives (arm-order-dependent carrier typing) and has a one-line source workaround.
verified-on: turmeric v0.46.0 (`tur --version` -> 0.46.0)
verified-by: turmeric-spices docs accuracy sweep, 2026-09-09
plan: docs/archive/spices-type-features-uplift-plan.md (Phase U5)
---

# U5 regex matcher: can it become one `cata`?

> **Archived 2026-10-07; the surviving defect is now an open report.**
> Re-run on turmeric **v0.63.5** (macOS, Release build): the "Minimal repro"
> below still fails at the C compile step with the same three
> `-Wint-conversion` errors, and writing the capturing `AddF` arm first still
> compiles and runs. The defect was tracked in
> [match-result-temp-typed-from-first-arm](match-result-temp-typed-from-first-arm.md),
> resolved 2026-10-09: either arm order compiles now.
> Everything else in this document is unchanged from the 2026-09-09 sweep.

> **RESOLVED 2026-09-09 -- yes, it can.**
>
> This document previously reported that a `cata` whose carrier is a function
> taking a function argument (`B = (fn [(fn [cstr] bool) cstr] bool)`)
> **segfaulted at runtime**, and that the regex matcher therefore had to stay
> direct structural recursion. That is no longer true on v0.46.0: the doc's own
> minimal repro compiles and prints `7`, and the regex matcher's exact carrier
> shape compiles and matches correctly.
>
> What survives is a much narrower **emitter** defect about the C type of one
> temporary, described below. It is order-dependent and has a trivial
> source-level workaround, so it is a papercut, not a blocker.

## TL;DR

- **The old blocker is gone.** A function-typed carrier whose own argument is a
  function type-checks, compiles, and runs. No thin/fat closure mismatch at the
  application site, no segfault.
- **A narrower defect survives.** In an algebra whose return type is a function
  type, the emitter types the `match` result temporary from the **first arm as
  written**. If that first arm returns a closure that **captures nothing**, the
  temporary is emitted as `int64_t` while every arm actually builds a `void *`
  fat-closure box, and `cc` rejects the file.
- **Workaround: write a capturing arm first.** With that one reordering the
  whole matcher compiles and runs. Nothing else changes.

## Verification (turmeric v0.46.0)

### The old repro now passes

The minimal repro this document filed on 2026-06-22 -- carrier
`B = (fn [(fn [int] int) int] int)`, whose first argument is the CPS
continuation -- was re-run verbatim:

```
tur check repro.tur   -> exit 0
tur run   repro.tur   -> 7
```

`7` is the expected answer. There is no int-from-pointer warning and no
segfault. (The run emits one unrelated `-Wtypedef-redefinition` C11 warning
about `tur_adt_Expr`, which is cosmetic and unconnected to carriers.)

### The regex matcher's real carrier shape

The matcher carrier `B = (fn [(fn [cstr] bool) cstr] bool)` was rebuilt
standalone over a `ReF` functor with the spice's own five constructors
(`EmptyF`, `LitF`, `AltF`, `CatF`, `StarF`) and the spice's `lit-step` inline-C.
Folded with `re-cata`, `(cat (lit "a") (lit "b"))` against `"ab"` prints
`match`. The fold is correct end to end.

That run required the arm reordering below. With the arms in their natural
order -- `EmptyF` first -- the program fails at the C compile step.

## The surviving defect: arm-order-dependent carrier typing

### Symptom

`tur check` passes. `cc` fails:

```
error: incompatible pointer to integer conversion assigning to 'int64_t'
       (aka 'long long') from 'void *' [-Wint-conversion]
    __t270 = __t273;
error: incompatible integer to pointer conversion returning 'int64_t'
       (aka 'long long') from a function with result type 'void *'
    return __t270;
```

One assignment error per algebra arm, plus one on the return.

### Cause

The algebra lowers to a C function returning `void *` (the fat-closure
pointer). Its `match` lowers to a result temporary that every arm assigns into.
The emitter picks that temporary's C type from the first arm. A closure that
captures nothing is classified as thin, so the temporary is declared
`int64_t` -- but the arm still builds a fat box:

```c
static void * alg(const tur_adt_ExprF__fn2_fn0__struct_int__int * l) {
        int64_t __t270 = 0;              /* <-- typed from the first arm */
        ...
            case 0: {
                void *__t272 = malloc(sizeof(void *) + 2 * sizeof(int64_t));
                __t271[0] = (int64_t)(intptr_t)__tur_fatshim2;
                __t271[1] = (int64_t)(intptr_t)__fn_1502;
                void *__t273 = __t271;
                __t270 = __t273;         /* <-- void* into int64_t */
```

When the first arm's closure *does* capture, the same temporary is emitted as
`void * __t270 = 0;` and every arm assigns cleanly.

### Observed matrix

Carrier `B = (fn [(fn [int] int) int] int)`, varying only the `LitF` arm body
(`n` is the matched constructor's field):

| First arm's body | captures `n` | `tur check` | `cc` | runs |
|---|---|---|---|---|
| `(k (+ s n))` | yes | pass | pass | yes |
| `(if (> n 0) (k s) 0)` | yes | pass | pass | yes |
| `(k s)` | no | pass | **FAIL** | -- |
| `(k (+ s 0))` | no | pass | **FAIL** | -- |
| `(let [b (k s)] b)` | no | pass | **FAIL** | -- |

Two controls pin the rule to arm **order**, not to capture as such:

- Writing the capturing `AddF` arm **first** and the non-capturing `(k s)`
  `LitF` arm second: compiles and runs.
- The regex `ReF` algebra with the capturing `LitF` arm written first and the
  non-capturing `EmptyF` / `StarF` arms after it: compiles and runs.
- All arms non-capturing: fails, as expected.

So a non-capturing closure anywhere but the first arm is harmless. Only the
first arm sets the temporary's type.

This is why the regex matcher hits it by default: `EmptyF` is a **nullary**
constructor, so its arm `(k s)` has nothing it *could* capture, and it is
naturally written first.

## Minimal repro (no inline C)

```turmeric
(load "stdlib/typeclass-functor.tur")

(defdata ExprF :copy [a] (LitF :int) (AddF a a))
(defdata Expr  :copy (Roll (ExprF Expr)))

(definstance Functor [ExprF]
  (fmap [c g]
    (match c
      (LitF n)   (LitF n)
      (AddF x y) (AddF (g x) (g y)))))

(defn unroll-e [e : Expr] : (ExprF Expr) (match e (Roll l) l))

(defn cata [B] [alg : (fn [(ExprF B)] B) e : Expr] : B
  (alg (:: (fmap (unroll-e e) (fn [c : Expr] : B (cata alg c))) (ExprF B))))

(defn lit [n : int] : Expr (Roll (LitF n)))
(defn add [x : Expr y : Expr] : Expr (Roll (AddF x y)))

;; FAILS at `cc`: the first arm's closure captures nothing.
;; Swap the two arms and it compiles and runs.
(defn alg [l : (ExprF (fn [(fn [int] int) int] int))]
         : (fn [(fn [int] int) int] int)
  (match l
    (LitF n)   (fn [k : (fn [int] int) s : int] : int (k s))
    (AddF x y) (fn [k : (fn [int] int) s : int] : int
                 (x (fn [s2 : int] : int (y k s2)) s))))

(defn main [] : int
  (println ((cata alg (add (lit 3) (lit 4))) (fn [r : int] : int r) 0)))
```

## Bottom line for Track C / U5

- regex value folds: already one `re-cata` each (shipped earlier).
- regex matcher: **no longer blocked.** It stays direct structural recursion in
  `spices/regex/src/regex/tree.tur` today as a code-shape choice, not because
  the compiler prevents the cata. Converting it needs only that a capturing arm
  be written before the nullary `EmptyF` arm.
- The other U5 AST targets (`c-dsl`, `glsl`, `scscm`, `template`) do not need a
  function-typed carrier and were never gated by this.
