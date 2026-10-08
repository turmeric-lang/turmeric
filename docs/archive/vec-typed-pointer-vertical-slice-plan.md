---
title: Vec Typed-Pointer Vertical Slice -- Execution Plan
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: The concrete execution plan for step 2 of the M3 -> M7 sequencing (m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md): convert the Vec primitives from the int64 carrier ABI to the typed-pointer (`Vec__A *`) ABI mandated by the parametric-type ABI matrix, type the Eq[Vec] dispatch path, and drive the ~114 `Vec int` carrier-bridge crossings to 0. Grounded in the actual emitted C as of 2026-06-15.
---

# Vec Typed-Pointer Vertical Slice -- Execution Plan

This is **step 2** of the by-value-direction sequencing locked in
[docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../../reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
("Update 2026-06-15 ... Sequencing"), using the ABI classification from
[docs/parametric-type-abi-matrix.md](../../parametric-type-abi-matrix.md).
It is the proving ground for the typed-pointer ABI before Map/Set/MutableMap
(step 3) and the M4 dict-ABI work (step 4).

Prereqs landed: M2b (`#{Construct}` / make-struct), M4c Path A (per-instance
specs), M4-rest (direct dict dispatch), M5 (residual-straddle retirement, both
`CK_CONCRETE -> CK_CARRIER` producer bridges deleted), `Eq [Cons]` carrier
rewrite (#369). Suite green at 1636-1637/0 on the branch head.

## The key representational insight (grounds the whole slice)

Inspecting the emitted C for `vec-eq-ascribed` (`tur -Xdata-literals emit-c`),
the Vec header is:

```c
typedef struct Vec__int {
    void *  data;     /* element buffer; elements stored as int64_t carriers */
    int64_t len;
    int64_t cap;
} Vec__int;
```

Two facts that make this slice tractable:

1. **The element buffer stays int64-carried in ALL element types.** `data` is
   `void *` (an `int64_t *` buffer) regardless of `A`. `Vec__int`,
   `Vec__cstr`, `Vec__bool` differ only in the *type label on the header
   pointer* -- the runtime memory is byte-identical. So "typed pointer" types
   the **header**, not the elements. The element-level carrier (int64 slot in
   `data[i]`) is orthogonal and is NOT what this slice removes.

2. **The producer inline-C bodies are identical across element types except
   for the struct type *name*.** `vec-new`'s body mallocs a
   `{void* data; int64_t len, cap;}`; `vec-push!` reallocs `data`. None of
   that logic varies with `A` -- only `sizeof(Vec__A)` and the cast target
   `(Vec__A *)` differ, and those are the same 24-byte layout for every `A`.

Together these mean inline-C-body monomorphization for Vec is **not** a
general C-rewriting problem (the M5 docs' worry). It is the narrow problem of
*substituting the concrete header struct type name* into a fixed body
template, which the make-struct / heap-box mechanism (below) handles without
any inline-C string surgery.

## Current state (what the emitted C shows today)

- `Vec [A]` lowers to a **by-value** `Vec__A` struct in monomorphic positions.
  The `Eq [Vec]` spec is `bool __inst_Eq_eq_qu_Vec__spec__..(Vec__int, Vec__int)`
  -- the 24-byte header is passed **by value** (copied).
- Producers/accessors (`vec_new`, `vec_hylen`, `vec_hyget`, `vec_push`) keep
  the **int64 carrier** signature (`int64_t (int64_t, ...)`).
- The dispatch boundary bridges between them: every Eq[Vec] call site emits
  `(*(Vec__int *)(intptr_t)(handle))` -- a `CK_CARRIER -> CK_CONCRETE` deref +
  header copy. These are the **~114 `Vec int` crossings** the M3 audit counts.

### Two latent problems with the by-value status quo

- **Mutation hazard (matrix roadblock 1).** A by-value `Vec__int` copies the
  header, so a `vec-push!` on the copy would realloc/bump `len` on the copy and
  be invisible to the caller. Eq is read-only so it is *correct today by luck*,
  but the moment a by-value Vec reaches a mutator it is a silent miscompile.
  This is a CLAUDE.md "works by luck" finding, and the typed-pointer ABI is the
  fix: `Vec__A *` shares the header, so mutation is visible.
- **The bridge deref-copy is pure overhead.** `*(Vec__int *)(intptr_t)(h)`
  copies 24 bytes at every dispatch arg. A typed pointer needs at most a cast.

## Target ABI

Per the matrix: `(Vec A)` lowers to **`Vec__A *`** (typed pointer to the
heap-allocated header) in every monomorphic position. The int64 carrier
survives **only** in type-erased positions (`@Any`, existential,
heterogeneous HAMT, `tur_poly_fn_t`), with a single `(Vec__A *)(intptr_t)h`
cast (no deref/copy) at the pack/open boundary.

After the slice:
- `vec-new : (Vec A)` returns `Vec__A *` (a fresh `malloc`'d header).
- `vec-push! / vec-get / vec-len / vec-set! / vec-pop! / vec-free` take
  `Vec__A *` and `p->field` / `p->data[i]` directly -- no cast.
- `Eq [Vec]`'s dict slot and spec take `Vec__A *` (no by-value copy, no bridge).
- The ~114 `Vec int` `carrier->concrete` crossings drop to 0; the remaining
  crossings are the documented type-erased boundary.

## Mechanism: make-struct boxes directly, no inline-C string surgery

> **Superseded note (2026-06-15):** the design below was written around a
> separate `heap-box [A] [v : A] : ptr<A>` primitive. Execution (sub-step 2)
> replaced it with **make-struct doing the heap-box itself** -- see the sub-step
> 2 entry under "Sub-steps". The `heap-box` primitive and C-2's inline-C
> `A`-substitution are NOT needed. The section is kept for the design rationale
> (the value-vs-pointer split, the element-buffer-stays-int64 insight); read it
> with that substitution in mind.

The producers become pure-Turmeric over a **single** generic heap primitive,
so per-element monomorphization rides the existing make-struct / ABI-spec
machinery rather than rewriting inline-C bodies.

### One new primitive: `heap-box`

```turmeric
;;; heap-box -- move a by-value struct onto the heap, returning a typed pointer.
(defn heap-box [A] [v : A] : ptr<A>
  ```c
  A *p = (A *)malloc(sizeof(A));   /* `A` is substituted to the concrete C name per spec */
  *p = v;
  return (int64_t)(intptr_t)p;
  ```)
```

`heap-box` is the *only* inline-C body that needs the concrete element/struct
name substituted. It is the canonical inline-C-body monomorphization case:
a `defn [A]` whose body references `A` as a C type. Implement the substitution
narrowly (see "Compiler work" below) -- it generalizes to exactly the small set
of "allocate/copy a `T`" primitives, not arbitrary C.

> Decision point D-1: `ptr<A>` vs a dedicated owned-pointer type. `ptr<A>`
> already lowers to `A *`. The simplest path reuses it; if the owned/borrowed
> distinction matters later (Slice borrows, double-free hygiene) introduce an
> `Owned<A>` newtype. **Default: reuse `ptr<A>` for this slice.**

### Producers/accessors rewritten

```turmeric
(defn vec-new [A] [] : (Vec A)
  (heap-box (make-struct Vec :data (null-ptr) :len 0 :cap 0)))

(defn vec-len [A] [v : (Vec A)] : int        (.len v))
(defn vec-get [A] [v : (Vec A) i : int] : A  (:: (vec-data-get-checked__ (.data v) i (.len v)) A))
(defn vec-set! [A] [v : (Vec A) i : int x : A] : void  ...)   ; .data[i] write
(defn vec-push! [A] [v : (Vec A) x : A] : nil  ...)           ; realloc via a raw-buffer helper
```

`(Vec A)` now being `Vec__A *`, `(.len v)` / `(.data v)` are ordinary `p->field`
reads on a pointer -- no ABI-aware deref, no bridge. The realloc in `vec-push!`
stays inline-C but operates on the **raw `data` buffer** (a `ptr<void>` /
`int64_t *`), which is element-agnostic, so its body needs no `A` substitution
(it is a plain `vec-buf-grow__ [data : ptr<void> len cap] : ptr<void>` helper).

This is the crucial split: **header allocation/copy** (needs `A` -> `heap-box`)
vs **element-buffer growth** (element-agnostic int64 buffer -> plain inline-C
helper). Only the former monomorphizes.

## Compiler work

The stdlib rewrite above needs the following emit/elab support. Each item
should land + be pinned before the stdlib flip so the flip is a clean swap.

### C-1: `(Vec A)` lowers to `Vec__A *` (typed pointer) in monomorphic positions

Today TY_APP over a parametric struct lowers to a by-value `Vec__A`
(`types.c` struct-app path; spec params at `emit_fns.c`). For matrix
typed-pointer types, lower to `Vec__A *` instead. Scope the pointer-ABI to a
**per-type flag** (matrix-classified) so by-value-struct types (Option/Result/
Pair/Tuple) are untouched -- this is the lesson of M5 Finding 4 (a blanket
`TY_APP -> pointer` rule regressed 54 fixtures because Option/Result are
genuinely by-value).

- Add a `defstruct` attribute (e.g. `#{Heap}` or a manifest list) marking
  Vec/Map/Set/MutableMap/Cons/GVec as typed-pointer types.
- `emit_type_c_name` / the struct-app lowering emits `Vec__A *` for a
  `#{Heap}`-tagged TY_APP; by-value for the rest.
- `struct_field_c_type` already lowers a value-struct *field* to `T *`; for a
  `#{Heap}` type the field is *already* a pointer, so reconcile the two rules
  (a `#{Heap}` field is `T *`, full stop; no double-pointer).

### C-2: ~~`heap-box` inline-C `A`-name substitution~~ -- DROPPED 2026-06-15

Superseded by the make-struct-boxing design (sub-step 2 above). Because
`make-struct` of a `:heap` type does the `malloc`+copy itself, building the C
from the *known concrete type*, there is no inline-C body that needs an `A`
token rewrite. The only genuinely-inline-C producer left is `vec-push!`'s
realloc, which operates on the element-agnostic raw `data` buffer and so needs
no per-`A` substitution. No inline-C-body monomorphization infrastructure is
required for the Vec slice.

### C-3: carrier boundary keeps a cast, not a deref

At the type-erased boundary (`@Any`, existential, HAMT), a `Vec__A *` packs as
`(int64_t)(intptr_t)p` and opens as `(Vec__A *)(intptr_t)h`. Confirm
`emit_carrier_bridge`'s `CK_CARRIER -> CK_CONCRETE` path, when the concrete
side is a `#{Heap}` type, emits a **cast** (`(Vec__A *)(intptr_t)h`) not the
deref-copy (`*(Vec__A *)(intptr_t)h`). This is where the 114 crossings convert
from copies to (eventually zero) casts.

### C-4: Eq[Vec] dict slot typed `Vec__A *` (rides existing per-instance spec)

The `Eq [Vec]` spec already specializes per element; with C-1 its params become
`Vec__A *` automatically. The remaining int64 dict slot is the M4 dict-ABI item
(step 4) -- out of scope for this slice except to confirm the abstract-dispatch
path still compiles via the carrier (one residual `concrete->carrier`
crossing remains until M4).

## Sub-steps (each independently landable + green)

1. **C-1 behind a flag, no stdlib change. -- LANDED 2026-06-15.** Added the
   `:heap` defstruct annotation + `StructDef.is_heap` (following the
   `:copy`/`:linear` precedent), and the gated `type_c_name` lowering that maps
   a `:heap`-tagged TY_STRUCT/TY_APP to `Name *` / `T__A *`. Tagged nothing yet;
   full suite byte-identical (**1639 passed, 0 failed**, zero snapshot drift),
   proving the flag is the sole gate. Pinned by `tur_heap_abi_unit`
   (`tests/compiler/test_heap_abi.c`): bare name when unset, `Name *` when set,
   no sticky state. NOTE: the marker spelling is the keyword `:heap` (not
   `#{...}`); `#{...}` is the effect-set attribute syntax and is reserved for
   `defn`, so the struct attribute reuses the `:copy`/`:linear` keyword slot.
2. **make-struct heap-boxing + field deref. -- LANDED 2026-06-15 (design pivot:
   no separate `heap-box` primitive).** A generic `heap-box [A] [v : A] : ptr<A>`
   hits a value-vs-pointer conflict: `(make-struct Vec ...)` has type `(Vec A)`,
   which now lowers to the pointer, so make-struct can't yield the by-value
   header heap-box wants. Resolution: make **`make-struct` of a `:heap` type box
   itself** -- build the by-value header literal (`(Vec__int){...}`) with the
   *value* C name, `malloc` + copy, return the typed pointer `Vec__int *`. No
   `heap-box`, no inline-C `A`-substitution (C-2 dropped: the emitter builds the
   C from the known concrete type, never a token-rewrite of an opt-in body).
   Landed: `type_is_heap_struct` / `type_struct_value_c_name` (`types.c/.h`);
   `EX_MAKE_STRUCT` boxing + M2b rt-recovery of the value name; `EX_GET_FIELD`
   `(p)->field` deref for a `:heap` receiver. Pinned by
   `tests/fixtures/heap-make-struct-roundtrip/` (`Box [A]` over int/bool ->
   `Box__int *` / `Box__bool *`, field reads deref), `requires.compiled`.
   All gated on `is_heap`: full suite green, zero snapshot drift. Surfaced (and
   filed) a pre-existing make-struct cstr-into-carrier-field gap that does not
   affect the Vec migration.
2b. **`set!` field-write through a `:heap` pointer. -- LANDED 2026-06-15.**
   Completes the primitive toolkit (construct + read + WRITE). `(set! (.field p)
   v)` on a `:heap` receiver is interior-mutable (no `^mut` required, like rc)
   and lowers to `(p)->field = v` -- so a mutation in a callee is visible to the
   caller (reference semantics, the matrix's correctness requirement; a by-value
   header copy would lose it). `elab_set_field` skips the `^mut` gate for a heap
   receiver; `emit_set_field_stmt` derefs. Gated on `is_heap`; suite green
   (**1640/0**), zero drift. Pinned by extending `heap-make-struct-roundtrip`
   with a `bump-int` callee that mutates the shared header (caller observes 99/8).
   NOTE: `set!` field-write in *polymorphic* context (`set! (.field b) v` where
   the field is a bare tyvar) is a pre-existing elab type-check gap, unrelated to
   heap -- the pin uses a concrete-typed callee. Vec's mutators are concrete per
   spec, so this does not block 3.

   **Primitive toolkit for the typed-pointer ABI is now complete:** construct
   (make-struct boxes), read (`(p)->field`), write (`set!` derefs), mutation
   visible across calls (shared pointer). The remaining work is the stdlib flip.

3. **Tag `Vec` `:heap` + rewrite producers/accessors (the stdlib flip).** Now
   fully expressible in pure Turmeric (no inline-C-body monomorphization):
   - `vec-new [A] [] : (Vec A)` -> `(make-struct Vec :data <null-buf> :len 0 :cap 0)`
     (make-struct boxes to `Vec__A *`).
   - `vec-len`/`vec-get` -> `(.len v)` / raw-buffer read over `(.data v)`
     (the existing `vec-data-get-checked__` element-agnostic helper).
   - `vec-set!`/`vec-pop!`/`vec-push!` -> `set!` on `.len`/`.cap`/`.data` plus
     element-agnostic raw-buffer inline-C helpers (`vec-buf-grow__`,
     `vec-buf-set__`) over `ptr<void>`. The realloc never needs `A`.
   - `vec-of` macro is unchanged (it expands to `vec-new` + `vec-push!`).

   **Not yet started -- two hard gates that must be satisfied in the same change:**
   - **Coordinated snapshot regen.** Tagging Vec `:heap` changes `(Vec A)`'s C
     name everywhere -> a large fraction of the ~1640 `expected.c` regenerate.
     Per the Fixture STRICT RULE this is one coordinated commit; coordinate
     timing with in-flight Vec-touching branches.
   - **Spice (ecs/json) validation.** The plan's validation harness requires the
     `../turmeric-spices/spices/{ecs,json}` roundtrip for any Vec-touching
     change (ecs is the canonical heavy Vec user). **The sibling checkout must be
     present** (`git clone https://github.com/turmeric-lang/turmeric-spices/
     ../turmeric-spices`); it was absent in the session that built the toolkit,
     which is why 3 was deferred rather than rushed.
   - Interpreter parity: all core vec fns have native overrides in `src/main.c`
     (`native_vec_*`), which win over the Turmeric source bodies, so the
     tree-walker is unaffected by the source rewrite -- but re-audit the 10
     existing `requires.compiled` vec fixtures after the flip.
4. **C-3 + re-audit.** Confirm `TUR_M3_AUDIT=1` shows `Vec int`
   `carrier->concrete` crossings at 0 (modulo the M4 dict-slot residual).
   Confirm the by-value mutation hazard is gone (a `vec-push!` through a passed
   `(Vec A)` is now visible to the caller -- add `vec-push-through-param` fixture).
5. **Delete the `*-byval` twins.** `vec-len-byval` / `vec-get-byval` /
   `vec-eq-loop-byval` and the Option C twin-redirect machinery become dead for
   Vec (the receiver is already a typed pointer, no twin needed). Remove them;
   confirm green. (Keep the redirect mechanism itself until Map/Set also flip.)

## Status: LANDED 2026-06-15 (step 3 shipped; the stdlib flip is inline-C, not pure-Turmeric)

The Vec typed-pointer slice is in tree. What shipped diverges from the
sub-step-3 sketch above on the *producer body* question, for a reason the
sketch missed -- recorded here so it is not re-litigated:

- **`Vec` is tagged `:heap`** (`stdlib/vec.tur`), so `(Vec A)` lowers to
  `Vec__A *` in every monomorphic position (spec params, `Eq[Vec]` dispatch).
- **The producers stayed INLINE-C carrier bases**, NOT pure-Turmeric
  make-struct/field-access bodies. Two reasons the pure-Turmeric rewrite is the
  wrong shape here:
  1. **Interpreter parity.** `tur --interpret` routes a call to a
     `native_vec_*` override ONLY when the callee body is inline-C (eval.c's
     "native override for inline-C functions" path). A pure-Turmeric
     `vec-push!` bypasses the native and runs source the tree-walker cannot
     execute (it bottoms out in the raw-buffer inline-C helpers, which have no
     native). Keeping the bodies inline-C preserves interpreter parity for
     free.
  2. **Float element reads.** `vec-get`/`vec-pop!` return the element tyvar
     `A`; a pure-Turmeric `(:: ... A)` body mints an A-monomorphized spec
     (e.g. returning `double` for `A=float`) whose carrier read numerically
     converts the float bits instead of bit-reinterpreting them. Inline-C
     bodies are not return-specialized, so the carrier base returns the raw
     int64 and the call-site ascription does the correct reinterpret.

  The inline-C bodies take/return the int64 carrier, which for a `:heap` value
  IS the header pointer; concrete call sites reach them through the C-3
  reinterpret-cast (below). Mutation is visible to all holders because the
  handle is a shared pointer, never a by-value header copy.

### Compiler support that landed (the actual enabling work)

- **C-1 (already in tree):** `:heap` lowering of `(Vec A)` -> `Vec__A *`.
- **C-3 (`emit_carrier_bridge`, `src/compiler/emit_core.c`):** a `:heap` type
  crosses the carrier boundary as a pure reinterpret CAST
  (`(Vec__int *)(intptr_t)h`), never a struct deref-copy. This is what turns
  the ~114 `Vec int` `carrier->concrete` crossings from 24-byte header
  deref-copies (the segfaulting / mutation-hazard shape) into casts.
- **pass-by-pointer (`type_struct_pass_by_ptr`, `src/compiler/types.c`):** a
  `:heap` type is never pbp-wrapped (it is already a pointer; wrapping made a
  `(Vec A)` parameter a double pointer `Vec__A **` and every field access read
  the wrong memory).
- **ABI-aware field access (`EX_GET_FIELD` in `emit_expr.c`, set-field in
  `emit_stmt.c`):** a `:heap` receiver derefs directly (`(p)->field`) in
  concrete positions but casts through the canonical layout-generic header
  (`((Vec *)(intptr_t)v)->field`) in the abstract polymorphic *base* (e.g. the
  `Eq[Vec]` carrier base, which keeps `vec-len-byval`'s `(.len v)` alive for
  the uniform dict slot).
- **Interpreter element-tag side table (`src/main.c`):** a native vec stores
  raw int64 carrier cells, losing the value tag. The typed `vec-get` return
  makes the read-back ascription a transparent `EX_REINTERPRET` (it no longer
  re-tags). Vecs are homogeneous, so `native_vec_push` records the element tag
  per vec-header pointer and `vec-get`/`vec-pop!` re-tag float/cstr/bool
  carriers. Restores interpreter parity for `tce1-vec-{float,cstr}` etc.

### Result

- Full compiled suite green (`1642 passed, 0 failed`), 75 `expected.c`
  regenerated in the same commit.
- `tur --interpret` suite at baseline (`1201 passed, 2 failed` -- the 2 are
  pre-existing `eq-carrier-capturing-comparator` / `mutmap-eq`).
- Spice roundtrip: `../turmeric-spices/spices/{ecs,json}` -- only pre-existing
  failures (ecs HKT-row `poly-call-row`/`query-typed`/`sized-dense-rt`; json
  `derive-{decode,encode}-struct` Decode/Result carrier). The heavy Vec users
  (spawn1k, sparse-rt, for-each-*) all pass.
- The ~114 `Vec int` `carrier->concrete` crossings are now reinterpret CASTS
  (verified via `TUR_M3_AUDIT=1`); the ~6 residual in `m5-constrained-poly-vec-eq`
  are the abstract-dispatch dict-slot residual that M4 (typed dict slots)
  clears -- out of scope for this slice.

### Still open (steps 4-5, unchanged)

- **M4 dict-ABI:** the `Eq[Vec]` dict slot is still `bool (*)(int64_t,...)`, so
  abstract dispatch through a generic `(defn f [A] [(Eq A)] ...)` keeps the
  residual `concrete->carrier` crossing. Typed dict slots clear it.
- **Step 5 (`*-byval` twin deletion):** `vec-len-byval` / `vec-get-byval` /
  `vec-eq-loop-byval` and the Option C twin-redirect are still in tree; defer
  their removal until Map/Set also flip (the redirect mechanism is shared).
- **Map/Set/MutableMap typed-pointer migration (step 3 continuation):** same
  inline-C-base + `:heap` recipe applies; their producers are likewise
  native-overridden in the interpreter, so they too stay inline-C.

## Validation harness

- `bash tests/run.sh`: zero new `FAIL`; snapshots regenerated in the same
  commit as each stdlib-touching sub-step (steps 3+).
- `bash tests/run-turi.sh`: the interpreter has its own Vec natives
  (`src/main.c`); confirm the typed-pointer producers still resolve to natives
  or carry `requires.compiled` markers where they bottom out in the new
  inline-C. Re-audit the 10 existing `requires.compiled` vec fixtures.
- Per-step `TUR_M3_AUDIT=1` sweep (methodology in the M3 report) tracking the
  `Vec int` crossing count down to 0.
- Spice roundtrip: `../turmeric-spices/spices/ecs` uses Vec heavily; rerun its
  suite after step 3.

## Risks

- **Snapshot blast radius (matrix roadblock 6).** A Vec ABI change touches a
  large fraction of the ~1640 `expected.c`. One coordinated regen; coordinate
  timing with in-flight Vec-touching branches.
- **Interpreter parity.** The tree-walker's Vec natives assume the int64
  carrier handle. Typed-pointer producers must still present an int64-compatible
  handle to the interpreter (a `Vec__A *` *is* a pointer, so `(int64_t)(intptr_t)p`
  round-trips) -- verify `native_vec_*` in `src/main.c` are unaffected (they cast
  the handle to the same anonymous struct).
- **Mutation semantics regression.** Step 4's `vec-push-through-param` fixture is
  the guard; without it the by-value->pointer flip could pass tests while a
  subtle aliasing case breaks. Add it *before* the flip.
- **Double-free / ownership.** `vec-free` on a typed pointer is unchanged
  (frees `p->data` then `p`), but the typed pointer makes accidental aliasing
  easier to write. D-1 (owned-pointer newtype) is the escalation if this bites.

## Out of scope (later steps)

- Map/Set/MutableMap typed-pointer migration (step 3) -- replicate this slice.
- M4 dict-ABI (typed dict slots / per-(instance,element) dict) -- step 4; until
  it lands, abstract `(defn f [A] [(Eq A)] ...)` dispatch through the dict keeps
  one `concrete->carrier` crossing.
- Deleting `emit_carrier_bridge` wholesale -- never (matrix roadblock 4); it is
  re-scoped to the type-erased boundary in step 5 of the M3 sequencing.

## North star

`(vec-of 1 2 3)` produces a `Vec__int *`; `(vec-push! v 4)` mutates it in place
visibly to all holders; `(.eq? a b)` passes two `Vec__int *` to a typed
instance method with no carrier round-trip. The int64 carrier appears only when
the vec is stuffed into a `@Any` or an existential -- exactly the cases the
plan keeps it for.
