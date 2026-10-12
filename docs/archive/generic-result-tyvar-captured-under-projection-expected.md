# A generic call's unbound result variable reads as the caller's when the expected type is a projection

**RESOLVED 2026-10-12** (filed and fixed the same day) -- see "Fixed" at the end.

**Severity: low-medium (a C type mismatch, or a type error, depending on a
variable's spelling).** Found 2026-10-12 while fixing
[associated-type-unusable-nullary-and-generic](associated-type-unusable-nullary-and-generic.md).

## Repro

```turmeric
(defclass Box [a]
  (type Inner : Type)
  (unwrap [x : a] : Inner))
(defopaque W :int)
(defopaque V :int)
(definstance Box [W] (type Inner = int) (unwrap [x] (:: x int)))
(definstance Box [V] (type Inner = cstr) (unwrap [x] "v"))

(defn both [A] [(Box A)] [x : A] : (Vec (Inner A))
  (let [v : (Vec (Inner A)) (vec-new)]
    (vec-push! v (unwrap x))
    v))

(defn main [] : int
  (println (vec-get (both (:: 5 W)) 0))
  (println (vec-get (both (:: 5 V)) 0))
  0)
```

It prints the right values, but the emitted C mixes pointer types, which
`tests/run.sh` fails on and gcc 14 / clang 15+ reject outright:

```
warning: initialization of 'tur_adt_Vec__W *' from incompatible pointer type 'tur_adt_Vec__int *'
```

Spell the generic's variable `B` (`[B] [(Box B)] ... (Vec (Inner B))`) and the
warning goes away. Use a local producer whose variable is NOT the caller's,
`(defn mk [T] [] : (Vec T) (vec-new))`, and `(let [v : (Vec (Inner A)) (mk)]
...)` is a type error instead: `function 'vec-push!' arg 2: expected T, got
(Inner A)`.

## Root cause

`vec-new` is `[A] : (Vec A)`, with no argument to bind `A` from, so its result
can only be instantiated from the expected type. The binding from an expected
result (`src/compiler/elab_call.c`, after `e->expected_type =
saved_expected_return`) takes a GROUND expected type, or a bare type variable
for a bare-variable result, or a `^construct` callee; `(Vec (Inner A))` is
none of those (the projection is a named type variable), so nothing binds
`vec-new`'s `A`, and its result stays `(Vec A)` -- the callee's variable,
under the caller's name. The `let` binder then carries that type: in `both`
it IS the caller's `A` (so `W` / `V` in each spec), and with `mk` it is a `T`
nothing knows.

The same family as the archived generic-call-result-leaks-callee-tyvar-names
(a partially bound result), for a result nothing bound at all.

## Fix directions

1. Bind a still-open result variable from a NON-ground expected type when the
   expected type's variables are the enclosing body's own (the composition the
   `^construct` branch already does), at least when the callee's result is an
   application over the open variable. The ground-only guard there exists so a
   relay does not mark an inner call "specialized" with no concrete
   substitution; a projection is resolved per spec, so it should be safe to
   compose through, but measure that first.
2. At minimum, prefer the `let` annotation over the init's type when the
   init's type still names a variable that belongs to the callee.

Worth a fixture with the caller's variable spelled the same as the callee's,
since that is the spelling that hides the defect behind a working program.

## Fixed (2026-10-12)

Fix direction 1, gated narrowly. After the existing result-from-expected
branches (`src/compiler/elab_call.c`, after `e->expected_type =
saved_expected_return`), a global callee whose result is an application over
a variable no argument bound, under an expected application that mentions an
associated-type projection, binds its variables from the expected type --
`vec-new`'s `A := (Inner A)`. The projection is a variable every spec of the
enclosing generic binds by name (`call_bind_assoc_projections`), so emit
composes the binding through the active spec as it does for the `^construct`
branch's caller variables, and the result is `(Vec (Inner A))`, reduced per
spec. Gated to a projection so the relay returns the ground-only guard
protects (binding `K -> K` would mark a call specialized with nothing
concrete) are untouched.

Pinned by `tests/fixtures/assoc-projection-expected-binds-callee-result`
(the caller's variable spelled like `vec-new`'s, and a local `mk [T]`
producer, at an `int` and a `cstr` projection; both engines). It is a type
error on `main` and the pointer-type warning in the first spelling.
`typeclass-assoc-nullary-method-generic`'s `both` is back to the natural `A`.
