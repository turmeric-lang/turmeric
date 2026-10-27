# Typed `Map[K V]` Surface -- key-type safety + one accessor (TMS0--TMS6)

> **Status:** TMS0--TMS5 landed (branch `claude/typed-map-surface-plan-JJPdC`).
> `Map[K V]` is a real type at the API boundary: `(map-get m k)` works for any
> `Hash`/`MapKey` key type, the checker rejects key/value mismatches, and there
> is one `map-assoc`/`map-get`/`map-has?`/`map-dissoc` -- `smap-*` removed.
> TMS6 (generalize to other containers) is deferred.
>
> **What actually shipped vs. the original design (verified during execution):**
> - **TMS0 (parameterized-struct types in signatures):** already supported by
>   the landed PTC4 machinery -- `(Map K V)` in param position and `: (Map K V)`
>   in return position type-check and reject mismatches (`TUR-E0001`). The one
>   needed change was making `Map` a **transparent int newtype** (a parametric
>   struct with a single `:int` field, `(defstruct Map [K V] (carrier :int))`)
>   so `(Map K V)` is emitted as `int64_t` in every C signature -- pure-Turmeric
>   and inline-C alike -- keeping the runtime/ABI identical (Option 1).
> - **TMS1 (inference):** explicit annotation is required for the nullary
>   `(map-new)` (option a) -- `(:: (map-new) (Map K V))`, or a bare `(map-new)`
>   tail whose K/V are pinned by the enclosing return type. Forward/first-use
>   inference is *not* available (the checker resolves each generic call's
>   tyvars first-arg-wins, with no cross-call unification). Literals seed from
>   a value-pinned `map-empty-for`.
> - **TMS3 accessors are MACROS, not functions.** A typed *function* accessor
>   would ABI-specialize its value parameter to V's concrete C type and clash
>   with the int64-carrier runtime bridge -- so `:float` values truncated and
>   `Vec`/struct values were a hard C error. The accessors expand at the use
>   site: the value flows straight into the inline-C carrier (any type rides the
>   int64 word), while a borrow-checked `tur-map-kcheck` still enforces the key
>   type K (`TUR-E0001` on a wrong-typed key). Cost: the accessors are macros
>   (no first-class/higher-order use); the key is evaluated once via a let.
>
> **Type:** stdlib (`stdlib/map.tur` rewrite) + small compiler lowering change
> (`#map{...}` -> `hamt-of` for all key types) + migration of fixtures/docs.
>
> **One-line goal (met):** make `Map[K V]` a *real* type at the API boundary, so
> `(map-get m k)` works for any `Hash`/`MapKey` key type, the type checker
> rejects key/value mismatches, and there is exactly one `map-assoc`/`map-get`/
> `map-has?`/`map-dissoc` -- no `smap-*` cousins to remember.

## The problem (verified 2026-05-31)

The container layer is **type-erased to `:int`**. The parameterized struct
exists but is decorative:

```turmeric
(defstruct Map [K V] (hamt :ptr<void>))                    ; stdlib/map.tur:14 -- has K, V
(defn map-new []            :int  ...)                     ; ...but every op is :int
(defn map-assoc [K V] [m :int key :int val :V] :int ...)   ; key is :int!
(defn map-get   [V]   [m :int key :int dflt :V] :V  ...)
```

Three concrete symptoms fall out of this:

1. **No compile-time key-type safety.** The handle is `:int` and remembers no
   `K`. Even after GHE makes a generic `map-get-g [K V] [m :int key :K ...]`,
   nothing stops `(map-get-g a-string-map 5 0)` -- the map is `:int`, so `K`
   unifies to `int` and it compiles, then silently misses at runtime. *This is
   the edge Approach A alone does not smooth.*

2. **Accessor discipline.** A content-keyed (string) map built by `#map{"a" 1}`
   must be read with `smap-get`/`smap-has?`/`smap-dissoc`; an int map with
   `map-get`. Choosing the wrong family is not a type error -- it just misses.
   The caller carries the key-semantics in their head.

3. **Non-`int`/`cstr` keys have no uniform surface.** `bool`, `float32`, and
   user `Hash`/`Eq` key types can be inserted only via the low-level
   `map-assoc-eq` (explicit hash + eq closure); there is no typed front door,
   and the `#map{...}` reader rejects them outright (`TUR-E0282`).

This is **not** Map-specific. The same `:int`-handle pattern is used by `Vec`,
`Set`, `Option`, `Result`, `Pair` -- none declares a parameterized type
(`:Vec A`, `:Option a`) in any signature; all thread element types only at
value slots. So "typed `Map[K V]`" is really the first instance of *typed
collection handles*; this plan pilots the mechanism on `Map` (where the payoff
-- key safety + accessor unification -- is highest) and leaves a generalization
hook for the rest.

## Dependency on GHE (Approach A)

