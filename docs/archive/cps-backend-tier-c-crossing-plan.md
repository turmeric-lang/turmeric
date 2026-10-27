---
title: "CPS backend Tier C -- wide by-value aggregate slot crossing: investigation + prerequisites"
status: done
description: Graduation gate item 3 (Tier C) asks that wide by-value aggregates crossing the DK slot as an effect payload / resume value / function return box at the boundary. The box-at-boundary emit design is settled and small. This document records what the investigation found when trying to LAND it: every source program that would route a by-value aggregate across the DK slot is blocked by an upstream prerequisite OUTSIDE the CPS backend's emit path (effect payload typing, the direct-path fiber crossing, the shift-body struct translation, and the function-return Type carrying a NULL ADT def with classification running before any EmitCtx exists to resolve it). Tier C cannot land a round-trip fixture until at least one of those crossings has a working direct baseline AND a populated crossing Type. This plan reframes Tier C as gated on those prerequisites and lays out the fix order.
---

## Why this exists

The `cps-backend` graduation gate
([cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md#graduation-gate----what-must-hold-before-cps-backend-goes-always-on),
item 3) requires that **wide by-value aggregates that cross the one-word DK
slot** -- as an effect payload, a resume value, or a function return -- are
emitted natively (boxed at the boundary), not left on the whole-function
fallback. N3 landed the *local* struct/ADT slice (a by-value aggregate that
stays a local and never touches the slot); Tier C is the remaining *crossing*
slice.

The **emit design is settled and small** (see "The box-at-boundary design"
below): thread the full `Type` to the six DK slot boundaries, and when it is an
owning-free by-value product, heap-copy on store / deref on load. A prototype of
exactly that was written and builds clean. The blocker is not the design -- it
is that **no source program in the current pipeline actually routes a by-value
aggregate across the DK slot with a working direct baseline**, so there is no
round-trip fixture to assert `direct == cps` against (the discipline every
CPS-backend phase follows). This document records the five prerequisites the
investigation hit, each with a repro, so the next step is unambiguous.

## The box-at-boundary design (settled)

For a by-value aggregate type `T` (a `tur_adt_<Name>` struct that can exceed one
word) crossing the DK slot:

- **store** (`slot_store`): `(intptr_t)({ T *__bx = malloc(sizeof(T)); *__bx = (v); __bx; })`
- **load** (`slot_load`): `(*(T *)(slot))`, and at a single-shot boundary
  (the root entry unwrap) additionally `free` the box.

Restricted to **owning-free** products (`AdtDef.needs_drop_glue == false`): the
box is then a pure bitwise value copy with no refcount / drop concern, safe even
under a multi-shot resume (each crossing gets its own copy). Aggregates with
owning fields (rc / ref / weak) stay on the fallback -- their crossing needs
retain-on-copy + drop glue (that is N3-O2 / gate item 4, not Tier C). Carrier
ADTs (heap Vec/Map, parametric apps) also stay out: their bit pattern already IS
the int64 carrier, and bit-copying an owning handle would duplicate its
refcount.

The predicate is `type_is_byvalue_adt_product(t) && !def->needs_drop_glue`. The
six boundaries (deliver, lifted-frame value param, handler-case arg, perform arg,
resume value + result, entry unwrap) already have `slot_store`/`slot_load` seams
from Tier B; Tier C only widens them from `TypeKind` to the full `Type` and adds
the box/unbox arm. Classification (`fn_sig_ok`, `atom_ok`, the subset predicates)
widens from `slot_ty(kind)` to `slot_ty(kind) || slot_box_ty(type)` wherever the
full `Type` is in hand (it is -- every `CVar`/`CAtom` carries it since N3).

None of that is the hard part. The hard part is below.

## The five prerequisites (each blocks a different crossing)

Every way to route a by-value aggregate across the DK slot was tried. Each hits
a distinct upstream gap. `Pr` below is `(defstruct Pr [first : int second : int])`
-- a 16-byte owning-free by-value product, the ideal Tier C probe.

### P1 -- effects cannot carry an aggregate *return* type -- **RESOLVED**

**Resolved.** `defeffect` now resolves a user-type return annotation (a keyword
`:Pr` is given an `F_SYM` view and routed through `type_expr_from_form`) instead
of rejecting it, and records the full result Type on the effect constructor
(`EffectConstructor.result_full_type`, set post-registration). `perform` reads it
so the perform result carries the real monomorphized `Pr` def rather than a
def-less `TY_ADT`. With the CPS binder declarations also using the full Type
(`binder_ctype_full` for `CT_RESUME` / `CT_LETCONT`), a by-value aggregate crosses
the effect boundary (perform result, resume value, resume result) boxed. Fixture
`tests/fixtures/cps-backend-tierc-effect/` (`Get [] :Pr`, CPS `== 42`). The direct
baseline still fails to compile (P2 below), so the fixture asserts the CPS value
(the float-effect precedent). The original analysis is kept for the record:


```turmeric
(defeffect Get [] : Pr)          ; error: defeffect: unknown return type 'Pr'
```

An effect's value type is restricted to the built-in/known set; a user struct is
rejected at `defeffect`. So the "effect resumes with a by-value record" crossing
(the N3-TierC round-trip target as literally written) is not expressible.
**Fix locus:** effect declaration elaboration (accept ADT/struct value types).

### P2 -- an aggregate returned *across an effect* breaks the direct/fiber path -- **RESOLVED**

**Resolved.** The fiber effect ABI is int64-based (`tur_effect_perform` /
`tur_effect_cont_resume` / the dispatch fn all pass/return `int64_t`), so a
by-value aggregate could not ride it by a cast. `emit_effects.c` now boxes it at
every fiber-effect boundary (heap-copy + pointer, `eff_box_expr`, gated on
`eff_type_boxed` = owning-free by-value product) and unboxes on the consuming
side: the perform argument (box) and result (unbox), the handler-case param
(unbox) and return (box), the body fiber's result slot (box), the resume value
(box) and result (unbox), and the handle-site dispatch result (unbox). The
fiber handler/body functions' `current_fn_ret_ctype` is set to the real ABI
type (`int64_t` / `void`) so a pending-panic propagation return is well-typed.
This is **purely additive** -- an aggregate effect value was a parse error
before P1, so no existing (scalar) effect path changed; the full suite is green.
Now `direct == cps` for aggregate effect crossings (`cps-backend-tierc-effect`,
`cps-backend-tierc-effect-arg`). The original analysis is kept for the record:


```turmeric
(defn make-pr [] : Pr (let [v (perform (E))] (make-struct Pr :first v :second 10)))
(defn run [] : Pr (handle (make-pr) (E [] k) (resume k 32)))
```

`tur run` (direct style) fails to **compile**:
`tur_adt_Pr __t = (tur_adt_Pr)__dispatch(...)` -- the fiber effect path passes
the continuation result as an `int64_t` and casts it back to the struct, which C
rejects. So even where the CPS backend could box it correctly, there is **no
direct baseline** to assert `direct == cps` against (the same class as the
`fiber-effect-float-result-truncated` bug, but a hard compile error rather than a
wrong value). **Fix locus:** the direct/fiber emitter's non-scalar
continuation-result crossing.

### P3 -- `make-struct` inside a `shift` body is untranslated (CT_UNSUPPORTED) -- **RESOLVED (T-C2)**

**Resolved.** The translation was actually *two* gaps, both now closed:
(1) `joins_closed_rec` was missing a `CT_LETRAW` case, so a shift/reset body
containing a delegated `make-struct` was rejected (fixed in T-C1); (2) the
shift-body helper delivered its aggregate value to the prompt (`KK_PROMPT`)
without boxing, because a *cross-function* shift's prompt result type is not
carried by the enclosing `cur_ty`. Fixed by having `CT_APPCONT` pass the
delivered atom's own `Type` to `emit_deliver` (the atom carries the real `Pr`
type); the box is then selected from that. Fixture
`tests/fixtures/cps-backend-tierc-shift/` (`direct == cps == 42`). The original
analysis is kept for the record:


