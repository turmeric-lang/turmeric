# A generic over a parametric `:heap` sum is never specialized, so a constructor in its body aborts at runtime

**RESOLVED 2026-10-14**, by fix directions 1 and 3, plus two gaps the fix
exposed. Pinned by `tests/fixtures/heap-sum-generic-rebuild`: the repro's
`bump`, a read-only `has?`, a result-only `mk`, a generic calling a generic,
the sum nested in an `Option`, `float`/`cstr`/`bool`/struct elements, and
open slots. One narrower shape stays open as
[heap-sum-unpinned-tyvar-relayed-to-generic](../reported/heap-sum-unpinned-tyvar-relayed-to-generic.md).

## Resolution

- **The clone decision** (fix direction 1). `emit_abi_register_call` forces
  an ABI change when a parameter or the result instantiates to a type that
  is, or carries among its arguments, a ground `:heap` parametric sum
  application, even though the C spelling is unchanged
  (`type_mentions_ground_heap_sum_app`, `src/compiler/emit_module.c:5861`,
  used at `:7048` and `:7395`). This sits next to the G4
  `type_phantom_hides_aggregate` rescue, as the report suggested. The
  predicate itself is `adt_app_is_heap_sum` (`src/compiler/types.c:6067`).
  Distinct element types that spell the same clone name get the existing
  `__h<n>` suffix.
- **The clone read the base layout.** With a clone minted, its `match` still
  read the scrutinee as `tur_adt_Fr *`, because the "upgrade to the
  monomorph" step at the top of the ADT match emitter accepted only by-value
  products. For `int`, `float` and `cstr` the 8-byte slots happen to line up.
  A `cstr` binder was still declared `int64_t`, which gcc 14 and clang 15+
  reject. A `(Fr bool)` read its fields at the wrong offsets. Both upgrade
  sites (`src/compiler/emit_expr.c:20913`, `:20939`) now also accept a
  `:heap` sum app. The word in hand already points at that monomorph, which
  is what `main` uses when the static type is concrete. This also resolved
  [heap-sum-match-binder-of-app-payload-unbridged](heap-sum-match-binder-of-app-payload-unbridged.md).
- **Open slots reached the trap without a generic.** `(match (Nil) ...)`
  with nothing pinning `T`, and `(One 7.1)` of a two-parameter
  `(defdata Two :heap [A B] ...)`, named the base `ctor_Fr_Nil` /
  `ctor_Two_One` straight from `main`. That was true on `main` before this fix
  as well. A constructor stores no value in an open slot
  (`tyvar_.open_slot`), so `emit_ground_open_slots`
  (`src/compiler/emit_expr.c:6470`) grounds it to the `int` carrier. The
  constructor emitter uses it through `emit_heap_ctor_open_suffix` (`:6492`,
  in both the nullary and the n-arg branch). A nullary ctor nothing pins
  types as the bare, unapplied ADT, so there every slot is open.
  `emit_abi_register_call` grounds a call's open-slot bindings the same way
  (`src/compiler/emit_module.c:6942`), but only when that is what exposes a
  ground `:heap` sum. Result/Option open slots ride the carrier and keep the
  bindings elab gave them, which is the case
  `set-add-elem-hash-disagrees-with-set-member` warns about.
- **Not taken:** fix direction 2 (a body walk for suffix-less constructors).
  Direction 1 decides from the signature, and the cost is a clone for a
  read-only generic such as `has?` too. That is correct, just not free.

Suite: `bash tests/run.sh` 3792 passed, 0 failed, with no `expected.c`
snapshot moving. Both new fixtures also pass `tests/run-turi.sh`, gcc 13 and
clang 18 with no warnings, and `tests/check-emitted-float-conversions.py`.


**Severity: medium-high.** A correct program compiles cleanly and then
aborts at runtime. The interpreter runs the same program correctly. The
abort is loud: it prints a message and calls `abort()`.

**Discovered:** 2026-10-13, prototyping
[generic-zipper-plan](generic-zipper-plan.md) (its D2), on
`main` at v0.64.4 (`704cc531`).

## Repro

