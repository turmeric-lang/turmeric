# Typed Collection Elements -- Polymorphic Vec/Map carriers (TCE0--TCE6)

> **Status:** All phases landed (TCE0-TCE6). `vec-of` / `[...]` over any
> scalar element type; `hamt-of` / `#map{...}` with int keys and any scalar
> *value* type; element homogeneity enforced; reads via
> `(:: (vec-get v i) :T)`. Content-keyed (string) maps land via a runtime
> HAMT equality callback plus the `map-*-eq` / `smap-*` stdlib layer (TCE4).
>
> **Type:** Compiler + stdlib for TCE0-TCE3,TCE5,TCE6 (no new runtime; the
> existing `int64_t` carrier buffer is reused). TCE4 additionally adds a
> runtime HAMT key-equality callback (`tur_hamt_*_eq`).
>
> **Approach:** "Approach B" from the vec/map element-type investigation --
> make the `Vec[A]` / `Map[K V]` *operations* polymorphic over a scalar
> element type and have the compiler bridge each instantiation across the
> existing `int64_t` carrier with a per-instance reinterpret. This is the
> lighter-weight alternative to full layout monomorphization (TS3b).
>
> **Depends on:**
> - [archive/route-b-typed-slots-plan.md](archive/route-b-typed-slots-plan.md)
>   -- TS1 typed thunks, TS2 `EX_REINTERPRET`, TS3 typed containers
>   (Cons/Option/Pair/Result) are all landed and are the template here.
> - [archive/history/typed-slots-gs5-compiler-support-plan.md](archive/history/typed-slots-gs5-compiler-support-plan.md)
>   -- the generic-substrate / scalar-boundary work this plan extends.
>
> **Relationship to TS3b:** `route-b-typed-slots-plan.md` deferred `Vec`
> to a sub-phase TS3b that rewrites all eight inline-C functions to
> `sizeof(A)` / `A*` indexing (a real `double*` buffer). TCE is a smaller
> intermediate: it keeps the single `int64_t` buffer and bridges *scalar*
> element types by bit-reinterpret. TCE unblocks `:cstr`/`:bool`/`:nil`/
> `:float` element checking now; TS3b remains the path for genuinely
> aggregate (multi-word struct/ADT) elements. See "Approach B vs TS3b".
>
> **Last updated:** 2026-05-31

---

## Motivation

`vec-of` / `hamt-of` (and their underlying `vec-push!` / `map-assoc`)
declare their element parameters as a literal `:int`:

```turmeric
(defn vec-push! [v : int val : int] : nil ...)        ; stdlib/vec.tur:85
(defn map-assoc [m : int key : int val : int] : int ...) ; stdlib/map.tur:64
```

The `Vec [A]` (`vec.tur:12`) and `Map [K V]` (`map.tur:14`) type parameters
live only on the *structs*; they are phantom and never threaded into the
operations. The checker compares argument kinds by strict `TypeKind`
equality (`src/compiler/elab_call.c:1738`), so:

```turmeric
(vec-of "x")    ; TUR-E0001: function 'vec-push!' arg 2: expected int, got cstr
(vec-of 1.5)    ; TUR-E0001: function 'vec-push!' arg 2: expected int, got float
```

`[...]` lowers to `vec-of` and `#map{...}` to `hamt-of`, so the
data-literal forms inherit the same restriction. Yet `:cstr`, `:bool`,
`:nil`, and `:float` all fit the carrier: pointers and `:bool`/`:nil` are
already `int64_t`-width, and a `:float` is a `double` (8 bytes = `int64_t`).
The rejection is a missing-polymorphism limitation, not a representational
one.

The sibling containers already solved this. `Cons[A]`, `Option[A]`,
`Pair[A B]`, and `Result[A B]` are parameterized over a real element type
and monomorphize to concrete C layouts (`Cons__float { double head; ... }`,
verified in emitted C). Vec and Map are the documented hold-outs.

---

## Empirical baseline (measured 2026-05-31)

These probes define exactly what works today and what the compiler must
gain. Reproduce with `./build/tur run <file>`.

