# A method whose only class-typed argument is an associated type cannot be called

**RESOLVED 2026-11-05** -- see "Resolution" at the end.

**Severity:** low (refusal, with a workaround). In a class with an associated
type, a method whose parameters mention the class variable only through that
associated type -- `(peek [h : Handle] : int)`, or `NumArray`'s view methods
`(na-view-ref [w : View i : int] : Elem)` -- is not dispatched by the
elaborator:

- at a concrete type, `(na-view-set! w 0 9.9)` with `w : F64View` is
  `TUR-E0015: no instance of typeclass 'NumArray' for type 'F64View'`;
- in a generic, a method whose result is also an associated type is taken for
  a return-directed one: `(let [x (na-view-ref w i)] ...)` is "cannot infer
  type for return-directed method 'na-view-ref'".

The emitter half was fixed when this was found: a generic call whose receiver
is typed `(View V)` now dispatches on `V` in each specialization
(`emit_dispatch_tyvar`, fixture `assoc-typed-receiver-dispatch`); before, every
specialization silently called the representative instance's method.

## Repro

`tests/fixtures/assoc-typed-receiver-dispatch/input.tur` with
`(grab [s] ...)` removed from the generic, calling `(peek h)` on a concrete
`AHandle` instead -- E0015.

## Workaround

Give such a method the class-typed value as its first parameter and ignore it
in the instance (`(na-view-ref [^borrow a : v w : View i : int] : Elem)`):
that is what `NumArray` does.

## Fix directions

`elab_try_return_dispatch` should not count a method as return-only when a
parameter is typed by one of the class's associated types, and
`elab_method_call` should dispatch such an argument: on `V` for a projection
`(View V)`, and on the instance whose `View` binding is the concrete type for
a concrete one (refusing when several instances bind it).

## Resolution (2026-11-05)

All three pieces of the fix directions, in `src/compiler/elab_typeclasses.c`:

- **Concrete receiver.** `elab_method_call` dispatches a method whose first
  parameter is an associated type on the instance whose binding of that type
  is the argument's type: `(peek (:: 3 AHandle))` runs `Store [A]`'s `peek`
  when that instance says `(type Handle = AHandle)`. Two instances binding it
  to the same type are TUR-E0020, naming the associated type and the type,
  rather than a pick by registration order.
- **Not return-directed.** `elab_try_return_dispatch` and
  `elab_symbol_is_return_dispatch_method` skip a method with a parameter typed
  by an associated type (`method_has_assoc_param`): the argument pins the
  instance, so argument dispatch takes the call. `(bump h)` with a result of
  `Handle` was "cannot infer type for return-directed method".
- **The result in a generic.** A receiver holding the projection `(Handle C)`
  dispatches on `C`, so its result is read at `C`: the same projection for a
  `Handle` result, `(Size C)` for another associated type, `C` for the class
  variable. It took the representative instance's binding before, and once the
  concrete rule above existed a chained `(peek (bump h))` would have dispatched
  statically on that type in every specialization.

Pinned by `tests/fixtures/assoc-typed-receiver-concrete-and-chained` (both
engines) and `tests/fixtures/errors/assoc-typed-receiver-ambiguous`.
`stdlib/numvec.tur` keeps its workaround (the array passed beside the view):
it is still correct, and removing it changes the public signatures.