This plan presumes the GHE builder exists: a single `Hash[K]` + `Eq[K]`-driven
`map-assoc`/`map-get` that dispatches correctly for any scalar `K`, with the
`int` path staying zero-overhead. Without it, a unified typed `map-get` would
still need per-key-type bodies. TMS supplies the *type* `Map[K V]`; GHE supplies
the *dispatch* it constrains. They can be staged: GHE first (uniform dispatch,
still `:int` handles), then TMS (nominal handles + safety). TMS0--TMS1 (language
support) can proceed in parallel with GHE.

## Design options for the handle

- **Option 1 -- phantom/newtype handle (preferred).** Give `Map[K V]` a nominal
  type that is *representationally* the existing `:int` (a parameterized newtype
  over the heap-pointer-as-int). Zero runtime/ABI change -- the same
  `struct { void *hamt; }*` cast to `int64_t` -- but the type checker now tracks
  `K`/`V` and rejects mismatches. The `(defstruct Map [K V] ...)` is promoted
  from decorative to load-bearing. This is the smallest change that buys all
  three fixes.

- **Option 2 -- full struct-by-value.** Pass the `Map` struct itself (not a
  pointer-as-int). Larger codegen change, touches the carrier/ABI, more snapshot
  churn; no clear benefit over Option 1 for a single-word handle.

- **Option 3 -- keep `:int`, add lint + docs only.** Cheapest; gives no
  compile-time safety. Reasonable as a pure-pre-v1 stopgap, but does not solve
  symptom (1). Recorded as the fallback if TMS slips.

Option 1 is the plan of record below.

## Phasing

### TMS0 -- parameterized-struct types in signatures

Make a parameterized struct usable as a *declared* type in `defn` params and
returns: `(defn f [m :Map K V] :Map K V ...)`. Today no stdlib signature does
this; confirm the elaborator can (a) parse `:Map K V` (and `:Vec A`, etc.) in
param/return position, (b) unify it during call checking, (c) bind `K`/`V` as
type variables scoped to the `defn`'s `[K V]` list. Land a tiny fixture that
defines and calls such a function on a dummy parameterized struct.

- **Acceptance:** a `defn` taking and returning `:Map K V` type-checks; a call
  passing a `Map[cstr int]` where `Map[int int]` is expected is a type error.

### TMS1 -- inference for nullary/ambiguous constructors

`(map-new)` has no argument to infer `K`/`V` from. Use the **expected-type
channel** (RT1/RT2, "where-clause parsing + expected-type channel", already
landed) to flow the annotation inward:

```turmeric
(let [m (map-new) :Map cstr int] ...)        ; or (:: (map-new) :Map cstr int)
```

Decide the inference story: (a) require an annotation at the binding site when
the type is otherwise unconstrained, with a clear diagnostic; or (b) defer `K`/
`V` until the first `map-assoc` fixes them (first-use inference). Option (a) is
simpler and predictable; pair it with a good "cannot infer key type for
`map-new`; annotate `:Map K V`" error.

- **Acceptance:** `(map-new)` with an expected-type annotation infers `K`/`V`;
  unannotated and unconstrained `map-new` produces the dedicated diagnostic, not
  a confusing downstream error.

### TMS2 -- thread `Map[K V]` through the public API

Rewrite the map signatures from `:int` to `:Map K V`, keeping the runtime body
identical (Option 1: still a pointer-as-int under the hood). Add the
`#{(Hash K) (Eq K)}` constraints so the GHE builder is selected:

```turmeric
(defn map-new   [K V] #{(Hash K) (Eq K)} [] :Map K V ...)
(defn map-assoc [K V] #{(Hash K) (Eq K)} [m :Map K V key :K val :V] :Map K V ...)
(defn map-get   [K V] #{(Hash K) (Eq K)} [m :Map K V key :K dflt :V] :V ...)
(defn map-has?  [K V] #{(Hash K) (Eq K)} [m :Map K V key :K] :bool ...)
(defn map-dissoc[K V] #{(Hash K) (Eq K)} [m :Map K V key :K] :Map K V ...)
(defn map-merge [K V] #{(Hash K) (Eq K)} [a :Map K V b :Map K V] :Map K V ...)
```

The internal inline-C accessors (`map-hamt`, `map-wrap`) keep their raw `:int`/
`:ptr<void>` view behind `#{Unsafe}`; only the public surface gains types.

- **Acceptance:** existing map fixtures, retyped, pass; a key/value-type mismatch
  (`(map-assoc int-map "k" 1)`) is `TUR-E0001`; the `int` path emits unchanged C
  (zero snapshot churn beyond the GHE-introduced bodies).

### TMS3 -- unify accessors; retire / alias `smap-*`

