# A literal stood for a generic's associated type, or the caller's own variable, unchecked

**Resolved 2026-10-29.** Severity was medium (silent wrong values; one shape
diverged between engines).

Found while checking what generic code over a numeric-array class can do
(simd-and-vectorization-plan section 3.1).

## What was accepted

Inside `(defn f [V] [(C V)] ...)` with `(type E : Type)` in `C`:

```turmeric
(defn f [V] [(C V)] [a : V] : (E V) 1.5)            ; return
(defn g [V] [(C V)] [a : V] : nil (c-put a 1.5))    ; method argument at E
```

At an `E = int32` instance the emitted C was `return 1.5;` / `c_put(a, 1.5)`
in an `int32` slot: 1.5 became 1. `tests/check-emitted-float-conversions.py`
flagged both as implicit float->int conversions.

The same hole for an ordinary type variable, through name capture:

```turmeric
(defn second [A] [x : A y : A] : A y)
(defn plain [A] [x : A] : A (second x 1.5))   ; called (plain 7)
```

printed `4609434218613702656` compiled (1.5's bits read as the int `A`) and
`1.5` under turi. Spelled `second [B]` it was already refused.

## Causes and fixes

- **Return.** `rt_is_rigid` (`src/compiler/elab_core.c`) exempted every
  associated-type projection from the declared-tyvars-are-rigid check. A
  projection over the defn's own variables is now as rigid as they are:
  TUR-E0709.
- **Argument, plain variable.** `call_collect_type_bindings`
  (`src/compiler/elab_call.c`) has a rule (m5) that lets a binding `A := A`
  absorb a later concrete argument without overwriting it, meant for a lambda
  after typed arguments. When the callee's and caller's variables were
  spelled alike it absorbed a ground scalar too. Not any more, at an
  argument's own level (an `int` inside a function-typed argument still rides
  the carrier, as `map-eq-driver` needs): TUR-E0001.
- **Argument, class method.** The method-call path checked nothing at an
  abstract receiver: `method_arg_scalar_at_abstract_ok`
  (`src/compiler/elab_typeclasses.c`) refuses a ground scalar at a parameter
  the class types with its variable or an associated type, outside `unsafe`.

Turning the return check on exposed two places that typed a legitimate
projection as `int`, both fixed with it:

- a generic relaying to a generic whose variable is spelled the same had its
  `(Inner A)` result renamed apart to an open slot (`call_bind_assoc_projections`
  now binds the projection to itself in that case);
- a method with an UNTYPED receiver parameter, `(unbox [b] : Inner)`, took
  its associated result from the representative instance, not `(Inner B)`.

## Fixtures

`errors/literal-at-assoc-type-return-refused`,
`errors/literal-at-caller-tyvar-arg-refused`,
`errors/literal-at-assoc-type-method-arg-refused`; the relay and untyped
shapes are `typeclass-assoc-nullary-method-generic` and
`assoc-type-method-applied-opaque-head`, which the first cut of the return
check failed.
