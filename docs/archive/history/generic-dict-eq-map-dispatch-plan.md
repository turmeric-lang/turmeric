# Generic-Dict `Eq [Map]` Dispatch -- content equality through a polymorphic `^Eq` constraint (GDE0--GDE5)

> **Status:** Done (GDE0-GDE5 completed). Extracted from
> [generic-hash-eq-dispatch-plan.md](archive/generic-hash-eq-dispatch-plan.md),
> "Remaining (large -- the generic-dict path)"). The concrete-receiver
> `(.eq? a b)` path on a `Map[K V]` is already content-correct for `:cstr` and
> struct keys (GHE5, via dispatch-site synthesis). This plan closes the one
> remaining hole: when `Eq [Map]` is reached through a **polymorphic** `^Eq A`
> constraint instead of a concrete `.eq?`, the comparison is still identity-keyed
> (it compares map *pointers* as ints).
>
> **Type:** Compiler (monomorphizer/emit core + typeclass dispatch), with a
> possible small runtime addition (Phase GDE3, option 3a). No new stdlib surface.
>
> **One-line goal:**
>
> ```turmeric
> (defn eq2 [^Eq A] [a :A b :A] :bool (eq? a b))
> ;; mapX, mapY : Map[cstr int] with distinct key pointers but equal text/values
> (eq2 mapX mapY)   ; must be true (content equality), today returns identity
> ```
>
> compiles and dispatches `(eq? a b)` through the constraint to a
> **content-correct** `__inst_Eq_eq__Map`, so a generic `^Eq A` body works on a
> `Map[K V]` argument exactly like a direct `(.eq? a b)` does.

## Background -- why the concrete path works but the generic-dict path does not

GHE5 made `(.eq? a b)` on a *concrete* `Map[K V]` receiver content-correct by
extending the **dispatch-site synthesis** (`try_synth_recursive_eq` in
`src/compiler/elab_typeclasses.c`): at the concrete call site the compiler knows
`K`, so it threads the per-`K` `MapKey` comparator into a `map-eq-k?` helper.

The generic-dict path defeats that synthesis. In

```turmeric
(defn eq2 [^Eq A] [a : A b : A] : bool (eq? a b))
```

the body `(eq? a b)` is elaborated **once**, with `a : A` a *type variable*. The
concrete `Map[cstr int]` type the synthesis needs is not visible until the
instantiation site `(eq2 mapX mapY)`, by which point the body is already
elaborated. So the generic-dict path needs the real
"constrained-generic instance specialization" feature (CGI gap #2 in the parent
plan), which is three coupled changes in the monomorphizer + emit core.

### The three coupled gaps (as observed in the source today)

1. **No instance-driven specialization.** `eq2`'s body bakes
   `__inst_Eq_eq__int` (the `TY_INT` carrier representative the CGI method-call
   fix picks for a tyvar receiver). It is **never** specialized for
   `A = Map[cstr int]`, because that type's C ABI is `int64`, identical to the
   `int` representative, so the `abi_changes` test in `emit_abi_register_call`
   (`src/compiler/emit_module.c`, gate at the `if (!abi_changes)` early-return,
   ~line 433) is false and no specialization is interned. The base clone has no
   `current_abi_specialization`, so `emit_reresolve_method_call` bails
   (`recv->type.kind == TY_TYVAR` still unbound; `emit_core.c:606`) and keeps the
   baked `__inst_Eq_eq__int`. Result: `(eq2 mapX mapY)` compares the two map
   pointers as `int`s.

2. **`TY_APP` is not re-resolvable.** Even with a specialization in hand,
   `emit_inst_suffix_component` (`emit_core.c:560`) returns `NULL` for `TY_APP`,
   and `emit_reresolve_method_call` (`emit_core.c:588`) only mangles scalar kinds
   plus `TY_STRUCT` (the WKC3 branch at `emit_core.c:614`). A `Map[cstr int]`
   receiver resolves to `TY_APP`, so re-resolution cannot name
   `__inst_Eq_eq__Map` -- it falls back to the int carrier representative.

3. **`__inst_Eq_eq__Map`'s body is identity.** Reaching the instance is still not
   enough: the bare-HKT `Eq [Map]` instance body receives an *unapplied* `Map`
   handle and is generic over `K`, so it compares by identity. The dispatch-site
   synthesis cannot fix it (the body is generic), so this needs either a key/value
   comparator **stamped into the map at runtime** (the instance body reads it; no
   compile-time dispatch) or **true bare-HKT element-method dispatch** in the
   instance body (real dictionary passing for HKT instances).

All three must land for the one-line goal to pass; they are sequenced below so
each phase is independently testable against an intermediate failure mode.

## Relationship to existing building blocks (verified)