1. **Concrete-typed wrappers work end to end.** A hand-written
   monomorphic wrapper that reinterprets a known scalar through the
   carrier round-trips losslessly:

   ```turmeric
   (defn vec-push-f! [v : int x : float] : nil (vec-push! v (:: x :int)))
   (defn vec-get-f  [v : int i : int] : float (:: (vec-get v i) :float))
   ;; prints 1.5, 2.25 correctly
   ```

   The same pattern works for `:cstr`. This proves the carrier is
   representationally sufficient -- only the *type surface* is missing.

2. **A bare tyvar into an `:int` param is rejected.**

   ```turmeric
   (defn vpush! [A] [v : int x : A] : nil (vec-push! v x))
   ;; TUR-E0001: function 'vec-push!' arg 2: expected int, got tyvar
   ```

3. **An explicit `::` reinterpret on a tyvar type-checks but
   miscompiles.** With `(vec-push! v (:: x :int))` in a `[A]` body, the
   call passes `tur check`, but the monomorphized specialization drops the
   reinterpret:

   ```c
   static void vpush___spec__void_int64_t_double(int64_t v, double x) {
       vec_push_(v, x);   /* double truncated into int64_t param -- no bitcast */
   }
   ```

   The `EX_REINTERPRET` node produced by `::` on a tyvar does not survive
   specialization once `A` is bound to a concrete scalar, so the value is
   passed by C numeric conversion (truncation for `:float`, pointer-to-int
   warning + crash for `:cstr`). Root cause: `type_size_bytes(TY_TYVAR)`
   returns 0 (`src/compiler/elab_core.c:79`), so `elab_ascribe` produces an
   erased `EX_ASCRIBE` instead of a reinterpret.

4. **An `[A]`-generic *inline-C* function sidesteps the whole problem.**
   When the body is inline-C, the function is **not** monomorphized -- it is
   emitted once on the `int64_t` carrier ABI (inline-C forces an `int64_t`
   signature). The concrete-into-tyvar call boundary then rides the existing
   TS4P1 reinterpret path, and a `:A` result ascribed with `::` rides the
   TS3.3 path. Both directions round-trip `:float` and `:cstr` with **no
   compiler change**:

   ```turmeric
   (defn box-push! [A] [v : int x : A] : nil
     ```c ... vec->data[vec->len++] = x; ```)   ; emitted as (int64_t v, int64_t x)
   (box-push! vf 1.5)
   ;; emits: box_push_(vf, ((union { double s; int64_t d; }){.s = 1.5}).d)
   (:: (box-get vf 0) :float)
   ;; emits: ((union { int64_t s; double d; }){.s = box_get(...)}).d  -- prints 1.5
   ```

**Conclusion (revised 2026-05-31).** Findings (2)/(3) only bite for
*specialized* (non-inline-C) generic wrappers. Finding (4) shows the
inline-C primitives -- which is exactly what `vec.tur` / `map.tur` already
are -- stay on the carrier and bridge correctly today. **No compiler change
is required.** Approach B reduces to a pure-stdlib change: give the existing
inline-C ops `[A]` / `[K V]` binders and let the call-boundary reinterpret
machinery do the rest. TCE0 is therefore a validation phase, not a compiler
phase.

Caveat (measured): a `:A`-returning generic does **not** infer `A` from the
surrounding context -- an unconstrained `(vec-get v i)` defaults `A` to
`:int`. Typed reads need an explicit `(:: (vec-get v i) :float)` or a typed
binding. This preserves int back-compat (existing int callers are
unaffected) at the cost of one ascription on non-int reads.

---

## Goals and non-goals

### Goals

1. `(vec-of e ...)`, `[e ...]`, `(hamt-of k v ...)`, and `#map{k v ...}`
   type-check and run for element types `:int`, `:cstr`, `:bool`, `:nil`,
   and `:float`, with the element type inferred and enforced (all elements
   must unify to one `A`).
2. `vec-get` / `map-get` are declared to return the element type
   (`:A` / `:V`), so a typed read is expressible. Because return-type-param
   inference does not flow from context (measured), an unconstrained read
   defaults `A` to `:int`; non-int reads use `(:: (vec-get v i) :float)` or
   a typed binding.