```turmeric
(defdata Fr :heap [T] (Nil) (Frame T T))
(defn bump [T] [f : (Fr T)] : (Fr T)
  (match f
    (Nil) (Nil)
    (Frame a b) (Frame b a)))
(defn main [] : int
  (match (bump (Frame 1 7))
    (Nil) (println 0)
    (Frame a b) (println (- a b)))
  0)
```

- **Compiled** (`tur run`, with gcc or clang): prints
  `tur: internal error: base constructor 'ctor_Fr_Frame' of a parametric type
  has no definition; a call reached the dead base generic path instead of a
  per-spec clone`, then aborts.
- **`tur interpret`:** prints `6`.
- **Without `:heap`:** `(defdata Fr :copy [T] ...)` prints `6` compiled too.
- **Unaffected by `:copy`:** adding `:heap :copy` aborts the same way.

The emitted `main` calls the unspecialized `bump(int64_t)`. No
`bump__spec__...` clone is minted.

Which generics get a clone depends on the parameter types:

- A generic whose `T`-bearing parameter is `(Cons T)` gets one
  (`mk__spec__int64_t_tur_adt_Cons__R___int64_t`).
- So does one whose parameter is a `:heap` struct (`(Box T)`).
- One whose parameters are only the `:heap` *sum* `(Fr T)` does not. That
  covers both a match-and-rebuild like `bump` and a predicate like
  `(defn has? [T] [f : (Fr T)] : bool ...)`. The predicate just happens not
  to construct anything, so its base body works.

## Root cause

**The clone decision is a C-name comparison.** `emit_abi_register_call`
sets `abi_changes` only when the generic and the instantiated types spell
differently:

- `src/compiler/emit_module.c:6979` compares the parameter types:
  `strcmp(type_c_name(generic_arg), type_c_name(arg_types[i]))`.
- `src/compiler/emit_module.c:7330` compares the result types.

**`type_c_name` spells both `(Fr T)` and `(Fr int)` as `int64_t`.** It names
an ADT application as its monomorph only when `adt_app_is_byvalue_product`
holds (`src/compiler/types.c:4768`): a single-constructor record, such as a
`defstruct`, `(Cons A)` or `(Vec A)`. A multi-constructor `:heap` sum falls
through to `return "int64_t"`. Every comparison therefore says "no ABI
change", and no clone is minted.

**The base body calls a constructor that is never defined.** A `:heap`
parametric ADT gets only per-spec constructors (`ctor_Fr_Frame__int`). A
suffix-less constructor call in a base body is registered as a dead-base
trap (`src/compiler/emit_expr.c:13159`, `emit_note_dead_base_ctor` at
`:13176`), and its stand-in is emitted at `src/compiler/emit_module.c:4731`. The trap's
comment assumes "every live path routes to a spec". This repro is a live
path that does not.

## Fix directions

1. **Treat a `:heap` parametric sum as an ABI change.** In
   `emit_abi_register_call`, a parameter or result whose generic type is a
   `:heap` parametric sum application, and whose instantiation grounds it,
   is an ABI change even though the C name is unchanged. This mirrors the
   G4 `type_phantom_hides_aggregate` rescue next to line 6979, which forces
   a spec for exactly this "same C spelling, different body" case.
2. **Or force a clone from the body.** A body-side check that the callee's
   body (transitively) constructs a `:heap` parametric ADT with a
   suffix-less constructor would make the same decision from the other end.
   This is closer to the trap's own reasoning, but it is a walk.
3. **Pin it.** Make the repro a fixture (`heap-sum-generic-rebuild`), with
   a second function that only reads the sum, so the "no clone needed"
   path stays covered.

## Related

- [heap-sum-unpinned-tyvar-relayed-to-generic](../reported/heap-sum-unpinned-tyvar-relayed-to-generic.md)
  is the shape left open: `(has? (bump (Nil)))`, where nothing in the program
  ever fixes `T`.
- The zipper plan works around this by never rebuilding a parametric `:heap`
  sum in a generic body: its path frames are `:heap` structs with a nullable
  link.
- [heap-sum-match-binder-of-app-payload-unbridged](../archive/heap-sum-match-binder-of-app-payload-unbridged.md)
  (resolved 2026-10-14) is the same rebuild one level down. There the sum
  sits inside a `:heap` struct, the enclosing generic *is* cloned, and the
  binders are what went wrong.
