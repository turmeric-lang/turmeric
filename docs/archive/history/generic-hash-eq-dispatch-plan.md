# Generic `Hash` / `Eq` Dispatch -- Approach A for typed map keys (GHE0--GHE5)

> **Status (2026):** GHE0--GHE5 are **effectively complete.** GHE0--GHE3 + the
> CGI dispatch fix landed on this plan's branches; **GHE4 and GHE5 were delivered
> by the typed `Map[K V]` surface work (TMS, PR #167)** that landed on `main` and
> subsumes them -- see the *Landed since (GHE4/GHE5 -- delivered by TMS)* note
> below for what shipped and the resolved open decisions.
>
> **GHE5 `Eq [Map]` content equality (follow-up, landed):** direct `(.eq? a b)`
> on a concrete `Map[K V]` is now content-correct for **`:cstr`** and **heap-boxed
> struct** keys (threaded at the dispatch site; fixtures `eqmap-cstr-content`,
> `eqmap-struct-content`), and the `(:: <int> :Opaque)` ascription segfault was
> fixed en route (`opaque-ascribe-int`).  See the *Eq [Map] content equality*
> note below.
>
> **Remaining work (not landed):**
>   1. The **generic-dict** `Eq [Map]` path -- `(eq? a b)` reached through a
>      polymorphic `^Eq A` constraint rather than a concrete `.eq?` -- is still
>      identity-keyed (in fact compares map pointers as ints).  This is the large
>      three-part "constrained-generic instance specialization" feature (CGI gap
>      #2); the three coupled compiler changes are enumerated in the *Remaining
>      (large -- the generic-dict path)* note below.  **No longer deferred:** this
>      work has been extracted into its own active plan,
>      [generic-dict-eq-map-dispatch-plan.md](../generic-dict-eq-map-dispatch-plan.md)
>      (GDE0--GDE5), which sequences the three coupled monomorphizer/emit changes
>      and a red reproduction fixture.
>   2. ~~Struct keys with **non-`:int` fields** (the zero-struct witness only
>      type-checks for all-`:int`-field structs).~~ **DONE** — `zero_form_for_field`
>      in `elab_typeclasses.c` handles `float32`, `float64`, `bool`, `cstr`, and
>      all integer variants; fixture `eqmap-struct-float-fields` added.
>
> The historical status below is kept for the GHE2/GHE3/CGI root-causing record.
>
> ---
>
> > **Historical status:** GHE0 + GHE1 landed (branch `claude/focused-mendel-Sp7cQ`). GHE2--GHE5
> are **blocked** on a deeper compiler gap discovered during GHE1 verification:
> *constrained-generic method dispatch resolves the wrong instance.* A body that
> calls `(.hash x)` (or bare `(hash x)`) where `x : K` is a type parameter
> constrained by `^Hash K` does **not** dispatch through the constraint -- both
> the `int` and `cstr` monomorphizations emit `__inst_Hash_hash_float32(x)`
> (some arbitrary instance), rather than the per-specialization concrete
> instance or a dictionary load. The codebase has **no** working example of a
> `^Class`-constrained generic whose body actually calls a method on the
> constrained parameter (`tests/fixtures/typeclass-constraint` only checks that
> the *syntax* parses; its body is `true`). The GHE3 builder `map-assoc-g`
> depends entirely on this working, so it cannot be implemented as specified
> until constrained-generic method dispatch is fixed first -- a compiler feature
> this plan assumed already existed (see "Relationship to existing building
> blocks", which over-claimed the dictionary path works for poly tyvars).
>
> **Done:**
> - **GHE0** -- `stdlib/typeclass-hash.tur` stub auto-loaded; `(.hash 5)` etc.
>   dispatch by default; Hash removed from `typeclass.tur` to keep it idempotent.
> - **GHE1** -- bare-name typeclass method calls (`(hash x)`, `(eq? a b)`,
>   `(show v)`) now route to argument-type dispatch in `elab_call.c`, gated on
>   no existing binding + class membership. Fixture `ghe1-bare-method-dispatch`.
>
> **Why the dispatch fix is large (root-caused 2026-06-01):** the compiler has
> *no* dictionary threading for constrained generics and *no* typeclass-instance
> monomorphization. Constrained generics are realised purely by **emit-time ABI
> specialization** (`emit_module.c` `emit_abi_*`, `emit_call_name`/
> `emit_resolve_type` in `emit_core.c`). Three coupled gaps:
>   1. **Wrong instance baked at elab.** For a `TY_TYVAR` receiver,
>      `elab_method_call` (`elab_typeclasses.c:2698`) takes the `KIND_ARROW`
>      branch and "matches" the first instance whose `type_args[0]` is not in the
>      *incomplete* primitive list at line ~2724 (which omits `TY_FLOAT32`,
>      `TY_INT8..UINT64`, `TY_FLOAT64`) -- so a tyvar spuriously binds to
>      `Hash[float32]` and is recorded as an exact match (no `TUR_E0020`).
>   2. **ABI-only specialization.** A spec is created only when
>      `abi_changes` is true (`emit_module.c:374`). An `int` key == the int64_t
>      carrier, so *no* spec is made and the call uses the polymorphic **base
>      clone** -- which has no `current_abi_specialization`, so `emit_resolve_type`
>      cannot map `K`. Correct dispatch needs *instance-driven* specialization
>      (specialize when the body's tyvar method-dispatch differs per concrete
>      type, even when ABIs match), plus making the base clone dead for such fns.
>   3. **Emit-time re-resolution.** Even with a spec, `emit_call_name` must
>      re-derive `__inst_<Class>_<method>_<T>` from the resolved receiver type,
>      and the arg/result coercion in the direct-call path
>      (`emit_expr.c:1890+`) keys off the (wrong) baked binding's `arg_kinds`.
>
> All three must land together; the change spans elab + the monomorphizer + emit,
> with broad codegen-snapshot impact. This is a dedicated compiler feature, not a
> within-GHE patch -- it should be scoped as its own plan
> (`constrained-generic-instance-dispatch`) that GHE2--GHE5 then build on.
>
> **Landed since (CGI -- the hash half of Approach A):** the constrained-generic
> *method dispatch* fix is done (commit `CGI:` on this branch). For a `TY_TYVAR`
> receiver, `elab_method_call` now picks the carrier-compatible (`TY_INT`)
> instance as the representative (valid base clone, correct for `int` keys) and
> tags the call via `dict_arg`; `emit_call_name` re-resolves the call to
> `__inst_<Class>_<method>_<T>` for each non-carrier ABI specialization
> (cstr/bool/float32/...). Fixture `cgi-constrained-generic-dispatch` proves
> `(hash x)` and `(eq? a b)` dispatch correctly through `^Hash K` / `^Eq K`.
> A `map-assoc-g`/`map-get-g` prototype using this round-trips **int, bool, and
> float32** keys correctly (their hashes are injective on the int64_t carrier,
> so the carrier comparator is correct).
>
> **Landed since (GHE2 -- the comparator half / A2 generalized):** per-`K`
> specialization of a *function-value* reference is done. The ABI scan
> (`emit_abi_register_call`) now recurses into a freshly-created spec's body
> under that spec's tyvar bindings; `emit_abi_scan_fn_values` (emit_module.c)
> detects a generic fn referenced as a *value* argument (an inline
> `(fn [a :K b :K] (eq? a b))` comparator, or a named generic fn) and interns a
> child specialization for it, recursing so nested fn-values/method calls
> specialize too. At emit time `emit_reresolve_fn_value` (emit_core.c, consulted
> from `atom_var`) names the per-`K` child clone for the value reference -- the
> analogue of the `emit_call_name` re-resolution CGI added for method *calls*.
> The child clone's body re-resolves `(eq? a b)` to the concrete instance via the
> existing CGI machinery, so a distinct-pointer cstr lookup matches by content.
>
> **Landed since (GHE3 -- the `-g` builders):** `map-assoc-g` / `map-get-g` /
> `map-has-g?` / `map-dissoc-g` are in `stdlib/map.tur`, each a constrained
> generic `[^Hash K ^Eq K ...]` routing through `(hash key)` + an inline
> `(fn [a :K b :K] (eq? a b))` comparator threaded into the runtime `*-eq` HAMT.
> Fixture `ghe3-generic-map-key` proves content-keyed `:cstr` (distinct-pointer
> update collapses to one entry), identity `:int`, and `:bool` keys all round
> trip through one path. **`:float32` / `:float` keys are now also covered**
> (since the WKC carrier work below); `ghe3-generic-map-key` asserts a `:float32`
> key alongside the others and `wkc-wide-map-key` is the focused float
> round-trip. The runtime `*-eq` carrier is still one word (`void*` key +
> `bool(i64,i64)` comparator), but a `MapKey[K]` typeclass bit-reinterprets an
> inline float key into the carrier (no numeric truncation) and supplies a
> carrier-ABI `bool(i64,i64)` comparator that reinterprets back, so `1.5` no
> longer collapses to `1`. **Multi-word struct/ADT keys are now supported too**
> (WKC3): a `MapKey[K]` instance heap-boxes the key via the refcount-aware
> `tur_hamt_box_key` carrier (owned by the map) and the `-g` builders route
> through ownership-aware `tur_hamt_*_eq_o` calls; see
> [float32-map-key-carrier-plan.md](float32-map-key-carrier-plan.md) (W2).
> The remaining cost is a small per-struct `MapKey` instance (a `derive-map-key`
> macro is a follow-up); the carrier itself no longer restricts key width.
>
> **Landed since (GHE4/GHE5 -- delivered by TMS, PR #167):** GHE4 and GHE5 were
> realized by the **typed `Map[K V]` surface** work
> ([typed-map-surface-plan.md](typed-map-surface-plan.md), TMS2--TMS5),
> which landed on `main` and subsumes them:
> - **GHE4** -- `#map{...}` / `hamt-of` now route **every** key type through one
>   content-keyed builder (`(Hash K)` + `(MapKey K)`); the `smap-of` / `smap-*`
>   surface was removed and the `F_MAP_LITERAL` / `elab_call.c` string-key
>   special-cases deleted. The `-g` builders were folded into unified
>   `map-assoc` / `map-get` / `map-has?` / `map-dissoc` **macros** over a real
>   `Map[K V]` type (the macro form is what makes per-`K` `MapKey` dispatch
>   resolve at each concrete call site -- a constrained-generic *function* whose
>   ABI does not change across `K` would bake the carrier instance instead).
> - **GHE5** -- reads use those same unified accessors; the data-literals guide,
>   `hamt-of` docstring, and snapshots were regenerated. A user `Hash` + `MapKey`
>   instance on a `defopaque` key works for free **when constructed via a
>   coercion fn** (`(defn mk-uid [t :int] :UserId t)`); note that the unrelated
>   `(:: <int> :Opaque)` *ascription* form currently mis-emits a pointer
>   dereference and segfaults -- a pre-existing opaque-codegen bug, out of scope
>   here.
>
> **Decisions, as resolved by TMS:** (open-decision #2) keyword keys stay
> hash-collapsed to an int key -- maps have no first-class keyword key type;
> (#3) the `smap-*` surface is **removed**, not aliased; (#4) the int path is
> uniform with every other key (no byte-for-byte gate; snapshots regenerated).
>
> **GHE5 `Eq [Map]` content equality -- `:cstr` AND struct keys now
> CONTENT-correct.** `(.eq? a b)` on a concrete `Map[K V]` receiver compares keys
> by content for `:cstr` keys (distinct pointers, equal text) and for heap-boxed
> **struct** keys (distinct boxes, equal fields), closing the reported gap for
> the common cases. Done **not** by fixing the bare-HKT instance body (see #4
> below) but by extending the existing dispatch-site synthesis
> (`try_synth_recursive_eq` in `elab_typeclasses.c`): at the *concrete* `.eq?`
> call site the compiler threads the per-`K` `MapKey` comparator into a new
> content-aware `map-eq-k?` helper, alongside the recursive value comparator. The
> key witness that drives `MapKey[K]` dispatch is built per key kind (`mk-cmp`
> ignores its argument value):
>   - `:cstr` -> `(mk-cmp (:: 0 cstr))` (the int->cstr ascription is a no-op);
>   - all-`:int`-field struct `K` -> `(mk-cmp (make-struct K 0 ...))` (a by-value
>     zero struct matching `mk-cmp[K]`'s by-value ABI; `(:: 0 K)` would deref a
>     non-pointer aggregate).
> Fixtures `eqmap-cstr-content`, `eqmap-struct-content`.
>
> A related general bug was fixed en route: `(:: <int> :Opaque)` (a `defopaque`
> newtype, which has no fields and emits as `int64_t`) was mis-emitting a
> `*(int64_t *)(value)` carrier->concrete dereference and segfaulting; the
> `EX_ASCRIBE` emit now skips the bridge for opaque types (fixture
> `opaque-ascribe-int`). Opaque-over-int map keys are inline int values, so their
> `.eq?` was already identity-correct -- they only needed this segfault fix.
>
> Why this route and not the literal "#4": the `Eq [Map]` *instance* body
> receives a bare-`Map` (unapplied HKT) handle and a typed `map-eq?` *function*
> cannot resolve the per-`K` comparator (the CGI "no ABI change -> no
> specialization" gap bakes `mk-cmp_int`; and the recursive `.eq?` synthesis
> pins `map-eq?` to a *function* form, so it cannot just be a macro). The
> dispatch-site synthesis sidesteps both by emitting a specialized call where
> `K` is concrete -- which is also exactly how the stdlib already gives
> `Vec`/`Option`/`Result`/`Pair`/`Set` recursive *value* equality.
>
> ~~**Remaining (smaller):** struct keys with **non-`:int` fields** -- the zero
> witness `(make-struct K 0 ...)` only type-checks when every field is `:int`; a
> field-type-aware zero (or a deref-safe inline-C witness) would lift this.~~
> **DONE** -- `zero_form_for_field` in `elab_typeclasses.c` handles `float32`,
> `float64`, `bool`, `cstr`, and all integer variants; fixture
> `eqmap-struct-float-fields` added.
>
> **Remaining (large -- the generic-dict path).** **No longer deferred --
> extracted to
> [generic-dict-eq-map-dispatch-plan.md](../generic-dict-eq-map-dispatch-plan.md)
> (GDE0--GDE5).** When `Eq [Map]` is used through a *polymorphic* `Eq` constraint rather than a concrete `.eq?` -- e.g.
> `(defn eq2 [^Eq A] [a :A b :A] :bool (eq? a b))` then `(eq2 mapX mapY)` -- the
> result is still identity-keyed (in fact worse: see below).  The dispatch-site
> synthesis cannot help here because the body's `(eq? a b)` is elaborated once
> with `a : A` a *type variable*, so the concrete `Map[cstr int]` type the
> synthesis needs is not visible until the instantiation site.  Closing it needs
> three coupled compiler changes (the full "constrained-generic instance
> specialization" feature, CGI gap #2), each in the monomorphizer/emit core:
>   1. **Instance-driven specialization.** `eq2`'s body bakes
>      `__inst_Eq_eq__int` (the `TY_INT` carrier representative) and is never
>      specialized for `A = Map[cstr int]` because that type's ABI is `int64`,
>      identical to `int`, so `emit_abi_register_call`'s `abi_changes` is false.
>      So `(eq2 mapX mapY)` compares the two map *pointers* as ints.  Fix: force a
>      spec when a tyvar parameter dispatches a typeclass method (carries a
>      `dict_arg`) and binds to a type whose instance differs from the baked
>      representative, even when the ABI is unchanged.
>   2. **`TY_APP` re-resolution.** `emit_inst_suffix_component` returns NULL for
>      `TY_APP`, so even inside a spec `emit_reresolve_method_call` could not name
>      `__inst_Eq_eq__Map` from a `Map[cstr int]` receiver.  Fix: extract the
>      struct constructor from a `TY_APP` chain and mangle by its name.
>   3. **Content-correct `__inst_Eq_eq__Map`.** Even reaching the instance is not
>      enough -- its body is identity.  The dispatch-site synthesis cannot fix it
>      (the body is generic over `K`), so this needs either a key comparator
>      *stamped into the map at runtime* (a `tur_hamt_*` change, so the instance
>      body reads it with no compile-time dispatch) or true bare-HKT
>      element-method dispatch in the instance body.
> Content-keyed *lookup* (`map-get`/`map-has?`) is unaffected and correct.
>
> This is the deferred **Approach A** from
> [generic-map-key-dispatch-plan.md](generic-map-key-dispatch-plan.md) (see its
> *Decision note (GMK1)*). GMK shipped content-keyed string maps via
> **Approach B** (a `:cstr`-specific lowering onto the hand-written `smap-*`
> layer). Approach A replaces that special-case with *uniform* dispatch: a
> single constrained-generic builder routes `hamt-of` / `#map{...}` through
> `Hash[K]` + `Eq[K]` for **any** scalar key type, and -- as a prerequisite --
> makes typeclass methods callable generically at all.
>
> **Type:** Compiler (call elaboration + typeclass dispatch) + stdlib. No new
> runtime: the content-equality primitive (`tur_hamt_*_eq` + the thread-local
> `keys_equal` override) already exists from TCE4.
>
> **One-line goal:** `(defn map-assoc-g [K V] #{(Hash K) (Eq K)} ...)` compiles
> and dispatches, so `#map{...}` over *any* `Hash`/`Eq` key type (int, cstr,
> bool, float32, and user instances) builds a correct content-keyed map through
> one code path -- collapsing GMK's `:cstr` special-case (Approach C).
>
> **Scope note:** GHE unifies *dispatch* but leaves the map handle type-erased
> to `:int`, so it does not add compile-time key-type safety or remove the
> `smap-*` vs `map-*` accessor split. That ergonomics/safety layer is planned
> separately in [typed-map-surface-plan.md](typed-map-surface-plan.md) (TMS),
> which builds on this plan.

## Why this is more than a one-line emission fix

GMK0 was probed during the GMK work and turned out to be **two distinct gaps**,
both reproduced on `claude/blissful-mccarthy-xiF8C` (2026-05-31):

### Gap 1 -- `Hash` / `Show` classes are not in scope by default

The auto-load list in `src/main.c` (`stdlib_files[]`, ~line 628) loads only the
*stub* typeclass modules -- `typeclass-eq.tur`, `typeclass-functor.tur`,
`typeclass-clone.tur` -- so that typed-collection `definstance`s have those
classes in scope. The **full `typeclass.tur`** -- which declares `Hash`, `Show`,
`Num`, `Ord` *and* their primitive instances (`Hash[int]`, `Hash[cstr]`,
`Hash[bool]`, `Hash[float32]`) -- is explicitly **not** auto-loaded ("remains
on-demand", comment at `src/main.c:643`).

Consequence: `(.hash 5)` and `(.show 5)` fail with *"no typeclass method found"*
(`elab_typeclasses.c:2784`) because the class is unregistered, while `(.clone 5)`
and `(.eq? 1 2)` dispatch fine (their stub classes are auto-loaded). And
`(load "stdlib/typeclass.tur")` does **not** work around it: typeclass.tur
re-defines `Clone`/`Eq`/`Functor` instances that the stubs already emitted,
producing C `redefinition of '__inst_Clone_clone_int'` errors.

### Gap 2 -- bare-name typeclass calls are never dispatched

Even for an auto-loaded class, a **bare** `(method args...)` call is not routed
to instance dispatch. Evidence:

- `(eq? 1 2)` (Eq is auto-loaded) elaborates to something typed `:int`, not
  `:bool` -- `if`-condition then rejects it (*"if condition must be bool, got
  int"*). It resolved via the eval-mode "assume native" fallback, **not** the
  `Eq[int]` instance.
- `(hash 5)` / `(hash "a")` emit an *undeclared* callee `hash_<gensym>` -- the
  exact same shape a genuinely-undefined `(notamethod 5)` produces.

In `src/compiler/elab_call.c`, a head symbol that is a typeclass **method name**
is looked up with `elab_lookup_sym` (line ~924); finding nothing, it falls
through to the partial-application / eval-mode call path rather than to
`elab_method_call`. Only the **dot** form `(.method obj ...)` reaches
`elab_method_call` (the `name[0]=='.'` check at line ~865), where argument-type
dispatch and monomorphization to `best_method->binding` actually happen
(`elab_typeclasses.c:2676-2982`).

So Approach A's `map-assoc-g` body -- which calls `(hash key)` and compares keys
with `Eq[K]` -- cannot compile until **both** gaps close: the class must be in
scope (Gap 1) and bare-name method calls must dispatch on their argument's
static type (Gap 2). Gap 2 is a core call-elaboration feature with blast radius
across *every* typeclass; that breadth is why GMK deferred it.

## Relationship to existing building blocks (verified)

- **`Hash` / `Eq` classes + instances** -- `stdlib/typeclass.tur` (Hash, Show,
  Num, Ord with `int`/`bool`/`cstr`/`float32` instances) and
  `stdlib/typeclass-eq.tur` (Eq class + structural instances).
- **Instance method registration** -- each `definstance` method is registered
  globally under a mangled name `__inst_<Class>_<method>_<type>`
  (`elab_typeclasses.c:1664,1981`); dispatch resolves a receiver's static type
  against `e->typeclass_env.instances` (`elab_typeclasses.c:2699-2777`) and, on
  a unique match, emits a **direct** call to `best_method->binding`
  (`elab_typeclasses.c:2969`), skipping the dictionary. Ambiguity is
  `TUR_E0020`.
- **Dictionary path** -- when the receiver is a polymorphic tyvar
  (`has_poly_params`), `make_dict_expr` builds an `EX_DICT` that emits
  `dict_<Class>_<type>_singleton.<method>(...)` (`elab_typeclasses.c:2949-2974`,
  `emit_stmt.c:355-516`, `emit_expr.c` `EX_DICT`).
- **TCE4 content-equality layer** -- `tur_hamt_*_eq`, `map-{assoc,get,has?,
  dissoc}-eq`, `cstr-hash`, `tur-cstr-key-eq?`, and the `smap-*` convenience
  layer (now the GMK lowering target for `:cstr`).
- **GMK Approach B surface** -- `smap-of` macro + `#map{...}`/`hamt-of` string
  dispatch (`elab_toplevel.c` F_MAP_LITERAL, `elab_call.c` hamt-of rewrite).
  This is what Approach A will eventually subsume.

## Phasing

### GHE0 -- make `Hash` (+ `Eq` content) available by default (DONE)

Resolve Gap 1 without the redefinition collision. Preferred shape: split a
`typeclass-hash.tur` **stub** that declares `(defclass Hash [a] (hash [x] :int))`
plus the primitive instances (`int`, `bool`, `cstr`, `float32`), mirroring
`typeclass-eq.tur`, and add it to `stdlib_files[]` *before* `map.tur` (which
conceptually requires `Hash[K]`). Remove those declarations from `typeclass.tur`
or guard them so the on-demand full module stays idempotent against the stub
(the same stub/full split already used for Eq/Clone/Functor).

- **Acceptance:** with no `(load ...)`, `(.hash 5)`, `(.hash "a")`, and
  `(.hash true)` build and print stable values; equal strings hash equally; no
  `redefinition` errors; zero `expected.c` churn for programs that do not
  reference `hash` (stub bodies must be dead-strippable / not force-live like
  the Eq stub).

### GHE1 -- dispatch bare-name typeclass method calls (DONE)

Resolve Gap 2. In `elab_call.c`, before the eval-mode fallback, detect that the
head symbol is a registered typeclass **method** (scan `e->typeclass_env`
classes' method names) and, when it is, route to the same argument-type
dispatch `elab_method_call` performs -- i.e. treat `(hash x)` like `(.hash x)`
(first arg is the receiver). Reuse the existing instance-resolution and
monomorphization (`best_method->binding`) so a statically-typed argument emits a
**direct** call to `__inst_Hash_hash_<T>`, and a polymorphic argument emits the
dictionary load.

Design constraints / open questions:

- **Disambiguation from user defns.** A user `(defn hash ...)` or a local
  binding named like a method must still win; only fall into method dispatch
  when `elab_lookup_sym` finds nothing (or finds the class-method placeholder).
  Confirm there is no name that is *both* a stdlib defn and a method.
- **Receiver selection for multi-param / return-dispatch methods.** Methods
  whose dispatch variable is only in the return type already have
  `elab_try_return_dispatch` (`elab_call.c:935`); keep that path. For
  argument-dispatched methods, the receiver is the argument whose type is the
  class type variable -- usually arg 0, but verify against `Num.add [x y]` etc.
- **Backwards compatibility.** Some programs may currently rely on the
  eval-mode fallback resolving a same-named *native*. Gate behind the existing
  class-membership test so only genuine method names are intercepted; this
  cannot regress a program that never declared the class.

- **Acceptance:** bare `(hash 5)`, `(hash "a")`, `(eq? 1 2)` (`:bool`),
  `(show 5)` compile and run with the correct instance; a fixture asserts
  `(= (hash "a") (hash "a"))` and `(eq? 1 1)`. `bash tests/run.sh` green.

### GHE2 -- thread `Eq[K]` to the runtime as a `bool(int64_t,int64_t)` pointer (DONE)

The runtime `*-eq` ops need the comparator as a plain non-capturing function
pointer. Resolve the GMK1 `<eq-closure-for-K>` question. Two sub-options
(decide here, record the choice in this doc):

- **A1 -- method-as-value.** Make a typeclass method referenceable as a
  first-class value so the constraint-resolved `eq?` can be passed directly.
  General, larger; today referencing `eq?` as a value is `TUR-E0003`.
- **A2 -- per-`K` wrapper synthesis.** At each concrete instantiation, emit a
  tiny non-capturing `defn __eq_<K> [a :K b :K] :bool (eq? a b)` and pass its
  address -- mirrors the hand-written `tur-cstr-key-eq?`. Smaller, localized;
  the compiler emits one wrapper per `K` actually used as a key.

A2 is the lower-risk default (it is exactly what `smap-*` does by hand,
generalized). A1 is the cleaner long-term fix and is independently useful.

**DONE (A2 generalized).** Rather than synthesize a named `__eq_<K>` wrapper,
the compiler now specializes the lifted comparator *fn-value* itself per `K`, so
a plain inline `(fn [a :K b :K] (eq? a b))` works directly. The implementation
spans three sites:
- `emit_abi_scan_fn_values` (emit_module.c) -- detects a generic fn referenced
  as a value argument inside an active spec body and interns a child spec for it
  under the spec's tyvar bindings, recursing into the child clone.
- `emit_abi_register_call` -- after creating a spec, recurses into the spec body
  with those bindings active so the scan above fires.
- `emit_reresolve_fn_value` (emit_core.c, from `atom_var`) -- emits the per-`K`
  child clone name at the value-reference site.

The child clone's `(eq? a b)` re-resolves to the concrete instance via the
already-landed CGI method-call machinery. Purely additive: no existing fixture
references a generic fn-value inside a constrained-generic body, so the only
snapshot churn is gensym-ID shift from the new stdlib `-g` builders.

- **Acceptance (met):** `map-assoc-g`/`map-get-g` round-trip `:cstr` keys with
  distinct-pointer equality (fixture `ghe3-generic-map-key`).

### GHE3 -- the constrained-generic builder `map-assoc-g` (DONE)

With GHE0--GHE2 in place, `stdlib/map.tur` now carries (inline constraint form
`[^Hash K ^Eq K V]`, which is how the codebase spells constrained generics):

```turmeric
(defn map-assoc-g [^Hash K ^Eq K V] [m : int key : K val : V] : int
  (map-assoc-eq m (hash key) key val (fn [a : K b : K] : bool (eq? a b))))
```

plus `map-get-g` / `map-has-g?` / `map-dissoc-g`. The inline comparator
specializes per `K` via GHE2.

**Int-key codegen note (revised).** The plan originally required the `int`
instantiation to be byte-for-byte the identity `map-assoc` (`eq == NULL`
runtime fast path). With the uniform A2 comparator model the `int` path instead
routes through `map-assoc-eq` carrying the `Eq[int]` comparator. This is
*behaviorally* identical for int keys (hash == carrier == key, so a hash match
implies a true key match and `(= a b)` agrees with identity) but **not**
byte-for-byte the old int codegen. That only matters at GHE4, when existing
int-keyed literals are re-targeted -- see the revised gate in the status header.

- **Acceptance (met):** `map-assoc-g`/`map-get-g`/`-has-g?`/`-dissoc-g`
  round-trip for `K` in {`int`, `cstr`, `bool`, `float32`, `float`}; the `cstr`
  instantiation matches `smap-*` content-keyed behavior (distinct-pointer
  update collapses to one entry). `:float32` / `:float` were reinstated by the
  WKC carrier work (a `MapKey[K]` typeclass that bit-reinterprets the inline
  float key into the one-word carrier and supplies a carrier-ABI comparator);
  see [float32-map-key-carrier-plan.md](float32-map-key-carrier-plan.md).
  Fixtures: `ghe3-generic-map-key`, `wkc-wide-map-key`.

### GHE4 -- route `hamt-of` / `#map{...}` through `map-assoc-g`

Re-target the literal lowering and the `hamt-of` macro onto the `-g` builder,
collapsing GMK's Approach-B special-case (Approach C):

- `#map{...}` (`elab_toplevel.c` F_MAP_LITERAL): drop the string-key ->
  `smap-of` branch; lower **all** key types onto the `-g` chain. Keep the
  keyword-key `(hamt/hash-str ...)` normalization only if keyword keys are kept
  as a deliberate hash-collapsing convenience (decide + document).
- `hamt-of` macro / the `elab_call.c` hamt-of->smap-of rewrite: replace with
  the uniform `-g` chain; keep `smap-of` as a thin alias for source
  compatibility.
- **Gate:** existing `gmk-map-literal-cstr-key`, `tce3/4/5` fixtures still pass; regenerate snapshots and verify `bash tests/run.sh` zero `FAIL`.

- **Acceptance:** `#map{"a" 1}`, `#map{1 "x"}`, `#map{true 1}` all build
  correct content-keyed maps through one path; reads use the same `Eq[K]`.

### GHE5 -- generalize reads + docs + snapshot regen

`map-get` / `map-has?` / `map-dissoc` / `map-merge` / `Eq [Map]` must use the
same `Hash`/`Eq` for content keys. Provide `-g` reads wherever a literal can
build a content-keyed map. Then update docs and regenerate snapshots:

- `docs/guides/data-literals-guide.md` (relax key-type notes to "any
  `Hash`/`Eq` scalar"), the `hamt-of` / `smap-of` docstrings, this plan's
  status table, and the GMK plan's follow-up note (mark Approach C done).
- Regenerate `docs/api/` + `stdlib/docstrings.tur` and all `expected.c`
  snapshots (use the per-fixture `flags`; never the flagless loop -- see the
  GMK postmortem where a flagless regen wiped `sized-sz6-erasure`).
- **Acceptance:** `bash tests/run.sh` zero `FAIL`; `Hash`/`Eq` user instance
  for a `defopaque` key works for free.

## File touchpoints

| Phase | Status | File | Change |
|---|---|---|---|
| GHE0 | ✅ Done | `stdlib/typeclass-hash.tur` (new), `stdlib/typeclass.tur`, `src/main.c` | Split + auto-load a `Hash` stub; de-dupe instances |
| GHE1 | ✅ Done | `src/compiler/elab_call.c` (+ `elab_typeclasses.c`) | Route bare-name method calls to argument-type dispatch |
| GHE2 | ✅ Done | `src/compiler/emit_module.c`, `emit_core.c` | Eq comparator as a `bool(i64,i64)` pointer (A2 generalized fn-value specialization) |
| GHE3 | ✅ Done | `stdlib/map.tur` | `map-assoc-g` + `-g` reads; int fast path |
| GHE4 | ✅ Done (via TMS PR #167) | `src/compiler/elab_toplevel.c`, `elab_call.c`, `stdlib/map.tur` | Uniform `-g` lowering; retire Approach-B special-case |
| GHE5 | ✅ Done (via TMS PR #167) | `stdlib/map.tur`, `docs/`, snapshots | Read surface, docs, regen; `Eq [Map]` content-correct for `:cstr`+struct keys |

## Risks

- **Blast radius of bare-name dispatch (GHE1).** Intercepting method names in
  the main call path touches every program. Gate strictly on class membership;
  add fixtures for "user defn shadows a method name" and "method name that is
  not a method in this program is still an unbound-symbol error".
- **Int-key snapshot churn.** The `:int` path through `map-assoc-g` + `Eq[int]` is behaviorally correct; expect snapshot diffs for int-keyed fixtures and regenerate them. Verify correctness via `bash tests/run.sh`.
- **Stub/full typeclass collision (GHE0).** Mirror the Eq/Clone idempotent
  split exactly; `(load "stdlib/typeclass.tur")` must remain valid alongside the
  auto-loaded stub.
- **Preamble bloat.** Stub bodies and `-g` builders are pulled live by macro
  references (as `hamt-of`->`map_assoc` and GMK's `smap-of`->`*-eq` already
  are). Keep stubs dead-strippable; accept only "new stdlib bodies in the
  preamble," as in TCE4/GMK.
- **Key lifetime.** Unchanged from GMK: the HAMT does not own keys; literal
  string keys are static; runtime-built keys must outlive the map.

## Non-goals

- Aggregate (multi-word struct/ADT) keys -- the carrier slot is one word.
- Changing value typing (TCE3 covers scalar values).
- A general method-as-value feature beyond what GHE2 needs (A1 may deliver it
  as a side effect, but it is not a gate).

## Open decisions to record as the work lands

1. ~~GHE2: **A1 (method-as-value)** vs **A2 (wrapper synthesis)** -- default A2.~~
   **Decided: A2, generalized to per-`K` fn-value specialization** (a plain
   inline `(fn ...)` comparator specializes; no named-wrapper synthesis needed).
2. ~~GHE4: keep keyword keys as hash-collapsed, or content-key them too.~~
   **Resolved by TMS: kept hash-collapsed** -- maps have no first-class keyword
   key type; `:name` lowers to `(hamt/hash-str "name")`, an int key.
3. ~~Whether `smap-*` / `smap-of` remain as public aliases or are deprecated.~~
   **Resolved by TMS: removed entirely.** The unified `map-*` macros are the
   single path; the literal lowering's string-key special-case was deleted.
4. ~~GHE4: hold the int path byte-for-byte, or relax the zero-diff gate.~~
   **Resolved by TMS: relaxed.** Int keys ride the same uniform carrier as every
   other key; snapshots were regenerated.
5. `:float32` (and other content keys wider/narrower than the one-word carrier):
   deferred to [float32-map-key-carrier-plan.md](float32-map-key-carrier-plan.md).
6. **GHE5 `Eq [Map]` -- `:cstr` and struct keys now content-correct** (fixtures
   `eqmap-cstr-content`, `eqmap-struct-content`). `(.eq? a b)` on a concrete
   `Map[K V]` threads the per-`K` `MapKey` comparator at the dispatch site
   (`try_synth_recursive_eq` + `map-eq-k?`): `(mk-cmp (:: 0 cstr))` for `:cstr`,
   `(mk-cmp (make-struct K 0 ...))` for all-`:int`-field structs. The
   `defopaque`-ascription deref segfault was also fixed (`opaque-ascribe-int`).
   ~~**Remaining:** non-`:int`-field struct keys~~ **DONE** (see above), and the
   generic-dict `Eq [Map]` path (still identity; needs the true general
   HKT-instance element-dispatch fix; **no longer deferred -- extracted to
   [generic-dict-eq-map-dispatch-plan.md](../generic-dict-eq-map-dispatch-plan.md),
   GDE0--GDE5**). Content *lookup*
   (`map-get`/`map-has?`) was always correct.
