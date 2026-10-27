# E2c: an effectful fn-value stored in a STRUCT FIELD, called via `.field`, threads onto the DK (capability-field cluster)

**STATUS: RESOLVED.** All four capability-field fixtures DK-lower with correct
output and zero `eff=1`: `effect-struct-field-row` (`struct field row`),
`capability-effect-poly` (`hello`/`world`), `effect-type-alias` (`type alias
test`), `effect-subtype-capability` (`hello from pure`).  Flag-off byte-identical
(all fixture snapshots unchanged); flag-on effect soundness sweep clean; full
suite green (2203/0).

## The shape

```turmeric
(defeffect Emit [s : cstr] : nil)
(defstruct Emitter :copy [run : fn #fx{Emit}])      ; effectful capability field
(defn main [] : int
  (let [em (make-struct Emitter (fn [s] (perform (Emit s))))]
    (handle
      (do (.run em "struct field row") 0)            ; field-accessor call
      (Emit [s] k) (do (println s) (resume k nil)))))
```

`capability-effect-poly` stores a NAMED fn; `effect-subtype-capability` stores a
PURE fn (effect subtyping) -- all covered.

## Why it works (dynamic registry threading, no escape analysis needed)

The key realization: threading `(.field obj args)` to the fn-value's `__cps` via
`__tur_cps_lookup(obj.field)` and passing `__kont` is SOUND regardless of where
the struct is or whether it escapes -- the fn-value's perform reaches whatever
handler is on the DK at the call site, exactly as the fiber path would.  So no
"struct doesn't escape" proof is required; the fn-value just has to be
REGISTERED (have a `__cps`) so the lookup resolves.

## The four coordinated parts (all flag-gated on cps-tramp-resume)

1. **`(make-struct S ...)` lowers to a CONSTRUCTOR call** (`e->as.call_.ctor`
   set), so a fn-value stored in a field arrives as a ctor-call arg, not an
   `EX_MAKE_STRUCT`.  `fnval_stored_in_struct` (emit_cps_ir.c) walks the program
   for a ctor-call arg matching the fn-value whose ctor field carries an
   EFFECTFUL row; the registration loop `threadable_add`s it.
2. **Force-color a PURE fn-value in an effectful field** (cps.c
   `cps_force_color_eff_fnval_args`, ctor-arg case): a pure capability fn (e.g.
   `pure-greet`) must be colored to get a `__cps` to register -- else the
   `.field` thread's lookup finds nothing and the program emits empty output.
3. **`call_is_effectful_fnvalue`** (cps_ir.c) reads the field row off the
   CtorField for a `(.f obj)` callee (the field-access node's TY_FN type reads
   empty) so the E2c call is recognized as effectful.
4. **Thread the `.field` call** (cps_ir.c `cps_tail`/`cps_bind`): a `via_registry`
   `CT_TAILCALL` whose callee is the field-load atom (`CT_TAILCALL.fn == NULL`,
   new `fn_atom` field carries the `(.f obj)` load); the emitter
   (emit_cps_ir.c) uses `atom_str(fn_atom)` as the `__tur_cps_lookup` key.

Prerequisites (landed earlier): effect-row resolution on the CtorField
(commit 4801664) + its GADT init companion (commit 13aa356).

## Context

Complements E2a (fn-value PARAM threading, docs/archive/cps-e2-rowpoly-...).
Same registry channel (`__tur_cps_lookup`), different callee provenance (a struct
field load vs a param).