- **CGI method-call re-resolution** -- `emit_reresolve_method_call` /
  `emit_inst_suffix_component` (`emit_core.c:560-640`) already re-name
  `__inst_<Class>_<method>_<T>` for scalar and `TY_STRUCT` receivers inside a
  spec. Gap #2 extends this to `TY_APP`.
- **ABI specialization core** -- `emit_abi_register_call` /
  `emit_abi_intern_spec` (`emit_module.c:343+`) intern per-binding specs, but
  only when `abi_changes`. Gap #1 adds an instance-driven trigger.
- **Dispatch-site recursive `Eq` synthesis** -- `try_synth_recursive_eq` +
  `map-eq-k?` (`elab_typeclasses.c`) -- the *concrete* path GHE5 fixed; the
  reference behavior the generic-dict path must match.
- **TCE4 content-equality runtime** -- `tur_hamt_*_eq` + the thread-local
  `keys_equal` override. Gap #3 option 3a reuses this primitive (stamp the
  comparator into the map rather than thread it at the dispatch site).
- **`MapKey[K]` carrier** -- the per-`K` key witness (`mk-cmp` /`mk-box`,
  `stdlib/`) used by `map-eq-k?` and the WKC carrier; the comparator the runtime
  stamp (3a) or the instance body (3b) would consult.

## Phasing

### GDE0 -- characterize the failure with a red fixture

Add a fixture that drives `Eq [Map]` through a polymorphic constraint and
asserts content equality, so every later phase has a regression target and the
intermediate failure modes (identity -> wrong instance -> identity body) are
observable.

