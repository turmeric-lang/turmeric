---
title: Parametric Type ABI Matrix
category: Planning -- ABI / Codegen, end-to-end monomorphization
description: The per-type ABI classification that gates the by-value direction of M3/M7. Each parameterized stdlib type is classified as by-value-struct, typed-pointer, or type-erased-carrier, with the mutability/representation rationale. Step 1 of the M3 -> M7 sequencing in docs/archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md.
---

# Parametric Type ABI Matrix

## Why this exists

The by-value direction (chosen 2026-06-15; see the M3 report's "Update
2026-06-15") replaces the int64 carrier with each value's natural monomorphic
C layout so the `CK_CARRIER -> CK_CONCRETE` bridge has nothing to deref. But
"by-value" is **not one ABI** -- it splits by whether the type is an
immutable fixed-size value or a heap-backed/identity-bearing structure:

- A literal **by-value struct** is correct only for immutable, fixed-size
  values. Passing a mutable or heap-linked value by value copies its header
  and silently drops in-place mutations / shares the wrong thing.
- A **typed pointer** (`Vec__int *`, not `int64_t`) is the monomorphic ABI for
  everything heap-backed. It is still fully clang-checked (so it kills the
  bridge) while preserving identity and mutation.

This matrix is the **load-bearing decision** for every subsequent step: it says
which C ABI each parameterized stdlib type gets. Lock it before touching stdlib
or codegen -- getting a mutable type into the by-value-struct column is a silent
miscompile, not a perf nit.

## The three ABI classes

| Class | C representation | When | Bridge behaviour |
|---|---|---|---|
| **by-value struct** | `T__A` passed/returned by value (large ones spill to `const T__A *` via the existing Phase-D pass-by-ptr rule) | immutable, fixed-size, fields-by-value, no shared heap-linked structure | none in monomorphic code -- the value *is* the struct |
| **typed pointer** | `T__A *` (pointer to the heap structure / header) | mutable, OR immutable-but-heap-linked (a node graph you must not deep-copy) | none in monomorphic code -- the value *is* the typed pointer; deref is an ordinary `p->field` |
| **type-erased carrier** | `int64_t` (today's carrier) | existential / `@Any` / heterogeneous-HAMT / `tur_poly_fn_t`, and opaque runtime handles with no monomorphized C struct | carrier stays; `emit_carrier_bridge` survives **here only**, at the pack/open boundary |

A single type can appear in its natural column **and** be carried as int64 in a
type-erased context (e.g. an `Option` inside a `@Any`). The matrix below gives
each type's **natural owned ABI**; the carrier is the universal fallback in
type-erased positions until the M4 dict-ABI + existential-boundary work lands.

## The matrix

### By-value struct (immutable, fixed-size value types)

| Type | Def | Rationale |
|---|---|---|
| `Option [A]` | `(is-some :bool) (value A)` | immutable tagged value; fields by value |
| `Result [A B]` | `(is-ok :bool) (ok-val A) (err-val B)` | immutable tagged value |
| `Pair [A B]` | `(fst A) (snd B)` | immutable 2-tuple |
| `Tuple2 .. Tuple8` | `(e1 A) ...` | immutable N-tuples (Phase-D spills the big ones to `const *`) |
| `Either [L R]` | `:copy (Left L) (Right R)` | immutable sum, `:copy` |
| `Slice [A]` | `(ptr :ptr<void>) (len :int)` | **non-owning** view; copying `{ptr,len}` is a borrow, not a deep copy (like a fat pointer) |
| `Equal [a b]` | GADT `Refl` | zero-size type-equality witness |

These already lower to by-value structs in monomorphic code. Their residual
bridge crossings (`Option Device`, `Result int cstr`) are **not** a per-type-rep
problem -- they fire at the *type-erased dispatch* boundary because the typeclass
dict slot is still `int64_t (*)(...)`. That is the **M4 dict-ABI** item, not a
change to these types' layout.

### Typed pointer (heap-backed: mutable or immutable-but-linked)

| Type | Def | Why pointer, not by-value | Current rep / cleanup |
|---|---|---|---|
| `Vec [A]` | `(data :ptr<void>) (len :int) (cap :int)` | **mutable** -- `vec-push!` reallocs `data`, bumps `len`/`cap`; a by-value copy would drop the mutation | **DONE** (#377): `:heap`-tagged, `(Vec A)` -> `Vec__A *` |
| `MutableMap [K V]` | `(storage :ptr<void>)` | **mutable** hashtable, in-place set/delete -- `mutmap-set!` reallocs `storage` and writes back `m->storage`, so a by-value header copy goes stale (a "works by luck" hazard while only read-only dispatch touched it) | **DONE**: `:heap`-tagged, `(MutableMap K V)` -> `MutableMap__K__V *`. The multi-param `K=TY_STRUCT` elab gap is orthogonal (M4 dict-ABI) |
| `Map [K V]` | `(carrier :int)` | immutable/persistent **but** a HAMT node tree -- pass a pointer to the root, never deep-copy | **DONE**: `:heap`-tagged, `(Map K V)` -> `Map__K__V *`. The `(carrier :int)` field is internal-only (never accessed structurally; the C struct is really `{void* hamt}`), so it does not need typing for the `:heap` flip -- the handle is now a typed pointer regardless |
| `Set [A]` | `(hamt :ptr<void>)` | immutable/persistent, HAMT-backed (same as Map) | **DONE**: `:heap`-tagged, `(Set A)` -> `Set__A *` |
| `Cons [A]` | `(head A) (tail :int)` | immutable/persistent **but** a linked node chain -- a `(Cons A)` value is a pointer to the head cell | **DONE**: `:heap`-tagged, `(Cons A)` -> `Cons__A *` (the handle is now a typed pointer; #369's carrier-based `Eq [Cons]` keeps working because `(:: x :int)` relabels the pointer to the int64 carrier). The `tail :int` field stays an internal next-cell link (like Map's `carrier :int`); fully typing it to `(Cons A)` is a further No-Lazy-`:int` step, not required for the `:heap` flip |
| `GVec [a]` | `defdata` `GVNil`/`GVCons int (GVec int)`, both `: (GVec int)` | phantom-typed linked list (niche/demo) | **PENDING** -- `GVec` is a `defdata` sum (it was a `defgadt` until adt-gadt-unification slice 4; tagged-union lowering), not a `defstruct`, so the `defstruct` `:heap` typed-pointer flip has not been applied to it. Its annotations pin `a` to `int`, so it is not refined: an applied `(GVec int)` gets a by-value monomorph, while the bare `GVec` stays on the uniform carrier. Deferred (niche/demo, no bridge crossings) |

Two distinct rationales land in this column: **mutable** (Vec, MutableMap --
must share so mutation is visible) and **immutable-but-heap-linked** (Cons, Map,
Set, GVec -- by-value would be *safe* but means deep-copying a node graph, so we
pass a pointer to the root). Both get a typed pointer.

**Migration status (2026-06-15):** all `defstruct`-backed heap collections are
migrated -- `Vec` (#377), then `Set` / `MutableMap` / `Map` / `Cons` (this
session). Each is a one-line `:heap` `defstruct` flip riding the Vec slice's
compiler support, validated at suite 1646/0 + interpreter gate + ecs/json/frame
spice roundtrip with zero snapshot drift. Only `GVec` (a `defdata` sum, not a
`defstruct`) remains, and it is deferred. The remaining ~48 carrier-bridge crossings are
the **M4 dict-ABI** item (typed dict slots), not further `:heap` flips.

### Type-erased carrier (stays int64 / opaque; not in the bridge's monomorphic path)

These are `defopaque`/HKT handles whose pointee is **opaque runtime state with
no monomorphized C struct** to deref to, so they never produce a
`CK_CARRIER -> CK_CONCRETE` crossing in the first place. They stay as-is.

| Type | Def | Note |
|---|---|---|
| `Backtrack [A]` | `defopaque :int` | computation handle |
| `Kleisli [A B]` | `defopaque :int` | arrow handle |
| `Goal [A]` | `defopaque :ptr<void>` | logic goal tree handle |
| `Parser [A]` | `defopaque :ptr<void>` | parser-combinator handle |
| `Schema [A]` | `(raw :int)` | schema handle |
| `NonEmpty [A]` | `defopaque :int` | refinement newtype over a collection handle |
| `SizedBuf [n]` | `defopaque :int` | sized-buffer handle |
| `Promise` / `Future` / `FutureHandle` | `defopaque :ptr<void>` | async handles |
| `Fix [^f]` / `Free [^f a]` | `defdata ... :int` | HKT fixpoints -- carrier stays until the M6/M7 HKT pass |

Non-parametric handle structs (`MutexGuard`, `FileHandle`, `Socket`,
`RateLimitOpts`, `Ref`) are out of scope -- they are not generic and do not
cross the bridge.

## The element buffer stays int64 -- the typed-pointer decision is HEADER-only (frontier)

The typed-pointer column types a heap collection's **header** (`Vec__A *`,
`Set__A *`, ...), **not its element storage.** A `Vec__int`, `Vec__cstr`, and
`Vec__bool` differ only in the type label on the 24-byte header; the backing
`data` buffer is `int64_t[]` in **all three** -- every element is stored as an
int64 carrier regardless of `A`. This is deliberate (locked here):

- the interpreter's `native_vec_*` walk the same int64 buffer, so a typed
  element buffer would fork compiled vs interpreted storage and break parity;
- `float`/`cstr` elements are reinterpret-read out of the int64 slot
  (`(int64_t)vec->data[i]` is a *bit* reinterpret, not a numeric conversion);
  a typed `double[]`/`const char *[]` buffer would change those reads.

**Consequence (the permanent residual).** `vec-get` returns an element as
**int64**. When the element type is itself a `:heap` collection (`Vec[Vec[int]]`,
`Set[Vec[int]]`, ...), the structural-eq comparator must reinterpret that int64
back to the typed pointer -- `(Vec__int *)(intptr_t)(elem)` -- to dispatch the
inner typed `Eq` spec. These are the **22 `Vec int` comparator-thunk crossings**
that dominate the post-#400 M3 audit floor. They are cheap reinterpret casts
(Vec is `:heap`, so the int64 *is* the pointer), correct and free at `-O2`, and
they **cannot be removed by typed dict slots or a typed comparator parameter** --
a typed comparator only relocates the cast to the `vec-get` call site, because
the *source* (`vec_hyget` over the int64 buffer) is int64. Eliminating them
requires **element-buffer monomorphization** (`vec-get` returning `Vec__int *`
per element type, a typed `data` buffer), which this matrix **rules out** for
the parity/float reasons above.

**Disposition: accept them as the permanent type-erased boundary** (M4d Phase
2b; M3 report "Update 2026-06-17"). The bridge is down-scoped *to* this boundary,
not deleted from it. Revisiting element-buffer monomorphization is a separate,
deep frontier (it interacts with interpreter parity and float-element storage)
and is **not** scheduled under the current monomorphization plan; it would need
its own design pass alongside, or after, the M6/M7 HKT work.

## Consequences for sequencing

1. **Mutable vs linked is the only fork that matters for correctness.** Get
   Vec/MutableMap into the *typed-pointer* column (never by-value struct), or a
   `vec-push!` on a copy silently miscompiles. Map/Set/Cons are typed-pointer for
   representation (node graph), not mutation -- the result is the same ABI.
2. **By-value-struct types are already done at the layout level.** Their bridge
   crossings are the M4 dict-ABI item (typed dict slots), not a layout change.
3. **The carrier never fully dies.** It remains the ABI for the type-erased
   column and for *any* type in an existential/`@Any`/heterogeneous-HAMT/
   `tur_poly_fn_t` position. `emit_carrier_bridge` is therefore re-scoped to
   that boundary, not deleted outright (M3 report roadblock 4).
4. **The `:int` stand-ins fall out for free.** `Map.carrier :int` and
   `Cons.tail :int` are No-Lazy-`:int` defects; the typed-pointer migration types
   them (`*-root` / `Cons__A *`) as a side effect.

## Validation hook

After each type moves to its matrix ABI, re-run the suite-wide
`TUR_M3_AUDIT=1` per-fixture sweep (methodology in the M3 report). Expect the
type's `carrier->concrete` crossings to drop to 0 while the existential/`@Any`
crossings remain. Suite must stay green; regen the affected `expected.c`
snapshots in the same change.

## Related

- [docs/archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md](../archive/history/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md)
  -- the M3 tracking report; this matrix is its sequencing step 1.
- [docs/archive/history/end-to-end-monomorphization-plan-2.md](archive/end-to-end-monomorphization-plan-2.md)
  -- §M2/§M4/§M7, the plan this refines.