3. Preserve today's homogeneity guarantee: mixed-type literals stay a
   `TUR-E0001` on the offending element.
4. Keep the change additive -- the existing monomorphic `int64_t` ABI of
   the underlying primitives is untouched; only a polymorphic surface is
   layered on top.

### Non-goals

- Full `A*` / `sizeof(A)` buffer layout (native `double[]` storage). That
  is TS3b; TCE deliberately keeps the `int64_t` buffer and bridges by
  reinterpret.
- Aggregate (multi-word struct / ADT) element types in Vec/Map. A
  16-byte struct does not fit the 8-byte carrier; those need TS3b's real
  per-element layout. TCE restricts polymorphism to *scalar* element types
  (`type_size_bytes(A) <= 8` and a single-word ABI).
- Changing `*args*` or any externally depended-on ABI.
- Removing the explicit-hash `map-assoc-h` / `map-get` primitives; they
  remain the low-level entry points.

---

## Approach B vs TS3b -- when to pick which

| | TCE (Approach B) | TS3b (full monomorphization) |
|---|---|---|
| Buffer layout | one shared `int64_t[]`, reinterpret at edges | per-`A` `A*` buffer (`double[]`, ...) |
| stdlib churn | thin polymorphic surface over existing inline-C | rewrite all 8 `vec.tur` fns + HAMT carrier |
| Scalar elements (`:cstr/:bool/:nil/:float`) | supported | supported |
| Aggregate elements (multi-word structs/ADTs) | not supported | supported |
| `:float` storage | bit-reinterpret through `int64_t` | native `double` slot |
| Compiler work | **none** (see finding 4) | layout specialization for Vec + HAMT |

TCE is the right first step: it closes the user-visible fact as a pure
stdlib change (no compiler work, per empirical finding 4), and it leaves
TS3b as a clean follow-on for aggregate elements without re-doing any of
this work.

---

## Phase plan

Each phase leaves the tree green (`bash tests/run.sh` with zero `FAIL`)
and regenerates fixture snapshots per the CLAUDE.md codegen-snapshot rule.

### TCE0 -- Validate the carrier-generic inline-C pattern (no compiler change)

**Goal.** Confirm that an `[A]`-generic function with an inline-C body
stays on the `int64_t` carrier ABI and bridges scalar element types
correctly in both directions, with no compiler change. (Done -- empirical
finding 4 above.)

**Mechanism.** An inline-C body forces an `int64_t` C signature
(`emit_fns.c:345-350`), so the function is emitted once and not
monomorphized. A concrete scalar argument flowing into the `:A` parameter
rides the TS4P1 concrete-into-tyvar reinterpret (`elab_call.c:1762`); a
`:A` result read with `::` rides the TS3.3 ascribe reinterpret
(`elab_types.c:1770`). Both emit the size-equal `union` bitcast.

**Acceptance (probe).** `tests/fixtures/tce0-carrier-generic-probe/`
-- a self-contained `[A]` inline-C push/get pair exercised at `:float` and
`:cstr`, asserting the emitted C contains the `union` bitcasts and the
run output round-trips `1.5` / a string. No stdlib dependency, so it
documents the technique in isolation.

> This phase has no compiler cost. TCE1+ apply the validated pattern to the
> real `vec.tur` / `map.tur` ops.

### TCE1 -- Polymorphic Vec operation surface

**Goal.** Element-typed `vec-push!` / `vec-get` / `vec-set!` / `vec-pop!`
by adding `[A]` binders to the existing inline-C functions, bodies
unchanged.

**Stdlib changes (`stdlib/vec.tur`).**