- New fixture `tests/fixtures/gde-generic-dict-eq-map/` with:
  - `eq2 [^Eq A] [a :A b :A] :bool (eq? a b)` (the generic-dict driver).
  - Two `Map[cstr int]` values built with **distinct** key pointers but equal
    text + values (mirror `eqmap-cstr-content`'s distinct-pointer construction).
  - Asserts `(eq2 mapX mapY)` is `true`, and a negative case (`mapX` vs a map
    with one differing value) is `false`.
- Document the *current* (wrong) output in the fixture's notes / plan: it returns
  identity (pointer compare). Mark the fixture `requires.*` only if it needs the
  sibling spices repo (it should not).
- **Acceptance:** fixture builds and *fails* the equality assertion today,
  confirming the identity-keyed reproduction; `bash tests/run.sh` shows exactly
  this one new `FAIL` (no collateral churn).

### GDE1 -- instance-driven specialization (gap #1)

Force a specialization of a constrained generic when one of its tyvar parameters
is the **receiver of a typeclass-method dispatch** (the body carries a
`dict_arg`-tagged call on that parameter) and the instantiation binds the tyvar
to a type whose resolved instance **differs** from the baked representative
(`TY_INT` carrier), *even when the C ABI is unchanged*.

- In `emit_abi_register_call` (`emit_module.c`), before the `if (!abi_changes)`
  early-return (~line 433): scan the callee `FnDef`'s body for `EX_CALL`s that
  carry a `dict_arg` whose receiver (arg 0, ascriptions stripped) is a `TY_TYVAR`
  matching one of this call's `abi_bindings`. For each, resolve the binding's
  concrete type and compute the `__inst_<Class>_<method>_<component>` name it
  *would* dispatch to (reuse the suffix logic GDE2 generalizes). If that name
  differs from the baked representative's (`__inst_..._int`), set an
  `instance_changes` flag.
- Treat `instance_changes` like `abi_changes`: intern a spec
  (`emit_abi_intern_spec`) and record the specialized call, so the spec gets a
  `current_abi_specialization` binding `A -> Map[cstr int]` and the base clone is
  no longer used for this instantiation. Make the base clone dead for such fns
  (no instantiation should fall back to it once a spec exists).
- Keep the carrier fast path: when the bound type *is* the carrier representative
  (e.g. `A = int`), `instance_changes` stays false and the existing
  `emit_abi_note_carrier_call` path is unchanged (no snapshot churn for int).
- **Risk control:** gate strictly on "tyvar param is a `dict_arg` receiver"; do
  not specialize on every ABI-stable tyvar (that would re-spec unrelated
  generics). Add a unit-style fixture proving an ABI-stable generic with **no**
  method dispatch on its tyvar is still *not* specialized.
- **Acceptance:** the GDE0 fixture now creates a spec for `eq2` at
  `A = Map[cstr int]` (verify via a debug dump or by GDE2's re-resolution
  firing); the body no longer uses the int base clone. Equality may still be
  wrong (gaps #2/#3 remain), but the spec exists. `bash tests/run.sh` shows no
  *new* failures beyond GDE0's.

### GDE2 -- `TY_APP` re-resolution (gap #2)

With a spec in scope, make `emit_reresolve_method_call` name
`__inst_Eq_eq__Map` from a `Map[cstr int]` receiver.

- In `emit_reresolve_method_call` (`emit_core.c:588`), after the existing
  scalar/`TY_STRUCT` branches: when `resolved.kind == TY_APP`, walk the `TY_APP`
  chain to its head constructor, extract the constructor's struct/type name
  (`Map`), and sanitize-mangle it exactly as the `TY_STRUCT` branch does
  (`emit_core.c:614-626`) to produce the `component`. Factor the sanitize loop
  into a small helper shared by the `TY_STRUCT` and new `TY_APP` branches.
- Mirror the same `TY_APP` handling anywhere else the instance suffix is derived
  (the EX_INSTANCE_DEF `type_suffix` switch in `emit_stmt.c` and the dict-name
  switch, per the comments at `emit_core.c:608-612`) so the *definition* name and
  the *call* name agree -- otherwise the re-resolved callee references an
  undefined symbol.
- Optionally extend `emit_inst_suffix_component` to return the constructor name
  for `TY_APP` if a single-component answer is cleaner, but the chain-walk in
  `emit_reresolve_method_call` is the minimum.
- **Acceptance:** the GDE0 fixture's `(eq? a b)` now emits a call to
  `__inst_Eq_eq__Map` (inspect `emit-c` output), not `__inst_Eq_eq__int`. The
  symbol resolves (links) to the actual `Eq [Map]` instance. Equality is now
  whatever that instance body does (still identity -> GDE3). No new `FAIL`s
  beyond GDE0's equality assertion.

### GDE3 -- content-correct `__inst_Eq_eq__Map` body (gap #3)

Make the `Eq [Map]` instance body compare by content. The body is generic over
`K`, so it cannot dispatch `K`'s comparator at compile time the way the
dispatch-site synthesis does. Two options -- **decide here and record the
choice in this doc**:

- **3a -- runtime-stamped comparators (default, lower risk).** Stamp the per-`K`
  key comparator (and the per-`V` value comparator) into the map handle at
  construction time, reusing the TCE4 content-equality plumbing
  (`tur_hamt_*_eq` + the thread-local `keys_equal` override). The
  `__inst_Eq_eq__Map` body then calls a runtime `tur_hamt_eq_dynamic(a, b)` that
  reads each map's stamped comparators and walks entries -- no compile-time
  element dispatch needed. This mirrors how content *lookup* already supplies a
  comparator at runtime. Requires: a small `tur_hamt` field/accessor for the
  stamped comparators, populated by the `-g` builders / `#map{...}` lowering, and
  a `tur_hamt_eq_dynamic` runtime entry.
- **3b -- true bare-HKT element dispatch (general, larger).** Give the
  bare-`Map` instance body a real dictionary it can use to call `(eq? ka kb)` on
  the element type `K` -- i.e. genuine dictionary-passing for HKT instances. This
  generalizes beyond `Eq [Map]` but is a monomorphizer-core feature on its own.

Default to **3a**: it is localized, reuses an existing runtime primitive, and the
two maps already carry (or can be made to carry) their comparators. Record 3b as
the long-term general fix.

- Whichever option: handle the value side too (`Map[K V]` equality compares
  values via `Eq[V]`), and the empty-map / different-size short-circuits.
- **Acceptance:** the GDE0 fixture passes -- `(eq2 mapX mapY)` is `true` for
  distinct-pointer equal `:cstr`-keyed maps and `false` for a differing value.
  Add struct-keyed and `:int`-keyed generic-dict cases (the int case must stay
  correct: hash == carrier so identity already agrees, but it now flows through
  the same path). `bash tests/run.sh` zero new `FAIL`s.

### GDE4 -- generalize the generic-dict path beyond `Eq`/`Map`

Confirm the same machinery (GDE1's instance-driven spec + GDE2's `TY_APP`
re-resolution) is not `Eq`/`Map`-specific.

- Verify a generic-dict `^Hash A` / `^Show A` body over a `Map[K V]` (and over
  other typed aggregates -- `Vec[T]`, `Option[T]`, `Result[E A]`, `Pair[A B]`,
  `Set[T]`) re-resolves to the right `__inst_*` and behaves like the concrete
  `.method` path. Add fixtures for at least one non-`Eq` class and one non-`Map`
  aggregate to prove the `TY_APP` branch is general.
- If any aggregate's instance body is itself identity/incomplete for the
  generic-dict path, either reuse the GDE3 mechanism (3a runtime stamp / 3b
  dictionary) or note it as out of scope with a clear reason.
- **Acceptance:** generic-dict dispatch is content-correct for `Eq`/`Hash`/`Show`
  over `Map` and at least one other aggregate; fixtures green.

### GDE5 -- snapshot regen, docs, and close-out

- Regenerate every affected `expected.c` snapshot using the **per-fixture**
  `flags` (never the flagless loop -- see the GMK postmortem). Confirm
  `bash tests/run.sh` reports zero `FAIL`.
- Regenerate `docs/api/` + `stdlib/docstrings.tur` if any docstring changed
  (`python3 tools/gendocs.py stdlib/ --out docs/api/ --emit-tur stdlib/docstrings.tur`).
- Update the parent plan's status (now archived) cross-reference and mark this
  plan **Done**; flip the "Remaining work" item #1 / Open-decision #6 in the
  archived parent from *deferred* to *delivered here* (already done in the
  archive header as part of extracting this plan).
- **Acceptance:** `bash tests/run.sh` zero `FAIL`; the GDE0 driver and the GDE4
  generalization fixtures pass; no snapshot drift left uncommitted.

## File touchpoints

| Phase | File | Change |
|---|---|---|
| GDE0 | `tests/fixtures/gde-generic-dict-eq-map/` (new) | Red reproduction: generic-dict `Eq [Map]` over distinct-pointer `:cstr` keys |
| GDE1 | `src/compiler/emit_module.c` (`emit_abi_register_call`) | Instance-driven spec trigger: spec a constrained generic when a tyvar param's `dict_arg` method dispatch resolves to a non-representative instance, even with stable ABI |
| GDE2 | `src/compiler/emit_core.c` (`emit_reresolve_method_call`, `emit_inst_suffix_component`), `src/compiler/emit_stmt.c` (instance/dict-name suffix) | `TY_APP` receiver re-resolution to `__inst_<Class>_<method>_<Ctor>`; keep def-name and call-name manglers in sync |
| GDE3 | runtime `tur_hamt_*` + `stdlib/map.tur` (3a) **or** monomorphizer dictionary core (3b) | Content-correct `__inst_Eq_eq__Map` body (default 3a: runtime-stamped comparators + `tur_hamt_eq_dynamic`) |
| GDE4 | fixtures (+ any aggregate instance bodies) | Generalize to `Hash`/`Show` and `Vec`/`Option`/`Result`/`Pair`/`Set` |
| GDE5 | `expected.c` snapshots, `docs/`, `stdlib/docstrings.tur` | Regen + docs + close-out |

## Risks

- **Monomorphizer-core blast radius (GDE1).** The instance-driven spec trigger
  runs in `emit_abi_register_call`, on the hot path for *every* constrained-
  generic call. Gate it as narrowly as possible (tyvar param that is a `dict_arg`
  receiver *and* resolves to a non-representative instance) and add a negative
  fixture proving ABI-stable, non-dispatching generics are untouched. Expect some
  snapshot churn; regenerate with per-fixture flags and verify behavior via
  `tests/run.sh`, not by eyeballing diffs.
- **Def-name / call-name skew (GDE2).** If the `TY_APP` suffix mangling differs
  between the instance *definition* (`emit_stmt.c`) and the re-resolved *call*
  (`emit_core.c`), the call references an undefined symbol (link error). Share
  one sanitize/mangle helper between both sites.
- **Runtime ownership/lifetime (GDE3, 3a).** Stamping comparators into the map
  must respect the existing HAMT key-ownership model (WKC3's
  `tur_hamt_box_key` / `*_eq_o`). The comparator pointers are non-capturing
  function pointers (carrier ABI `bool(int64_t,int64_t)`), so they are static --
  but verify no per-map allocation leaks (ASan/LSan on the compiled path).
- **Int-key behavior preservation.** Int keys must stay correct and ideally
  avoid needless re-spec (hash == carrier == key, so identity already agrees).
  Keep the carrier fast path in GDE1 so `A = int` is not specialized.

## Non-goals

- A general method-as-value feature beyond what these phases need (3b may deliver
  HKT dictionary passing as a side effect, but it is not a gate).
- Compile-time key-type safety / removing type erasure of the map handle (that is
  [typed-map-surface-plan.md](archive/typed-map-surface-plan.md)'s domain).
- Aggregate keys wider than the one-word carrier beyond what WKC already covers.

## Open decisions to record as the work lands

1. GDE3: **3a (runtime-stamped comparators)** vs **3b (bare-HKT dictionary
   passing)** -- default 3a; record the final choice and, if 3a, the
   `tur_hamt_eq_dynamic` signature and where the comparator is stamped.
2. GDE1: the exact predicate for "instance differs from the representative" --
   compare resolved `__inst_*` names, or compare instance identities directly.
3. GDE4: which aggregates beyond `Map` are in scope for the generalization
   fixtures, and whether any aggregate instance body needs its own GDE3-style
   content fix.
