# `match` result temporary is typed from the first arm, so a non-capturing closure there breaks the C

**RESOLVED 2026-10-09.** Fix direction 2: arm types that are one fn type join
to the fat representation when either arm is fat. See "Resolution" at the end.

**Severity:** low-medium (compile failure with a one-line source workaround).
`tur check` passes; `cc` rejects the emitted C.

**Found:** 2026-09-09 (turmeric v0.46.0) in the regex matcher cata work;
re-verified 2026-10-07 on v0.63.5 (Release build, AppleClang). Carried over from
[u5-regex-matcher-cata-blocker-2026-06-22](../archive/u5-regex-matcher-cata-blocker-2026-06-22.md),
which has the full observed matrix.

## Symptom

An algebra whose return type is a function type, written with a `match` whose
**first** arm returns a closure that captures nothing, emits the result
temporary as `int64_t` while every arm builds a `void *` fat-closure box:

```
error: incompatible pointer to integer conversion assigning to 'int64_t'
       from 'void *' [-Wint-conversion]
    __t196 = __t199;
error: incompatible integer to pointer conversion returning 'int64_t'
       from a function with result type 'void *' [-Wint-conversion]
    return __t196;
```

One assignment error per arm plus one on the return. macOS AppleClang 21 treats
`-Wint-conversion` as a hard error, so the program does not build there; Linux
gcc only warns, which is why this can look macOS-only.

## Repro

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
(defn alg [l : (ExprF (fn [(fn [int] int) int] int))]
         : (fn [(fn [int] int) int] int)
  (match l
    (LitF n)   (fn [k : (fn [int] int) s : int] : int (k s))
    (AddF x y) (fn [k : (fn [int] int) s : int] : int
                 (x (fn [s2 : int] : int (y k s2)) s))))

(defn main [] : int
  (println ((cata alg (add (lit 3) (lit 4))) (fn [r : int] : int r) 0))
  0)
```

`./build-release/tur run repro.tur` fails with the three errors above.
Writing the capturing `AddF` arm **first** and `LitF` second compiles and runs.

## Root cause

Not located in source yet. From the emitted C: the `match` result temporary's
C type is taken from the first arm. A closure that captures nothing is
classified as thin, so the temporary is declared `int64_t`, but the arm still
builds a fat box (`malloc(sizeof(void *) + ...)` with `__tur_fatshim`) because
the *expected* type is a function value whose other arms capture. When the first
arm captures, the temporary is `void *` and every arm assigns cleanly.

The classification disagrees with the declared result type of the enclosing
function, which is what should decide the temporary's type.

## Fix directions

1. Type the `match` result temporary from the expected / declared result type
   when there is one, falling back to the first arm only for an unannotated
   `match`. This is the principled fix.
2. Or unify the arm result types before choosing the C type: a thin and a fat
   closure arm in the same `match` join to the fat representation.
3. Either way, add a fixture under `tests/fixtures/` with the non-capturing
   arm first. The compile of the emitted C is the detector: it fails on
   AppleClang today, so a plain `run.sh` fixture is enough.

## Impact

`spices/regex/src/regex/tree.tur` keeps `re-matches?` as direct structural
recursion rather than one `re-cata`. It is a code-shape choice today, not a
blocker: converting needs only a capturing arm before the nullary `EmptyF` arm.
This is the natural arm order for any nullary-first functor, so it will bite the
next function-carrier cata too.

## Resolution (2026-10-09)

The root cause was the arm-type join, not the emitter. `match_arm_type_compatible`
(`src/compiler/elab_structs.c`), which `match` and `if` both join their arms
through, returned the FIRST arm's type whenever the two were `type_eq` --
and `type_eq` ignores a fn type's `boxed` flag, which is exactly what tells a
capture-free lambda's thin code pointer from a capturing lambda's fat box.
The emitter then declared the result temporary from that type (`int64_t`)
while each arm, coerced to the function's declared result, stored a `void *`.
Two arms of one fn type now join to the boxed one when either is boxed, so
the temporary is `void *` whatever the arm order.

The fixture shows the gate also catches it under gcc: `run.sh`'s emitted-C
representation check fails the pre-fix build with "emitted C confuses scalar
representations", so this was never macOS-only in CI terms -- only no fixture
had the shape.

Pinned by `tests/fixtures/match-fn-result-thin-arm-first`: the report's cata
repro (capture-free arm first), a three-arm `match` with the capturing arm in
the middle over float payloads, and the same join through `if`.
