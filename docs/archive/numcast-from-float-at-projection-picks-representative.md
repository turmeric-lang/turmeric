# A return-dispatched method at another class's associated type ran the representative instance

**Resolved 2026-10-29.** Severity was medium (silent wrong values; the engines
disagreed).

Found while adding `NumCast` (stdlib/typeclass.tur) for generic numeric code
(simd-and-vectorization-plan section 3.1).

## Repro

```turmeric
(defclass NA [v] (type Elem : Type) (na-ref [^borrow a : v i : int] : Elem))
;; instances for F64Vec (Elem = float) and I32Vec (Elem = int32)
(defn z2 [V] [(NA V) (NumCast (Elem V))] [^borrow a : V] : (Elem V)
  (from-float 2.75))
```

`(z2 a)` on an `F64Vec` printed `2` compiled -- the int instance's
`from-float` -- and `2.75` under turi. Under turi, a generic that built a
value with `(from-int 0)` and was called from another generic gave the value
the int instance's runtime tag, so a later `(to-float ...)` on it printed
6.75 as `-4.61169e+18` (compiled was right).

## Cause

`from-float`'s class variable is only in its result, so the call is
return-dispatched at the expected type `(Elem V)`. Both re-resolution paths
treated an associated-type result as "dispatch on its argument `V`" -- right
when the class being dispatched DECLARES that associated type (a method
returning its own class's `Elem`), wrong for `NumCast`, whose instances are
keyed on the element type itself. No `NumCast` instance exists for the array
type, so the representative (the int instance) stayed.

- emit: `emit_dispatch_tyvar` (`src/compiler/emit_core.c`) maps the
  projection to its argument only when the dict's class owns the associated
  type; otherwise it hands back the projection, which `emit_resolve_type`
  reduces to the element type.
- turi: the same test at the frame-dictionary dispatch (`src/turi/eval.c`),
  and `frame_bind_constraint_dicts` binds a dictionary for a constraint on a
  projection (`(NumCast (Elem V))`) by reducing it through the frame's pin of
  `V` (`frame_reduce_projection`) when the caller's call did not pin the
  projection concretely -- a generic called from a generic.

## Fixture

`numcast-as-in-generics` (`two-and-three-quarters`, `mean`).
