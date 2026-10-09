# An associated type cannot be a nullary method's only class mention, or named at a type variable

**Narrowed again 2026-10-09: half 1 is fixed.** `(empty [] : Inner)` can be
called: `(let [z : (Inner W) (empty)] ...)` dispatches to `Box [W]` through the
projection the annotation spelled, `(:: (empty) Pt)` through the one instance
that binds `Inner` to `Pt`, and the method is called in a `let`, an
ascription, a field read and a `defn` body alike, on both back ends. Two
instances binding `Inner` to the same type refuse a bare ascription with a
message that says to spell the projection; a call with no expected type at
all keeps the existing "cannot infer type for return-directed method" (see
"Fixed 2026-10-09: half 1" at the end). **What is left** is below that: a
call whose expected type is neither spelled nor unique.

**Severity: low** (was medium) -- a hard error at `tur check` in the one
shape that is left, so nothing miscompiles. Both halves of the original
report cost the ability to write a class over an associated type the way the
guide's own shape suggests: one method spelling was unreachable and generic
code over such a class could not name the projection, so the class was usable
only one concrete instance at a time.

**Narrowed again 2026-10-03: half 2 is fixed** -- `(Inner A)` at a type
variable stays an unreduced projection and each call reduces it (see "Fixed
2026-10-03: the projection at a type variable" at the end).  **Half 1 is
what is left.**

**Narrowed 2026-10-03: the third blocker of half 2 is fixed** -- a
constrained generic that does not name the projection, in a module that holds
no instance, no longer fails TUR-E0015 "declares no instance at all" when the
importer declares the instances (see "Fixed 2026-10-03" at the end).  Half 1
(a nullary method whose only class mention is the associated type) and the
projection at a type variable stay open: both need an unreduced projection
type the instantiation can reduce later.

**Status: OPEN.** Found 2026-09-29 implementing `DeltaCRDT` for
[crdt-spice-plan](../upcoming/crdt-spice-plan.md) C4, on `tur` v0.56.3
(`origin/main` at `dc95b2fdc`). Both are worked around in the spice and the
workarounds are recorded at the class definition in
`spices/crdt/src/crdt/lattice.tur`.

## Half 1 -- a nullary method returning only the associated type is unreachable

```turmeric
(defclass Box [a]
  (type Inner : Type)
  (empty [] : Inner)                 ;; declared fine
  (unwrap [x : a] : Inner))
(defopaque W :int)
(definstance Box [W] (type Inner = int) (empty [] 0) (unwrap [x] (:: x int)))
```

The class and the instance both compile. The method cannot be CALLED:

```turmeric
(empty)                              ;; error: unknown function or operator 'empty'
(let [z : (Inner W) (empty)] ...)    ;; error: unknown function or operator 'empty'
(:: (empty) int)                     ;; error: ascribed type does not match the result shape of 'empty'
```

Dispatch reads the first parameter's type, and `empty` has none -- the class
variable `a` appears nowhere in its signature, only `Inner` does. So there is
nothing to resolve the instance from, and the method is never registered.
Annotating the binding with the projection does not rescue it: the third
spelling proves the name IS known to the ascription path, so the two halves
disagree about whether the method exists.

`bottom` in `stdlib/typeclass-lattice.tur` is the same nullary shape and works
-- because its return type is the class variable itself, which the
return-directed path can resolve from an ascription. It is specifically the
**associated type in return position with no other mention** that has no path.

A two-parameter class with a functional dependency is not an alternative:

```turmeric
(defclass Box [a d] | (a -> d)
  (combine [p : d q : d] : d))
;; TUR-E0015: no instance of typeclass 'Box' for type 'D' (method '.combine')
```

Dispatch reads the FIRST class variable only, so a method mentioning only `d`
looks for an instance at the delta type and finds none. The fundep does not
run backwards.

**Workaround:** give the method a witness parameter of the class type that it
does not read -- `(empty [^borrow x : a] : Inner)`. That works, and it is what
`DeltaCRDT`'s `delta-bottom` and `delta-join` do. It is visible in the API and
a caller has to hold a value of the state type to ask for the empty delta,
which is exactly the wrong requirement for a relay that buffers deltas without
holding a state.

## Half 2 -- the projection is rejected at a type variable

```turmeric
(defn take-concrete [d : (Inner W)] : int d)                 ;; fine
(defn take-generic [A] [(Box A)] [x : A d : (Inner A)] : int ;; rejected
  (unwrap x))
```

```
error: no instance binding for associated type 'Inner' at this type
```

Projection at a CONCRETE type reduces as
[the guide](../guides/typeclass-guide.md) documents. At a type VARIABLE, inside a
generic constrained by the very class that declares the associated type, it is
refused -- which is the only place a projection is load-bearing, since a
concrete type could always have been spelled directly.

The consequence is that no generic function over the class can take or return
a `Delta`. C4 therefore ships no generic delta helpers at all: every caller of
`apply-delta` names a concrete state type, and a `(defn apply-all [A]
[(DeltaCRDT A)] [x : A ds : (Vec (Delta A))] : A ...)` cannot be written.

A generic that does NOT name the projection in its signature is also blocked,
for a third reason:

```turmeric
(defn neutral? [A] [(DeltaCRDT A) (Eq A)] [x : A y : A ^borrow w : A] : bool
  (eq? (apply-delta x (delta-bottom w)) y))
```

```
TUR-E0015: 'apply-delta' is a method of typeclass 'DeltaCRDT', but this
program declares no 'DeltaCRDT' instance at all
```

-- when the generic and the class live in a module that holds no instance
itself, which is the normal layout (the class beside the vocabulary, the
instances beside the types). The instances exist in the program; the error
fires anyway.

## Why the class still shipped

The witness parameter makes half 1 survivable, and half 2 is survivable
because a CRDT caller knows its concrete state type. The associated type is
still earning its keep: it is what lets `ORSet`'s delta be a different type
from `ORSet` (a dot-store fragment plus an exact `DotSet`) while `GCounter`'s
delta is a `GCounter`. Forcing `Delta = a` for every instance would have cost
the causal types their compression, and erasing the delta to bytes would have
cost the checking.

## Fix directions

Half 1: dispatch needs a path for "the class variable appears only through an
associated-type projection in the return", which is what the concrete
projection machinery already resolves at annotation sites. The ascription
error message ("does not match the result shape") suggests the ascription path
computes the projection and then compares against something that was never
built for a nullary method.

Half 2: the projection reducer bails when the argument is a `TY_TYVAR` rather
than deferring the reduction into the instantiation, which is what the
constrained-generic monomorphizer would later supply. This is the same place
the archived
[typeclass-associated-types-missing](../archive/typeclass-associated-types-missing.md)
work landed.

Fixtures worth having: a nullary associated-type method called through an
ascription; a constrained generic taking `(Assoc A)`; and the
class-in-one-module, instances-in-another layout, which is the shape every
spice will hit.

## Fixed 2026-10-09: half 1

Return-directed dispatch (`elab_try_return_dispatch`,
`src/compiler/elab_typeclasses.c`) found a method by looking for a class
variable in its result and in none of its parameters -- which `(empty [] :
Inner)` never has: its result is the associated type's own name. So `(empty)`
was "unknown function or operator" -- unless a stdlib class had a
return-directed method of the same name, and `Alternative` has an `empty`,
`(empty [] : (f a))`. The search took the first class with the name, so every
ascription on `Box`'s `empty` was measured against `(f a)`: that is where
"ascribed type does not match the result shape" came from, and why the report
could not see that its own method was never a candidate.

- Every class with a return-only method of the name is a candidate now; a
  method whose result names one of its class's associated types is one, with
  the class's first parameter as its dispatch variable. When several classes
  qualify, the one whose result fits the expected type is chosen (the others'
  diagnostics no longer apply to it).
- An associated-type result is dispatched through the instance that binds
  the associated type to the expected type. The annotation that spelled the
  projection names the instance: `type_expr_from_form` records the last
  concrete projection it reduced (`Elab.assoc_hint_*`: `(Inner W)` -> `int`,
  with `W`), and the dispatch takes `W` when its result is that associated
  type and the expected type is that reduction; a `let` init or an ascribed
  expression consumes the hint. A bare type -- `(:: (empty) Pt)` -- selects
  the one instance binding the associated type to it, and when several do the
  call is refused: "2 instances of 'Box' bind 'Inner' to int; say which one by
  spelling the result as the projection, e.g. (:: (empty) (Inner T))".
- With no expected type at all the call keeps the existing "cannot infer type
  for return-directed method" refusal.

Pinned by `tests/fixtures/typeclass-assoc-nullary-method` (both engines: the
projection spelling in a `let` and in a `defn`, a bare ascription at a struct
type and at `int`, the colliding stdlib name) and
`tests/fixtures/errors/typeclass-assoc-nullary-method-ambiguous` (two
instances binding `Inner` to `int`).

## Fixed 2026-10-03: a constrained generic in an instance-less module

The `neutral?` shape above -- a generic over the class, in the module that
declares the class and no instance -- failed because an imported module is
elaborated whole at the import, before any of the importer's instances is
registered, and a method call on a constrained type variable needs a
*representative* instance to elaborate against (emit and the interpreter
re-resolve it per instantiation).  With none registered yet, the dispatch fell
through to the "declares no instance at all" diagnostic for a program that
declares several.  It had nothing to do with associated types: any class
reaches it.

Such a defn is now **parked** rather than reported (`elab_module.c`,
`noinst_park` / `elab_noinst_retry`): in an imported module a defn is
attempted under a capture frame, and a failure that is only that diagnostic
(counted by `Elab.noinst_failures`) waits.  At the importer's next statement
boundary after a new instance registers, the parked defns are retried with
their module context restored, and a success is appended to that module's
body.  Whatever is still parked when the program ends is elaborated once more
for real, so a genuinely instance-less program still reports TUR-E0015 against
each defn that needs an instance.  An instance declared after its use, a
nested import chain (a third module's generic over the parked ones), and
parked generics calling each other all resolve, on both back ends.  Pinned by
`tests/fixtures/typeclass-generic-in-instance-less-module`.

Not covered: separate compilation (`tur build <dir>` compiling the class
module as its own translation unit) still has no instance to elaborate the
generic against, and `tur check` of the class module ALONE still reports the
error -- in both cases the instances are genuinely absent from the program
being elaborated.

## Fixed 2026-10-03: the projection at a type variable

```turmeric
(defn take-generic [A] [(Box A)] [x : A d : (Inner A)] : (Inner A) d)
(defn get [A] [(Box A)] [x : A] : (Inner A) (peel x))
(defn count-all [A] [(Box A)] [x : A ds : (Vec (Inner A))] : int (vec-len ds))
```

all compile now, on both back ends, including an instance whose `Inner` is a
by-value struct.

- **Representation.**  `(Inner A)` with `A` a type variable is a named
  `TY_TYVAR` carrying `assoc_of` / `assoc_arg` (types.h), named by the printed
  projection, so two spellings of it are the same variable.  Everything that
  treats a type variable as abstract treats the projection the same way, so
  inside the generic it is an opaque type (built by `elab_assoc_projection`,
  elab_types.c, from both projection sites -- `type_expr_from_form` and
  `fn_type_from_form_impl`).  The signature checks that refuse a lone bare
  type variable as a typo leave a projection alone; it is determined by its
  argument.
- **Reduction.**  Once a call's arguments are in, `call_bind_assoc_projections`
  (elab_call.c) reduces each projection the callee's signature mentions:
  with `A` fixed to a ground type it is the instance's binding, which an
  argument typed `(Inner A)` must agree with (`TUR-E0001`, naming the
  projection) and which is recorded under the projection's name -- so the
  result instantiates by name and the emitter's per-call clone substitutes the
  real C type; with no instance it is `TUR-E0015`; at another type variable
  (`A := B` in a generic caller) it becomes `(Inner B)`.
- **Methods.**  A class method whose declared result is one of the class's
  associated types, called on a type-variable receiver, yields the projection
  `(Inner A)` instead of the carrier representative's binding.

Pinned by `tests/fixtures/assoc-type-projection-at-tyvar` (a projection param
and result, a generic relaying to a generic, `(Vec (Inner A))`, a method
result through an `if`, int and by-value-struct instances),
`assoc-type-projection-across-modules` (class and generic in an
instance-less module, instances in the importer, both engines),
`errors/assoc-type-projection-mismatch` and
`errors/assoc-type-projection-no-instance`.  `run.sh`, `run-turi.sh` green.

Not changed: a type-variable body is still accepted for any declared result
(`(defn f [A] [x : A] : int x)` checks clean), so `(peel x)` returned as `:
int` is not refused -- that leniency is general, not the projection's.

## Still open: half 1

A nullary method whose only class mention is the associated type,
`(empty [] : Inner)`, still has nothing to dispatch on.  With half 2 in place
the natural spelling inside a generic would be expected-type dispatch on a
PROJECTION -- `(:: (empty) (Inner A))` names `A`, which picks the instance --
but at a concrete type the projection is reduced where it is parsed
(`(Inner W)` is just `int`), so the instance is gone by the time the call is
typed.  A fix needs the ascription to keep the projection it was written as
(or the return-dispatch path to accept one), which is the design question
this half is left on.  The witness-parameter workaround above still applies.
