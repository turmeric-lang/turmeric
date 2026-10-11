# A Hash instance on an int-backed opaque breaks every `any`-keyed map

**Severity: low-medium (compile failure in unrelated code).** Filed
2026-10-11 while executing [parsec-guide-plan](../archive/parsec-guide-plan.md).
Reproduces on `main` at v0.64.0.

## Repro

```turmeric
#lang saffron
(defopaque Tag :int)
(definstance Hash [Tag] (hash [x] 0))
(define m #map{"a" 1})
(println (map-get m "a"))
```

```
stdlib/map.tur:576:8: error [TUR-E0020]: cannot dispatch '.hash' on an 'any'
receiver: the box holds one type at runtime, and which instance to run is not
decidable from it here
```

Delete the `definstance` and it compiles. The map literal's key is `any`, and
`(hash k)` inside `hamt-of` used to resolve to `Hash [any]`
(stdlib/typeclass-hash.tur). With a `Hash` instance on a non-heap, int-carried
opaque in scope, instance selection for the `any` receiver no longer lands on
`Hash [any]`, and the dynamic-dispatch path then refuses.

Found by giving `stdlib/char.tur`'s auto-loaded `Char` a `Hash` instance: 13
Saffron and map fixtures failed. `Char` ships without `Hash` for that reason.

## Fix direction

When an `any` receiver meets a class that has a `[any]` instance, select that
instance before ranking the others (`elab_method_call`'s `best_inst`, around
the `obj->type.kind == TY_ANY` check in src/compiler/elab_typeclasses.c).
