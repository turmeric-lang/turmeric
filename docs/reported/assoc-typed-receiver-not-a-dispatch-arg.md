# A method whose only class-typed argument is an associated type cannot be called

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
