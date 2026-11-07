# A rank-2 call whose type variable appears only in the result is typed from its first argument

**Severity: low (compile-time rejection, no wrong answer).** Found on
2026-11-06 while widening `await-through-rank2-struct-position`
(await-parks-only-to-the-nearest-c-frame, tenth pass); the shape was dropped
from that fixture rather than fixed here.

A call through a rank-2 parameter `f (forall [a] (-> int a))` instantiates
`a` from the first ARGUMENT's type, so `(f m)` with `m : int` is typed `int`
whatever the enclosing function or a `let` annotation says. At `int` that is
right by accident; at `cstr` or a struct the enclosing function's declared
return is then checked against `int` and refused (`TUR-E0708` / `TUR-E0709`),
although `(:: mk-p (forall [a] (-> int a)))` is the trusted opt-out that
asserts the value has that type.

## Repro

```turmeric
(defstruct P [a : int b : int])
(defn mk-p [m : int] : P (P m 1))
(defn r2ip-apply [f (forall [a] (-> int a)) m : int] : P (f m))
(defn main [] : nil
  (println (.a (r2ip-apply (:: mk-p (forall [a] (-> int a))) 13))))
```

```
error [TUR-E0709]: function 'r2ip-apply' declares return type 'P' but its
body returns int -- an aggregate is a real C type ...
```

`tur --interpret` prints 13. With `: cstr` in place of `: P` (and a
`cstr`-returning `name-of`) both engines refuse with `TUR-E0708`. An
annotated `let [p : P (f m)]` around the call does not ground it either. The
same parameter with `a` in an argument position (`(-> a a)`, `(-> a int)`)
works at every kind -- that is what the await fixtures exercise.

## Root cause

`src/compiler/elab_call.c`, the rank-2 call elaboration ("Determine return
type by instantiation", around line 14977): a bare `TY_TYVAR` result is
pinned to the argument whose parameter names the same variable, and when none
does it falls back to argument 0 ("the classic `a -> a` shape"). The `TY_APP`
result arm beside it already instantiates a variable no argument mentions
from the expected type (`poly_expected`, the turi-dict-passing-plan probe);
the bare-tyvar arm does not.

## Fix directions

1. In the bare-tyvar arm, when no argument's parameter names the result
   variable, instantiate it from `poly_expected` as the `TY_APP` arm does,
   and only fall back to argument 0 when there is no expected type. The
   enclosing function's declared return and a `let` annotation both reach
   the call as `poly_expected`.
2. Failing an expected type, refuse the call with a diagnostic naming the
   unconstrained variable rather than typing it `int`: a result-only `forall`
   variable with nothing to instantiate it is a genuine ambiguity.