```turmeric
(defn inner [] : Pr (shift (fn [v : Pr] v) (make-struct Pr :first 32 :second 10)))
```

`--dump-cps` shows the shift body as `<unsupported: form not in CPS2 subset>`.
The `CT_LETRAW` delegation that lets N3 emit `make-struct` as a local was wired
for the main-body / perform-continuation positions, not for a shift body. So the
"shift produces an aggregate delivered to the prompt" crossing never reaches the
emit path. **Fix locus:** `cps_ir.c` -- delegate `make-struct`/get-field in the
shift-body translation (`cps_shift_body`), same as the main body.

### P4 -- a function's aggregate *return* Type carries a NULL ADT def, and classification runs before any `EmitCtx` exists -- **RESOLVED (T-C1)**

**Resolved** by `fn_ret_type(fd)` preferring `fd->body->type` (which carries the
real def and is the value delivered to `KK_RET`) over the NULL-def
`return_type`, so no `EmitCtx` is needed at classification time. See T-C1 in the
reframed plan below. The original analysis is kept for the record:


This is the closest miss. A colored function that builds a `Pr` from constants
*after* a `reset` and returns it has a **working direct baseline** and a clean,
fully-translated CPS IR:

```turmeric
(defn inner [] : int (shift (fn [v : int] v) 32))
(defn outer [] : Pr
  (let [n (reset (inner))]
    (make-struct Pr :first 40 :second 2)))        ; $ tur run  => 42  (direct works!)
```

