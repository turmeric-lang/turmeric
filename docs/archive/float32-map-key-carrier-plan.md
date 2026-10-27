# Wide Map Key Carrier -- boxed keys that don't fit one word (WKC0--WKC5)

> **Status (update):** Implemented for **inline scalar** wide keys (`:float32`
> and `:float`/float64). The carrier fix landed as a stdlib `MapKey[K]`
> typeclass rather than a runtime heap box: `mk-box` bit-reinterprets the float
> key into the existing one-word `int64_t` carrier (a `memcpy`-equivalent union
> reinterpret, never a numeric conversion), and `mk-cmp` returns a carrier-ABI
> `bool(int64_t,int64_t)` comparator that reinterprets each carrier word back to
> the float before comparing. Because an inline scalar fits the word, **no
> runtime-ownership change (WKC2) was needed** for floats -- there is no heap
> allocation per key. A latent compiler bug was fixed along the way: the
> typeclass instance-name manglers omitted `TY_FLOAT`, so every `[float]`
> instance mis-named to the generic `T` and the dispatcher fell back to the
> `int` carrier representative (this is why a `:float` key silently truncated).
>
> **WKC2 (runtime boxed-key ownership) is now implemented** as the foundation
> for multi-word keys: the HAMT carries an optional refcount-aware boxed-key
> carrier (`tur_hamt_box_key` + `tur_hamt_box_retain/release`) and
> ownership-aware `tur_hamt_*_eq_owned` entry points that install retain/release
> for an operation and stamp the ops onto the resulting map so a later
> `tur_hamt_free` releases its keys. A box shared across structurally-shared
> versions is freed exactly once; one-word keys (NULL ops) are untouched.
> Validated by `tests/test_hamt_owned_keys.c` under ASan/UBSan/LSan (ctest
> `tur_hamt_owned_keys`, gate `tests/run-hamt-owned-keys.sh`) -- leak-clean and
> double-free/use-after-free clean across collisions, updates, deletes, and
> structural sharing. Fixing this surfaced and corrected a latent
> `tur_hamt_del` bug (it double-retained the fresh delete-root, leaking the node
> and its keys on `map-free`; harmless before only because compiled programs
> never free maps). The float path does not use this (inline carriers need no
> box); it exists for the still-pending aggregate-key lowering (WKC3).
>
> **WKC3 (aggregate/struct-key lowering) is now implemented.** A multi-word
> struct/ADT key round-trips through the `-g` builders via the WKC2 boxed
> carrier: a `MapKey[K]` instance for the struct heap-boxes the key bytes with
> `tur_hamt_box_key` (`mk-box`), supplies a carrier-ABI comparator over the two
> box payload pointers (`mk-cmp`), and returns `(mk-owned? _) = 1` so the
> builders route through new ownership-aware `map-*-eq-o` /
> `tur_hamt_*_eq_o(owned)` entry points (the map owns and frees the box). The
> `MapKey` class gained `mk-owned?`; one-word/inline instances return 0 (the
> plain `_eq` runtime path, no allocation). A second instance-dispatch gap was
> fixed for this: `emit_reresolve_method_call` now resolves a `TY_STRUCT`
> receiver to its (sanitized) type-name component, so an aggregate-keyed
> constrained-generic call dispatches to `__inst_<Class>_<method>_<Struct>`
> instead of falling back to the `int` carrier representative (the same failure
> float keys hit before `TY_FLOAT` was added to the suffix manglers).
> Round-trip (assoc/update/get/has/dissoc by content) is proven by
> `wkc3-struct-map-key`; the box ownership it relies on is the WKC2-validated
> path. Note: a struct `mk-box`/comparator still needs a little inline-C
> (`&p`/`sizeof` and the field compare) since generic Turmeric cannot introspect
> `K`'s size; a `derive-map-key` macro to generate that boilerplate is a
> follow-up.
>
> Deviations from the original W2 plan, by design:
> - One-word keys are no longer emitted *byte-for-byte* identically (WKC1's
>   strict zero-diff gate): they now route through `MapKey` dispatch (an
>   inlinable identity box + a named comparator) instead of an inline
>   `(fn [a :K b :K] (eq? a b))`. Behavior and cost are equivalent; the
>   `ghe3-generic-map-key` snapshot was regenerated accordingly.
> - The carrier fix is a stdlib `MapKey[K]` typeclass dispatched per-`K`, rather
>   than a compile-time boxing bridge baked into the `-g` lowering. Inline
>   scalars reinterpret into the one-word carrier (no allocation); only
>   aggregate keys allocate (and are owned via WKC2).
>
> Fixtures: `wkc-wide-map-key` (float32/float64 round-trip, update, dissoc),
> a reinstated `:float32` section in `ghe3-generic-map-key`,
> `wkc3-struct-map-key` (2-field struct key round-trip), and
> `tests/test_hamt_owned_keys.c` for the WKC2 runtime ownership.
>
> ---
>
> **Status (original):** Not started. Spun off from
> [generic-hash-eq-dispatch-plan.md](history/generic-hash-eq-dispatch-plan.md) (GHE3),
> which unified content-key *dispatch* for `int` / `bool` / `cstr` keys but left
> `:float32` keys failing. This plan covers the *runtime carrier*, not dispatch.
>
> **Type:** Runtime (HAMT `*-eq` key/comparator ABI + key ownership) + a
> compile-time boxing bridge in the `-g` lowering. No new typeclass machinery --
> GHE already routes `(hash key)` and the per-`K` comparator correctly; the
> values simply cannot survive the one-word carrier.
>
> **One-line goal:** give the HAMT a *boxed key* representation so a key of
> **any** type -- `float32`, `float64`, and (as a deliberate side effect)
> multi-word struct/ADT keys -- round-trips through `map-assoc-g` / `map-get-g`
> by content, with the common one-word (`int`/`bool`/`cstr`) path unchanged and
> key lifetime owned by the map.
>
> **Chosen approach: W2 (generalized boxed carrier).** The narrower W1
> (bit-reinterpret `float32` into the existing word) is rejected as the primary
> design -- see "Why W2 over W1" -- because it only buys back the keys that
> happen to fit one word and leaves the same wall standing for `float64` and
> aggregate keys. W2 fixes the carrier once, for all of them.