With TMS2, a single `map-get`/`map-assoc`/`map-has?`/`map-dissoc` works for
`cstr` keys (the GHE content-keyed path is selected by `Hash[cstr]`/`Eq[cstr]`).
Make `smap-*` thin deprecated aliases of the typed ops (or remove them after a
deprecation window). Re-point the `#map{...}` / `hamt-of` lowering (GMK Approach
B / GHE Approach C) at the typed `map-assoc` so the literal yields a
`Map[cstr int]`, not an `:int`.

- **Acceptance:** `#map{"a" 1}` has static type `Map[cstr int]`; `(map-get m "a")`
  type-checks and finds the entry; `smap-get` still works (alias) but is marked
  deprecated; `gmk-map-literal-cstr-key` passes through the typed surface.

### TMS4 -- migrate the codebase

Sweep stdlib, fixtures, examples, tutorials, and docs that treat a map as `:int`
to the `:Map K V` surface. This is the bulk of the work and the main risk
(snapshot churn). Gate on the int-key fast path staying byte-for-byte, and
regenerate `expected.c` snapshots per-fixture **with their `flags`** (never the
flagless loop -- see the GMK postmortem where a flagless regen wiped
`sized-sz6-erasure`).

- **Acceptance:** `bash tests/run.sh` zero `FAIL`; no remaining public `:int`
  map handles in stdlib outside the `#{Unsafe}` internals.

### TMS5 -- docs + snapshot regen

Update `docs/guides/data-literals-guide.md` (map keys are "any `Hash`/`Eq`
scalar"; the map literal has type `Map[K V]`), the map docstrings (typed
signatures), the GHE plan's status, and this plan's table. Regenerate
`docs/api/` + `stdlib/docstrings.tur` and all snapshots.

- **Acceptance:** docs describe one typed map surface; `just docs` clean.

### TMS6 -- (optional) generalize to other containers

If TMS0--TMS1 produced a reusable "parameterized-handle" mechanism, apply it to
`Vec[A]`, `Set[A]`, `Option[a]`, `Result[e a]` for the same key/element-type
safety. Pure follow-on; not a v1 gate. Decide here whether to do it or leave the
hook documented.

## File touchpoints

| Phase | File | Change |
|---|---|---|
| TMS0 | `src/compiler/elab_types.c`, `elab_structs.c`, `types.c` | Parameterized-struct types in param/return position |
| TMS1 | `src/compiler/elab_call.c` / expected-type channel | Inference + `map-new` diagnostic |
| TMS2 | `stdlib/map.tur` | Retype the public API to `:Map K V` + `Hash`/`Eq` constraints |
| TMS3 | `stdlib/map.tur`, `src/compiler/elab_toplevel.c`/`elab_call.c` | Unify accessors; alias `smap-*`; typed literal lowering |
| TMS4 | stdlib/fixtures/examples/tutorials | Migration + snapshot regen |
| TMS5 | `docs/`, snapshots | Docs + regen |
| TMS6 | `stdlib/vec.tur`, `set.tur`, `option.tur`, ... | Optional generalization |

## Risks

- **Migration scale / snapshot churn.** Retyping every map handle is broad. Keep
  Option 1 (newtype over int) so runtime/ABI and the int-key C output are
  unchanged; the diff is type annotations + regenerated preambles, not logic.
- **Inference cliffs (TMS1).** Nullary `map-new` and deeply-nested literals can
  outrun the expected-type channel. Prefer explicit annotation + a precise
  diagnostic over silent `K = int` defaults (which would reintroduce symptom 1).
- **Consistency pressure.** Typing `Map` but not `Vec`/`Set`/`Option` is an
  asymmetry; TMS6 exists so the mechanism is reusable rather than a one-off.
- **Depends on GHE.** If GHE slips, TMS2+ stall (a typed `map-get` over `cstr`
  needs the `Hash`/`Eq` dispatch). TMS0--TMS1 are independent and can land first.
- **Backward source compat.** Promoting `:int` map handles to `:Map K V` is a
  source-visible type change for any user code that named `:int`. Provide the
  `#{Unsafe}` raw accessors and an alias/deprecation window for `smap-*`.

## Non-goals

- Changing the runtime representation (Option 1 keeps the pointer-as-int).
- Aggregate (multi-word struct/ADT) keys -- still bounded by the one-word
  carrier slot (shared non-goal with GMK/GHE).
- Relaxing the `#map{...}` key-literal restriction (`TUR-E0282`) to arbitrary
  computed keys -- the builder (`map-assoc`) is the front door for those.

## Open decisions to record as the work lands

1. TMS1: explicit-annotation (preferred) vs first-use inference for `map-new`.
2. TMS3: deprecate-and-remove `smap-*`, or keep as permanent aliases.
3. TMS6: generalize to all containers now, or ship Map-only and document the hook.
4. Whether to sequence as **GHE then TMS** (recommended) or interleave TMS0--TMS1
   with GHE to shorten the critical path.