```
cps-fn outer [] k:cont<_>
  reset n { tailcall inner(<prompt>) }
  let t0 = <make-struct>          ; CT_LETRAW, real def
  (k t0)                          ; deliver Pr to KK_RET
```

Yet `outer` is not admitted. Two coupled causes:

1. **`fd->return_type` for an aggregate-returning fn is a `TY_ADT` with
   `as.adt_.def == NULL`.** The value site (`make-struct`'s result) has the real
   def, but the *return* Type does not, so the Tier C predicate cannot see it is
   a by-value product -- and the entry-unwrap `slot_load` cannot name the C type
   to box against.
2. **Classification (`ensure_S` -> `fn_sig_ok`) runs from
   `emit_runtime_preamble` (emit_module.c), which has no `EmitCtx`.** So it
   cannot call `emit_resolve_type` to populate the def, and by the time
   `emit_cps_ir_try_fn` runs (which does have `ctx`), the classification is
   already cached.

**Fix locus:** either populate `FnDef.return_type`'s ADT def during elaboration,
or thread `EmitCtx` (or a resolve hook) into `emit_cps_ir_program_has_emittable`
/ `ensure_S` so classification can resolve the crossing Type. P4 alone would
unlock the *return-crossing* slice (a real round-trip fixture, since the direct
baseline works), independent of P1/P2/P3/P5.

### P5 -- an aggregate effect *argument* loses its type in the handler -- **RESOLVED**

**Resolved.** `defeffect` now resolves and stores the full parameter Types
(`EffectConstructor.param_full_types`, populated when any param is an aggregate,
resolving a keyword `:Pr` the same way as the result type), and the handler-case
param binding uses the full Type instead of `type_from_kind(param_types[j])`, so
`p` carries the real `Pr` def and field access elaborates. The aggregate effect
argument then crosses the slot boxed at the `dk_perform` argument and is unboxed
at the handler case. Fixture `tests/fixtures/cps-backend-tierc-effect-arg/`
(`Put [p : Pr]`, CPS `== 42`; direct baseline blocked on P2). The original
analysis is kept for the record:


```turmeric
(defeffect Put [p : Pr] : int)
(defn run [] : int (handle (use-put) (Put [p] k) (resume k (+ (.first p) (.second p)))))
;; error: no typeclass method found for 'first'   -- p's type is erased in the case
```

The handler-case param `p` does not retain its `Pr` type, so field access on it
fails to elaborate. So the "aggregate as effect argument" crossing is blocked
before codegen. **Fix locus:** handler-case param typing in elaboration.

## Reframed Tier C plan

Tier C's emit design is done-on-paper and cheap; it is gated on landing at least
one crossing with (a) a populated crossing `Type` and (b) a working direct
baseline. In ascending order of independence:

- **T-C1 (smallest real slice): the return crossing. -- LANDED.** The full
  box-at-boundary machinery is wired: `slot_store`/`slot_load` take the full
  `Type` and box/unbox an owning-free by-value product
  (`type_is_byvalue_adt_product && !needs_drop_glue`); the `Type` is threaded to
  all six DK boundaries (deliver via `CE.ret_ty`/`cur_ty`, lifted-frame value
  param, handler-case arg, perform arg, resume value + result, entry unwrap);
  `atom_ok` / `fn_sig_ok` / the subset predicates widen from `slot_ty(kind)` to
  `slot_ty(kind) || slot_box_ty(type)`.

  **P4 resolved without ctx threading.** The insight: `fd->return_type`'s NULL
  ADT def is a surface-annotation artifact, but `fd->body->type` carries the
  real monomorphized def and is *exactly* the value delivered to `KK_RET`. A new
  `fn_ret_type(fd)` prefers the body Type for an aggregate return, so
  classification (which runs at preamble time, before any `EmitCtx`) sees the
  real def -- no signature churn needed. Two small fixes fell out: `emit_params`
  / the entry return type now use the full param/return `Type`
  (`emit_type_from_kind(TY_ADT)` loses the def), and **`joins_closed_rec` gained
  a `CT_LETRAW` case** (it was missing, so any reset/handle continuation
  containing a delegated `make-struct`/get-field was wrongly rejected -- a
  pre-existing N3 gap this surfaced).

  Round-trip fixture `tests/fixtures/cps-backend-tierc-return/`: `outer` returns
  a `Pr` built after a cross-function `reset`; the reset-cont helper boxes it
  (`dk_run(k, (intptr_t)malloc-copy)`) and the entry wrapper unboxes + **frees**
  it (single-shot root). `direct == cps == 42`. The return box is freed; the
  delimited-control DK nodes leak (`requires.no-leak-check`, same as
  `cps-backend-xfn-reset`). Full suite: 2006 passed, 0 failed.
- **T-C2: the reset/shift-result crossing. -- LANDED (P3 resolved).** A
  cross-function shift produces a `Pr` and delivers it to the prompt; the
  shift-body helper boxes it and the reset continuation unboxes it. The box is
  selected from the delivered atom's Type (a cross-function prompt's result type
  is not in the enclosing `cur_ty`). Fixture
  `tests/fixtures/cps-backend-tierc-shift/` (`direct == cps == 42`). This box is
  loaded by the reset continuation without a free (`consume=false`, multi-shot
  safety) so it leaks alongside the DK nodes (`requires.no-leak-check`).