- Add an `[A]` binder and retype the element parameter/result of the
  existing inline-C ops; the inline-C body is **unchanged** (it keeps
  writing to the `int64_t` buffer):

  ```turmeric
  (defn vec-push! [A] [v : int val : A] : nil ...)   ; body unchanged
  (defn vec-get  [A] [v : int i : int]  : A   ...)   ; body unchanged
  (defn vec-set! [A] [v : int i : int val : A] : void ...)
  (defn vec-pop! [A] [v : int] : A ...)
  ```

  No `*-raw` split is needed -- the inline-C function *is* the carrier
  primitive, and the `[A]` binder only changes its type surface. `A`
  defaults to `:int` when unconstrained, so existing int callers and
  `vec-get` reads are unaffected; non-int reads use
  `(:: (vec-get v i) :float)`.
- Restrict `A` to scalar element types (see Non-goals). A non-scalar `A`
  (aggregate struct/ADT) would not fit the 8-byte slot; that is the TS3b
  boundary. Document the restriction and cover scalars only.

**Acceptance.** `tests/fixtures/tce1-vec-float/`,
`tests/fixtures/tce1-vec-cstr/`,
`tests/fixtures/tce1-vec-bool/` -- push/get round-trips per type.

### TCE2 -- `vec-of` element inference + homogeneity check