## The bug, precisely

GHE3's `-g` builders thread a key through the runtime content-equality layer:

```turmeric
(defn map-assoc-g [^Hash K ^Eq K V] [m : int key : K val : V] : int
  (map-assoc-eq m (hash key) key val (fn [a : K b : K] : bool (eq? a b))))
```

`map-assoc-eq` (`stdlib/map.tur`) and the runtime it calls
(`tur_hamt_set_eq` in `src/runtime/hamt.c`) carry the key as a **single word**:

- `HamtEntry.key` is `void *key`, "owned by caller; HAMT does not free"
  (`src/runtime/hamt.h:38-43`).
- The override comparator is `typedef bool (*tur_hamt_keyeq_fn)(int64_t, int64_t)`
  (`src/runtime/hamt.h:125`), invoked via `keys_equal` by reinterpreting each
  stored `void *key` to `int64_t` (`src/runtime/hamt.c:248-250`).
- `map-assoc-eq`'s key parameter is typed at the `int64_t` carrier and the
  inline-C casts it `(void *)(intptr_t)key`.

For `int` (small integers), `bool` (0/1), and `cstr` (a genuine pointer) the
value *is* one word, so the round-trip through `void*`/`int64_t` is lossless and
content comparison works. For `:float32` it breaks two independent ways:

1. **Value truncation.** A `float32` argument bound to an `int64_t` carrier
   parameter is *numerically converted*, not bit-reinterpreted: `1.5f` becomes
   `1`. The stored key is already wrong before any comparison.
2. **Comparator ABI mismatch.** The per-`K` comparator GHE2 specializes has C
   signature `bool(float, float)`, but `keys_equal` calls it through
   `bool(int64_t, int64_t)`: integer-class registers where the callee reads
   float-class registers is undefined.

### This is pre-existing, not a GHE regression

The hand-written `*-eq` layer fails identically with no generic dispatch:

```turmeric
(defn f32eq [a : float32 b : float32] : bool ```c return a == b; ```)
(map-get-eq (map-assoc-eq (map-new) (hash 1.5f32) 1.5f32 42 f32eq)
            (hash 1.5f32) 1.5f32 f32eq)        ; => 0, want 42
```

returns `0`. GHE3 correctly *dispatches* `Hash[float32]` and a specialized
`bool(float,float)` comparator; the one-word carrier is what drops the value. So
this is a property of the TCE4 `*-eq` runtime ABI, predating GHE entirely.

## Why a one-word carrier exists today (and why it has to grow)

The HAMT is type-erased: one `Hamt *` holds keys of a single program-level type,
but the runtime is compiled once and cannot know that type. A `void*` key slot
is the natural erasure -- it holds `int`/`bool`/pointer keys directly. The cost
of that simplicity is that *only* one-word keys work, and the comparator ABI
(`bool(int64_t,int64_t)`) silently assumes the key fits a word. Float and
aggregate keys are the keys that don't.