- **T-C3: the effect payload / resume-value crossings. -- LANDED (P1 + P5 + P2
  resolved).** An effect declaring an aggregate *result* value threads it boxed
  through perform / resume (`cps-backend-tierc-effect`), and an aggregate effect
  *argument* crosses boxed through `dk_perform` into the handler case
  (`cps-backend-tierc-effect-arg`). P2 boxed the same crossings on the direct/
  fiber path, so both fixtures are true `direct == cps` equalities.

**All five prerequisites (P1-P5) are resolved.** The owning-free by-value
aggregate crosses the DK slot -- and the fiber int64 effect ABI -- boxed, in
every position: function return (T-C1), reset/shift result (T-C2), and effect
result / argument (T-C3). Gate item 3 (Tier C) is complete for owning-free
products; owning-field aggregates and carrier handles remain gate item 4.

**Note on leak checking.** In some environments (including the one T-C1/T-C2 were
developed in) LeakSanitizer is not active on the spawned fixture binary -- e.g.
removing `cps-backend-xfn-reset`'s `requires.no-leak-check` still passes. So the
leak posture of these fixtures is reasoned about manually, not certified by the
harness here: any fixture using `reset`/`shift`/`handle` leaks DK nodes and
carries the marker (matching the pre-existing delimited-control fixtures).

Gate item 3 stays open until T-C3 lands; until then every effect-payload
aggregate-crossing colored function keeps the whole-function fallback (correct,
just not native). The return (T-C1) and reset/shift (T-C2) crossings are native.

## Depends on / reuses

- Parent: [cps-backend-non-scalar-values-plan.md](cps-backend-non-scalar-values-plan.md)
  (Tier tiering, the `slot_store`/`slot_load` seams, N3 local struct/ADT
  delegation via `CT_LETRAW`).
- `type_is_byvalue_adt_product` / `adt_is_byvalue_product` / `AdtDef.needs_drop_glue`
  (types.c/.h) -- the settled Tier C predicate.
- Sibling residual (not Tier C): owning-field aggregates cross via gate item 4 /
  N3-O2 ([cps-backend-owning-pointers-plan.md](cps-backend-owning-pointers-plan.md)).

## Out of scope

- Owning-field aggregate crossing (retain-on-copy + drop glue) -- gate item 4.
- Carrier ADTs (heap Vec/Map, parametric apps) crossing -- they ride the int64
  carrier as Tier A once their owning-handle refcount discipline is settled,
  which is again gate item 4, not Tier C.