**Goal.** `(vec-of a b c)` infers the unified element type `A` from its
arguments and instantiates the TCE1 ops at `A`; a mixed-type literal is a
`TUR-E0001` on the offending element (today's behaviour, preserved).

**Measured surprise.** The pushes do **not** share `A` automatically: each
`vec-push!` instantiates `A` independently (the vec handle is plain `:int`,
not `(Vec A)`), so `(vec-of 1 "x")` was wrongly *accepted* after TCE1.
Phantom-typed handles do not help either -- the checker treats `[A]` params
as rigid (it rejects `(push (Box ?A) 1)` with "expected tyvar, got int"
rather than unifying `A := int`), so there is no HM inference threading one
`A` through the handle.

**Changes (compiler + stdlib).**

- Compiler (`src/compiler/elab_call.c`, variadic rest path ~1573): a
  polymorphic-tyvar rest `[& xs :A]` names a single type variable, so all
  rest args must unify to one type. Bind `A` to the first rest arg and
  require the rest to `type_eq` it (previously each arg was accepted
  independently). This is the HM-correct reading and the lever TCE2 uses.
- `stdlib/vec.tur`: add a fixed-arity, allocation-free pairwise check
  `tur-vec-homog__ [A] [a :A b :A]` (no-op body) and a `vec-homog-chain__`
  macro that chains it over adjacent elements; `vec-of` emits the chain
  before the pushes. A divergent element fails with `TUR-E0001` at its
  span. (A single variadic check would also work but mallocs a throwaway
  cons-list per literal -- rejected for the construction hot path.)

**Acceptance.** `tests/fixtures/tce2-vec-of-infer/` (homogeneous
float/cstr literals) and `tests/fixtures/errors/tce2-vec-of-heterogeneous/`
asserting `(vec-of 1 "x" 3.14)` reports `TUR-E0001` on the offending
element.

### TCE3 -- Polymorphic Map value surface

**Goal.** `Map[K V]` values of scalar type `V` (`map-assoc` /
`map-get` / `map-dissoc`) without manual reinterpret.

**Stdlib changes (`stdlib/map.tur`).**

- As in TCE1: add `[K V]` binders to the existing inline-C ops, bodies
  unchanged:

  ```turmeric
  (defn map-assoc [K V] [m : int key : K val : V] : int ...)
  (defn map-get   [K V] [m : int h : int key : K] : V ...)
  ```

- Values bridge through the carrier-generic pattern exactly like Vec
  elements; `V` defaults to `:int` for back-compat.

**Keys** are handled in TCE4 (they need hashing/equality, not just
storage). For TCE3, keep keys on the `:int` carrier and the explicit-hash
primitives (`map-assoc-h`, `map-get` with precomputed `h`) unchanged.

**Acceptance.** `tests/fixtures/tce3-map-cstr-val/` (int keys,
cstr/float values).

### TCE4 -- Polymorphic Map keys via Hash[K]/Eq[K] (NOT a stdlib-only change)

**Goal.** Scalar non-int keys (`:cstr` especially) with correct content
semantics: two equal strings at different addresses must be the same key.

**Blocker discovered (2026-05-31).** Unlike values, keys cannot ride the
carrier-generic pattern, because the runtime HAMT compares keys by **word
identity**: `src/runtime/hamt.c` (lines 237, 468, 610) does
`e->hash == hash && e->key == key`. So even if `map-assoc` hashed a string
by content, two distinct `const char*` pointers to equal text would not
match. The current design side-steps this by storing the *hash* as the key
(the `#map{...}` literal "normalizes keyword/string keys to their content
hash"), which conflates hash-colliding keys. True `:cstr` keys therefore
need a real change, not just a `[K]` binder.

**Required changes (larger; spans the runtime).**

- Runtime: extend the HAMT with an **equality callback** (and content hash)
  so `tur_hamt_{set,get,del}` compare keys by `Eq[K]` rather than word
  identity. This threads an `eq`/`hash` function (or a per-map vtable) into
  `hamt.c` and every call site -- the core data-structure change.
- Compiler/stdlib: make `map-assoc` / `map-get` / `hamt-of` `[K]`-generic
  and derive `hash`/`eq` from the key type's `Hash[K]` / `Eq[K]` instances
  (`stdlib/hash.tur`), passing them to the runtime. Inline-C bodies must be
  restructured to call out to the dispatched hash/eq (or wrap the HAMT call
  in a fixed-arity helper).
- Keep the int-key fast path: `K = :int` uses identity hash/eq, so existing
  int-keyed maps and all current snapshots are unaffected.
- Constrain `K` to types with `Hash`/`Eq` instances; a missing instance is
  a typeclass-resolution error.

**What landed.** Rather than thread full `Hash[K]` / `Eq[K]` typeclass
dictionaries into the runtime (which would require a `Hash` typeclass that
does not yet exist plus dictionary-passing into inline-C), TCE4 adds the
missing *primitive* and a focused stdlib layer on top:

- Runtime (`src/runtime/hamt.{c,h}`): the five collision-node key
  comparisons now go through `keys_equal`, which consults a thread-local
  override `g_hamt_key_eq` and falls back to pointer identity when none is
  set. New public entry points `tur_hamt_{set,del,has,get}_eq` install the
  override (saved/restored, so nested map ops compose) for one call. The
  hash is still caller-supplied, so content hashing already worked; the eq
  callback is the genuinely new capability. `eq == NULL` is byte-for-byte
  the old behaviour, so existing int-keyed maps and all snapshots are
  unaffected (the only codegen delta is the new stdlib function bodies).
- Stdlib (`stdlib/map.tur`, `stdlib/hamt.tur`): `map-{assoc,get,has?,dissoc}-eq`
  expose the `[K V]`-generic ops taking an explicit hash and a key-equality
  closure (passed as an int handle, invoked from C via a function-pointer
  cast -- the same mechanism `map-eq?` uses for its value comparator). The
  `smap-*` layer specializes them for `:cstr` keys, filling in `cstr-hash`
  (xxHash64 of contents) and `tur-cstr-key-eq?` (content compare).

**Known limitation / follow-up.** `hamt-of` and the `#map{...}` literal still
use the int-identity path (keyword/string keys are normalized to a content
hash *stored as the key*, with the historical collision caveat). Routing
those literals to the content-keyed path for `:cstr` keys needs key-type
dispatch at the `hamt-of` / `#map{...}` boundary. The `Hash` and `Eq`
typeclasses and their `cstr` instances already exist
(`stdlib/typeclass.tur`, `stdlib/typeclass-eq.tur`), so this is a wiring
problem, planned in
[generic-map-key-dispatch-plan.md](generic-map-key-dispatch-plan.md). Today,
correct string-keyed maps use the explicit `smap-*` API.

**Acceptance.** `tests/fixtures/tce4-map-cstr-key/` -- string keys
round-trip, including two distinct string instances of equal content
resolving to the same entry (update, not insert -- count stays fixed) and a
forced hash-collision chain that keeps distinct keys distinct.

### TCE5 -- Data-literal lowering

**Goal.** `[e ...]` and `#map{k v ...}` ride the typed surface
transparently.

**Changes.**

- `src/compiler/elab_toplevel.c` (~1066-1090) already lowers `[...]` to
  `vec-of` and `#map{...}` to `hamt-of`; no lowering change is needed once
  TCE2/TCE3/TCE4 land. Add fixtures that exercise the literal forms at
  non-int element types under `-Xdata-literals`.

**Acceptance.** `tests/fixtures/tce5-data-literal-cstr/`
(`["a" "b"]` and `#map{:k "v"}`).

### TCE6 -- Docs + snapshot regeneration

- Update the `vec-of` / `hamt-of` docstrings (remove "Carrier type is
  :int" / "Keys are int-valued" caveats; state the scalar-element rule and
  the TS3b boundary for aggregates).
- Note the new rule in
  [docs/guides/data-literals-guide.md](guides/data-literals-guide.md).
- Regenerate `docs/api/` (`just docs`) and all
  `tests/fixtures/*/expected.c` snapshots per the CLAUDE.md codegen rule;
  commit snapshots alongside the change.

---

## File change summary

| Phase | File | Change |
|---|---|---|
| TCE0 | (fixtures only) | Validate carrier-generic inline-C pattern; no compiler change |
| TCE1 | `stdlib/vec.tur` | Add `[A]` binders to existing inline-C ops; bodies unchanged |
| TCE2 | `src/compiler/elab_call.c` | Polymorphic-tyvar variadic rest unifies all args to one type |
| TCE2 | `stdlib/vec.tur` | `vec-of` emits a pairwise homogeneity check before the pushes |
| TCE3 | `stdlib/map.tur` | Add `[K V]` binders to value-side inline-C ops |
| TCE4 | `src/runtime/hamt.{c,h}` | Thread-local key-eq override; `tur_hamt_*_eq` entry points |
| TCE4 | `stdlib/hamt.tur` | `extern-c` decls for the `_eq` variants |
| TCE4 | `stdlib/map.tur` | `map-*-eq` (explicit hash + eq closure) + `smap-*` cstr layer |
| TCE5 | (fixtures only) | Literal-form coverage; lowering already in place |
| TCE6 | docstrings, `docs/api/`, fixtures | Doc + snapshot regen |

---

## Test plan

- New fixtures as direct children of `tests/fixtures/` (named `tceN-*`, so the compiled `run.sh` harness picks them up -- it does not descend into nested fixture dirs) per phase (listed
  above). Each asserts both `tur run` output and, where the boundary
  matters, an emitted-C assertion (presence of the `union` reinterpret,
  absence of a bare truncating store).
- A negative fixture for mixed-element literals (`vec-of 1 "x"`) asserting
  `TUR-E0001` on the right span.
- Full `bash tests/run.sh` green at the end of every phase.
- `tests/fixtures/*/expected.c` snapshots regenerated and committed with
  any phase that shifts call-site emission for existing int-typed fixtures.

---

## Open design decisions

1. **Typed reads ergonomics.** `vec-get` / `map-get` return `:A`/`:V`
   which defaults to `:int` when unconstrained (measured), so non-int
   reads need `(:: (vec-get v i) :float)`. Alternatives if this proves
   annoying: a `vec-get-as` helper taking an explicit type witness, or
   improving return-type-param inference from the consuming context (a
   compiler change, out of scope here).
2. **Scalar restriction enforcement.** Where to reject a non-scalar `A` --
   in `elab_call.c` at the reinterpret-insertion site (uniform, early), vs
   a dedicated diagnostic on the vec/map signatures (clearer message). The
   message should name TS3b as the path for aggregate elements.
3. **`:float` key hashing (TCE4).** Hashing float bits is well-defined but
   `0.0`/`-0.0` and `NaN` need a documented policy; may restrict map keys
   to `:int`/`:cstr`/`:bool` initially and defer float keys.
4. **Interaction with the KB-015 specialization-cache bug** noted in
   `route-b-typed-slots-plan.md` (a spurious `int64<->double` reinterpret
   around a second generic-accessor call). TCE0 should verify it does not
   reintroduce or collide with that cache; add a regression probe if the
   typed vec/map accessors trip it.