## Why W2 over W1

W1 ("bit-cast `float32` into the existing 64-bit word, add a
`bool(i64,i64)`->`bool(float,float)` shim") is the minimal patch, but it is a
point fix:

- It only rescues keys that fit one word after bit-casting. `float64` *just*
  fits 64 bits but still needs its own bit-cast + shim; a `Point{x y}` or ADT
  key does not fit at all and stays broken.
- It encodes the key type into the carrier convention in an ad hoc, per-type
  way (one shim per scalar width), so every new wide key type is another patch.
- It does nothing for key *lifetime*: the HAMT still does not own keys
  (`src/runtime/hamt.h:40`), which is already a latent footgun for runtime-built
  keys and becomes acute once keys are boxed.

W2 generalizes the carrier *once*. A key that is not a native one-word scalar is
**boxed**: the `void *key` slot holds a pointer to a heap allocation carrying the
key's bytes, the map owns that allocation, and the per-`K` comparator/hash
operate on the boxed form. `float32`, `float64`, and multi-word keys all flow
through the same path; adding a new wide key type needs no new runtime code.

This does cost a small indirection and an allocation per wide key -- paid only by
maps that actually use wide keys. The one-word path is preserved verbatim
(see WKC1), so `int`/`cstr` maps keep today's representation and cost.

> **Scope note vs GHE non-goals.** GHE explicitly listed multi-word keys as a
> non-goal ("the carrier slot is one word"). W2 is precisely the plan that lifts
> that restriction; reaching aggregate keys is an intended consequence of fixing
> float keys the general way, not scope creep. Aggregate-key *ergonomics*
> (deriving `Hash`/`Eq` for a struct) remain a separate concern -- this plan only
> guarantees the carrier can hold them once those instances exist.

## Design: the boxed-key carrier

Representation, chosen at the `-g` lowering boundary (compile time, where `K` is
known) and threaded consistently into the per-`K` hash and comparator:

- **One-word keys** (`int`, `bool`, `cstr`, pointer-shaped opaques): unchanged.
  The key is stored inline in `void *key` exactly as today; no box, no
  allocation, byte-for-byte the current codegen.
- **Wide keys** (`float32`, `float64`, struct/ADT): the lowering boxes the key
  into a fresh heap allocation holding its raw bytes and stores the box pointer
  in `void *key`. The map owns the box (WKC2 lifetime).

The runtime stays *representation-agnostic*: it still stores `void *key` and
still calls a `bool(int64_t,int64_t)` comparator. What changes is that for wide
keys both operands are *box pointers*, and the per-`K` comparator GHE2 already
specializes is the natural place to dereference + compare -- it is compiled with
`K` known, so it knows whether to treat the `int64_t` as an inline value or a
box pointer. Likewise `Hash[K]` already produces a 64-bit hash from the real
key value before boxing, so the hash word is unaffected.

The net effect: **the carrier convention generalizes; the runtime node layout
and the `void*`/`i64` ABI do not have to.** This keeps the change off the hot
node-traversal path and out of every existing `tur_hamt_*` snapshot.

## Phasing

### WKC0 -- lock the boxing contract

Pin down, and record here:

- Which key kinds box (anything not a native one-word scalar) and which stay
  inline. The predicate is on the *compile-time* `K`, evaluated in the `-g`
  lowering.
- The box layout (raw key bytes; the box does not need a type tag because the
  per-`K` comparator/hash already know `K`).
- That `Hash[K]` runs on the unboxed value (unchanged) and the comparator runs
  on box pointers for wide keys.

### WKC1 -- preserve the one-word path verbatim

Before adding boxing, prove the `int`/`bool`/`cstr` lowering is untouched: an
int-keyed and a cstr-keyed `map-assoc-g`/`map-get-g` must emit byte-for-byte the
current C. This is the regression gate for everything that follows.

- **Acceptance:** zero `expected.c` diff for the existing `ghe3-generic-map-key`
  int/bool/cstr assertions.

### WKC2 -- key ownership in the runtime

Boxed keys must outlive every entry that references them, and must be freed when
the map (and all its structural-sharing parents) are gone. Today the HAMT frees
neither keys nor values (`src/runtime/hamt.h:82-84`). Add an opt-in key
destructor / ownership hook so a boxed key is freed exactly once across
structural sharing (refcount-aware, mirroring node `ref_count`).

- **Acceptance (MET):** an ASan/LSan run of a wide-keyed map build+drop is
  leak-clean and double-free-clean (the compiler/codegen path is already
  leak-checked; this extends it to runtime-built boxed keys). Implemented as
  `tur_hamt_box_key`/`box_retain`/`box_release` + `tur_hamt_*_eq_owned` with a
  per-map `key_ops` stamp; the retain fires in `collision_node_copy`, the
  release in the collision free/delete paths and in `tur_hamt_free` (which
  installs the map's release hook for the node cascade). Validated by
  `tests/test_hamt_owned_keys.c` (ctest `tur_hamt_owned_keys`).

### WKC3 -- per-`K` box/unbox in the `-g` lowering + comparator

Teach the `-g` lowering to box a wide key on `map-assoc-g` and to box the probe
key on `map-get-g`/`map-has-g?`/`map-dissoc-g` (transiently) before calling the
`*-eq` runtime. Reuse GHE2's per-`K` fn-value specialization so the comparator
dereferences box pointers and delegates to the `Eq[K]`-resolved `eq?`; the box
form is selected by the same `K` that selects the comparator, so the two always
agree.

- **Acceptance (MET):** the `f32eq`-style repro returns `42` (`wkc-wide-map-key`);
  a `float64` key (`wkc-wide-map-key`) and a two-field struct key given
  `Hash`/`Eq` (and a `MapKey`) instance (`wkc3-struct-map-key`) also round-trip.
  Realized as: `MapKey` gains `mk-owned?`; the `-g` builders call `map-*-eq-o`
  with `(mk-owned? key)`, which select `tur_hamt_*_eq_o(owned)`; an aggregate
  `mk-box` heap-boxes via `tur_hamt_box_key` and `mk-cmp` compares two box
  payload pointers. The struct case also required teaching
  `emit_reresolve_method_call` to dispatch a `TY_STRUCT` receiver by type name.

### WKC4 -- fixture + reinstate the GHE3 float case

Add `float32`/`float64`/struct-key sections (extend `ghe3-generic-map-key` or
add `wkc-wide-map-key`), regenerate snapshots honoring each fixture's `flags`
(never the flagless loop -- see the GMK postmortem that wiped
`sized-sz6-erasure`), and flip the GHE3 acceptance note to mark `:float32` done.

- **Acceptance:** `bash tests/run.sh` zero `FAIL`; GHE3's deferred `{float32}`
  case is reinstated and passing.

### WKC5 -- docs + non-goal revision

Update the GHE plan's non-goals (multi-word keys are no longer excluded once
their `Hash`/`Eq` instances exist), the `map-*-g` docstrings (note boxed wide
keys + ownership), and the data-literals guide.

- **Acceptance:** docs regenerated (`just docs` / `gendocs.py`); no stale
  "one-word key only" wording remains.

## File touchpoints

| Phase | File | Change |
|---|---|---|
| WKC2 | `src/runtime/hamt.c`, `hamt.h` | refcount-aware boxed-key ownership hook |
| WKC3 | `stdlib/map.tur` + `-g` lowering | per-`K` box/unbox; box-aware comparator |
| WKC4 | `tests/fixtures/`, snapshots | wide-key fixture, flag-honoring regen |
| WKC5 | `docs/` | non-goal revision, docstrings, data-literals guide |

## Risks

- **Hot-path / footprint regression on one-word maps.** The entire value of W2
  hinges on the inline path being untouched; WKC1 is the gate. Box only when the
  compile-time `K` is wide; never thread a runtime "is-boxed" flag through node
  traversal.
- **Box lifetime across structural sharing.** A boxed key is shared by every
  persistent version that retains the entry; freeing it on the wrong drop is a
  use-after-free. WKC2 must tie box ownership to the same refcount discipline as
  nodes, and verify under ASan.
- **Comparator/box representation drift.** The boxed layout and the comparator's
  unbox must be chosen by the *same* `K`. Because GHE2 specializes the comparator
  per `K` and the lowering boxes per `K`, they share a single source of truth --
  do not introduce a second, runtime-side notion of the box format.
- **NaN keys.** `NaN != NaN` under `Eq[float*]`, so a NaN key can never be
  looked up. Acceptable (matches `=` on floats); document it, do not special-case.
- **Convert-vs-reinterpret at the boundary.** Even boxed, the float key's bytes
  must be *copied*, not numeric-converted, into the box. The bug being fixed is
  exactly an implicit float->int conversion; keep the boundary a `memcpy` of raw
  bytes.

## Non-goals

- Deriving `Hash`/`Eq` for struct/ADT keys automatically -- W2 makes the carrier
  able to *hold* such keys; producing their instances is separate.
- Changing the HAMT hash word, node layout, or the one-word carrier path.
- Float (or aggregate) *values* -- TCE3 covers scalar values; this plan is keys
  only.
