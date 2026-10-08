---
title: Value Representations
category: Compiler Internals
description: Every representation a Turmeric value can travel in, the boundaries where the compiler must bridge between them, and the failure shapes -- cc errors, link errors, segfaults -- when a bridge is missing
---

# Value Representations and Boundary Bridges (internals)

This guide is the map of every representation a Turmeric value can travel in
between its producer and its consumer, the boundaries where the compiler must
convert between them, and the failure shapes when a conversion (a "bridge")
is missing. It exists because the bugs in this area all share one anatomy and
kept being rediscovered one boundary at a time -- see the Maintenance section
at the bottom before changing anything it describes.

Audience: compiler contributors and anyone triaging a `cc invocation failed`,
link error, or segfault on code that passed `tur check`.

## The invariant

At any given boundary crossing, a value has exactly **one** correct
representation on arrival: the one the destination type demands. Everything
else must be bridged away *at the crossing*. Every bug in this family is a
producer and a consumer disagreeing about which representation crosses --
the checker accepts (the *types* agree), and then either cc rejects the
emitted C, the linker misses a symbol, or the binary reinterprets memory and
dies. Wrong-output without a crash is the rarest and worst form (see the
float bit-reinterpret note below).

## Representations: ordinary data

1. **The int64 carrier** -- the universal `int64_t` slot. Typeclass method
   results, generic specializations before the bridge, `Vec` element slots,
   cons cells, and every inline-C body traffic in this. It is an *erasure*:
   the bits are the value for scalars and heap pointers, and a spilled
   pointer (or garbage) for anything by-value.

2. **By-value concrete aggregate** -- `tur_adt_Result__int__int`,
   `tur_adt_FzB`, ... passed and returned as real C structs at typed `defn`
   boundaries. The invalid-C errors make the mismatch visible:
   `tur_adt_FzB b = __ps_158;` initializing an aggregate from an `int64_t`,
   or `aggregate value used where an integer was expected` going the other
   way.

3. **Heap pointer** -- `:heap` structs and parametric heap containers
   (`Option`, `Vec`, `Cons`). The carrier bits ARE the pointer, so the
   erasure round trip is lossless. This is why `Option` escaped the `bind`
   miscompile while by-value `Result` did not
   (`result-monad-bind-typed-boundary-miscompiles`), and why `:heap` was
   the workaround for the by-value Vec-element shape
   (`vec-byvalue-struct-element-invalid-c`, resolved by increment 3).

   **Container element slots follow one width-independent rule** (increment
   3) per element class: scalar bits inline, heap
   pointer as-is, by-value ADT product (ANY width) heap-boxed on insert and
   deref-unboxed on read, fn value as a fat handle. The decision lives in
   `type_is_boxed_container_elem` (`src/compiler/types.c`), consulted by
   the push-side bridges, the read-back recovery, AND the ownership folds
   (`tur-wide-byval?` / `tur-vec-elem-wide?`) so boxing and freeing cannot
   drift. The old fork -- wide boxed, narrow stack-spilled with no reader --
   was the missing-cell generator here. Width still matters where a paired
   inline layout exists (parametric-carrier monomorph fields, B4 closure
   params); those positions keep `type_is_wide_byval_adt`.

4. **Concrete scalar** -- plain `int64_t` / `double` / `bool` / `char*`.
   One trap: a float crossing the carrier needs a **bit reinterpret**, not a
   numeric conversion. Picking the wrong one turns `1.5` into `4.6e18`
   silently -- pinned by
   `tests/fixtures/constrained-generic-dispatch-float-element`. This is the
   documented wrong-output case; treat any new float divergence as this
   family first.

   In **source**, the two are spelled apart and `::` spells neither: an
   int/float `::` is a hard error, because the operator cannot tell a carrier
   slot holding float bits from a genuine integer. Reading a float back out of
   an `:int` slot -- a cons cell, a variadic rest list, a HAMT value -- is
   `(bits->float x)`, and pushing one in is `(float->bits x)`, both from
   `stdlib/bits.tur`; converting a real number is `int->float` / `float->int`
   from `stdlib/math.tur`. See
   [docs/archive/ascribe-int-to-float-expression-ambiguity.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ascribe-int-to-float-expression-ambiguity.md).

## Representations: fn-typed values

Function values are their own zoo. The per-boundary decision today spans
(at least) these forms -- see the investigation in
`docs/archive/poly-result-hof-capturing-closure-sigbus.md` for where each
is chosen (`carrier_ok`, `src/compiler/elab_fns.c` ~3600):

1. **`tur_poly_fn_t {env, fn}` carrier** -- for plain, non-effectful,
   carrier-safe signatures with no named tyvar.  The carrier<->fat seam is
   alias- and join-aware: the stage-2 tail walkers
   resolve a let-ALIAS of a carrier param to its origin (converting via
   poly-to-fat like the direct leaf), the `if` unifier admits a
   carrier-param arm against a boxed fn result by inserting the conversion
   at the join, and ascribing a carrier param to its own fn type is a
   no-op assertion (`fn-value-carrier-fat-seam-residuals`, archived).
2. **`^fat` parameter** -- explicit fat `{thunk, env}` handle.
3. **`:ptr<void>`-fat sink** -- carries an `is_fat` flag disambiguating
   thin-vs-fat dispatch at the invoke (`src/compiler/emit_expr.c` ~4246).
4. **Nominal bare `TY_FN` pointer** -- a thin code pointer with nowhere to
   put an environment. Fat-normalization has retired it in PARAM position
   for concrete, tyvar, and effect-annotated signatures alike; the two
   carrier-side feeds -- a call THROUGH a rank-2/forall param
   (`elab_poly_call`) and the make-struct fn-field store -- are shimmed so
   an already-normalized param is not boxed a second time. The thin form
   survives only for cfnptr, variadic, and arity>5 signatures; passing a
   capturing closure into an *effect-annotated* such param is a call-site
   TUR-E0007. Effect-row call sites dispatch fat through the E2a twin
   registry (slot 0 = a registered capturing-lambda entry whose `__cps`
   twin takes the env, slot 1 = the fatshim box's stashed bare-fn direct
   entry), threadable capturing lambdas are CPS-admitted with the direct
   thunk's env-unpack preamble, and effect-row checking peels the
   `EX_FN_TO_FAT` shim
   (`poly-result-hof-capturing-closure-sigbus`, archived -- every row).

   The `{ shim, orig }` box a bare fn is shimmed into is `malloc`'d per
   execution of the bridge.  At a normalized nominal param nothing frees it
   -- a leak per call if allocated there -- but such a box is a constant when
   the boxed value is a global fn, so it is allocated once at file scope and
   filled from `__tur_static_init`.

   A `^fat` sink takes the same hoist when the callee provably neither
   retains nor drops the argument -- `nonretain_param_mask` bit set for that
   parameter.  This was thought impossible ("a `^fat` callee may drop its
   argument and the call site cannot tell"), and the missing fact was going
   to need an ownership annotation on the parameter.  It does not: dropping
   goes through `TUR_CLOSURE_DROP`, a C macro reachable only from an
   inline-C body, and a body containing any inline-C has
   `nonretain_param_mask == 0` by construction.  So the bit already means
   "neither retains nor drops".  A `^fat` sink WITH inline-C -- the shape
   that can drop -- has the bit clear and keeps its heap box, which is what
   `tests/fixtures/closure-drop-glue-fatshim` pins.
   ([`fat-sink-shim-box-leaks-per-call`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/fat-sink-shim-box-leaks-per-call.md),
   gated by `tests/run-fat-shim-leak.sh`.)

   Two predicates, both in `src/compiler/types.c`, and they deliberately
   disagree: `fn_param_type_is_fat_normalized` (param position, tyvars
   included) and `fn_result_type_is_fat_normalized` (declared result and
   nested result annotations, concrete only -- widening it double-boxes
   against the hrt-curried-result poly-call protocol, which boxes returned
   closures itself). `repr_of` routes on `REPR_POS_PARAM`, so the
   `--emit-abi-trace` output reports the same split.
5. **Struct-field-fat** -- `defstruct` fn-typed fields, normalized to the fat
   representation uniformly after the same bug was fixed there
   (`tests/fixtures/capturing-closure-struct-field/`).

### Slot 0's signature: which parameters are the word

Every fat box is called through slot 0, and the C type a caller casts slot 0
to must be exactly the type the entry stored there was defined at -- clang's
`-fsanitize=function` checks that, and WASM's `call_indirect` traps on any
difference.  So a parameter's **slot spelling** is one decision, made by
`thunk_param_slot_c_name` (`src/compiler/emit_module.c`), which every
typed-thunk typedef, hand-built fat-call cast, typed fatshim and adapter
consults:

| Parameter | Slot spelling | Why |
| --- | --- | --- |
| wide (> 8 byte) by-value aggregate | `int64_t` (a box pointer) | b4box: the thunk body loads it at entry |
| function value (not a cfnptr) | `int64_t` | every definition takes a fn parameter as the word (ER4), and every erased consumer calls at words |
| untyped `ptr<void>` | `int64_t` | **fnsan-ptr-void-fn-slot-word (2026-10-02).**  A program erases a closure to `ptr<void>` and calls it back as `(fn [ptr<void>] ...)`; for that call to be type-exact the two parameters must share one spelling, and the fn parameter's is the entrenched one |
| everything else | `type_c_name` | -- |

The `ptr<void>` row is a slot fact, not a definition fact: a closure thunk
still DEFINES the parameter `void *` (its body, forward, spec and CPS
declarations all say so), and slot 0 holds a converting `__tur_slot0_<thunk>`
entry (`ensure_closure_slot0_widen`, the same wrapper that widens a narrow
result).  A bare defn boxed into a fat value gets a typed fatshim that
converts the word back (`thunk_param_def_c_name` is the definition side).
Results are untouched: a `ptr<void>` result and a fat function result are
both `void *` already.  Runtime C that calls a closure's slot 0 by hand must
follow the table -- `httpd`'s handler dispatch is `void (*)(void *, int64_t)`,
`image/register-global!`'s deser is `TUR_APPLY1_T(int64_t, int64_t, ...)`,
and the reactor's callbacks were already all-word.  A typed `ptr<T>`,
`ptr<const-void>` and a cfnptr keep their real C types: they are FFI
spellings, not erasure.

Plus one in-flight form the minimization matrix in
`docs/archive/history/fn-typed-value-return-ascribe-miscompiles.md` exposed: the
**by-value fat struct** sitting in a parameter slot, whose return path
used to cast it thin (`return (int64_t)(intptr_t)v;` on an aggregate --
fixed by fat-normalization stage 2's poly-to-fat tail conversion).

## Boundaries

Each of these is a crossing where a bridge may be needed:

- function return / function parameter (typed `defn`)
- `let` binding / ascription `(:: e T)`
- generic (tyvar-typed) call argument and result
- typeclass method dispatch result (bare and dotted spellings)
- `Vec` element slot (push and get) -- **instrumented** (increment 4 stage
  3). Two mechanisms box an element here and they are easy to
  confuse: a nominal by-value ADT/struct element takes the box/deref bridge
  behind `type_is_boxed_container_elem`, while a concrete by-value *app*
  element (`(Vec (Option int))`) takes a monomorph-aware path -- malloc the
  monomorph on push, reconstruct field-by-field through the generic one-word
  box on read. Both really do box; only the first consults the predicate. The
  second is layout-safe because every parametric monomorph's payload occupies
  exactly one word.
- struct field store / load -- **instrumented and measured silent**
  (increment 4 stage 3). The declaration side of this boundary
  is one chokepoint, `adt_ctor_field_c_type`, which all nine field-emission
  sites route through; a `repr_of` shadow there found 84 disagreements and
  all 84 were one word spelled two ways, not seams. Worth knowing when you
  read a field slot: the owner, not the field type, picks the protocol. A
  by-value owner inlines a drop-glue-free aggregate field and **boxes** one
  that owns an rc/ref (so the owner stays trivially copyable); a carrier
  owner keeps every field as a one-word slot.
- closure capture and closure return
- inline-C body edge (always the raw carrier)

The pairing is the problem: ~4 data representations (6 for closures) times
each boundary kind, where every pair needs its own bridge and the bridges
are implemented point-by-point, not derived from one convention. Each open
report is one missing cell.

**Open cells.** These are the crossings that still have no working bridge;
each has a live report in `docs/reported/`. This table is the campaign's
index -- a repr cell with a filed report belongs here, so if you file one,
add the row.

| Open cell (producer -> boundary) | Report |
| --- | --- |
| `^Class`-constrained parameter (carrier `int64_t`) -> generated instance method taking a `defdata` ADT by value; instantiating at `int` is a no-op erasure and works | [`typeclass-constrained-param-erases-adt-to-int64`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/typeclass-constrained-param-erases-adt-to-int64.md) |

The table was empty between 2026-08-21 and these two filings. The pair that
emptied it closed on the same day, and the `let` merge-temp one is worth a
note because its filing had the arrow backwards. It was recorded here as
"CAPTURELESS closure (bare fn pointer) -> `let` merge temp decided
`fat-handle`", i.e. a thin producer reaching a fat boundary -- the same shape
as the archived `let-bound-noncapturing-lambda-segfaults-as-fn-arg`, which
really was that and was closed with the signature-keyed
`ensure_bare_fnptr_poly_shim` adapter. It was the opposite: the tail already
emitted a proper fat box and the merge TEMP was declared thin, so reusing that
adapter would have boxed an already-boxed value. The lesson for this table is
that a cell's *direction* is worth confirming against the emitted C before
pairing it with a neighbour's bridge -- the shadow line names the two forms but
not which side is the producer.

It was also not benign under `TUR_REPR_NO_SHADOW_ICE=1`, which is how it read
at filing time: that repro exits 0, but a variant that returns and CALLS the
closure emits `-Wint-conversion` on the temp assignment -- a hard error under
GCC >= 14. A loud-but-benign shadow is worth re-testing with the value actually
consumed before trusting the "benign".

File a new repr cell in this table as well as in `docs/reported/`.

**Closed cells (paper trail).** Bridges that now exist. Kept here because the
resolution notes say *which* bridge was added and what it is paired against --
the next cell in this family is usually adjacent to one of them.

| Closed cell (producer -> boundary) | Resolution | Report |
| --- | --- | --- |
| fn value reaching a `let` merge temp in RESULT position -- the TAIL emitted the fat `{ thunk, env... }` box while the TEMP was declared thin `R (*)(A...)`, so the assignment was `-Wint-conversion` (hard error under GCC >= 14) and the R3 shadow ICE'd on it | the two sites keyed fat-vs-thin off different facts -- `emit_temp_decl` off `type.as.fn.boxed` (a TYPE fact), stage-2 tail normalization off `fn_result_type_is_fat_normalized` (a POSITION fact). `merge_temp_fn_is_fat()` asks `repr_of` in RESULT position instead, and the decl and its ctype mirror both spell the temp from it | [`let-returning-noncapturing-lambda-ices-at-merge-temp`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/let-returning-noncapturing-lambda-ices-at-merge-temp.md) |
| bare-var tail of a NON-parametric by-value product -> `emit_if` merge temp (the parametric half was already bridged) | position-sensitive, not type-sensitive: `emit_arm_is_recorded_byval_agg()` asks the localvar side table what representation the arm's value actually has HERE, and suppresses the carrier->concrete bridge when it is already the aggregate. A TYPE-level widening regressed ten fixtures because the same type rides the carrier at the vec/map element and assoc-type seams; the recorded type differs there, so those keep their bridge. **SR1 (2026-08-26) added the case that side table cannot answer:** it records LOCALS, and a by-value aggregate PARAMETER is not one, so a sum-typed param returned from an arm (`re-repeat-n`'s `atom : Regex`) was bridged as though it were a carrier. `emit_arm_is_byval_agg_var()` is the second suppressor, and it IS the type-level test this row warns about -- kept safe only by being narrowed to by-value SUMS (the vec/map and assoc-type seams this row names are products) and gated on the seam. Widen it past sums and you should expect the ten fixtures back | [`byvalue-product-tail-var-double-unboxed-nonparametric`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/byvalue-product-tail-var-double-unboxed-nonparametric.md) |
| four carrier->concrete bridging sites each asking "does this value already HAVE the aggregate representation?" their own way -- the arm sites through a shared predicate, TWO let-binding init sites through separate inline copies of the same three comparisons, and the CPS `letraw` mirror not at all, while its comment claimed "same gate as the direct site" | one copy: `emit_value_is_recorded_as(v, want_ctype)`, taking the wanted C type as a STRING because that is what the binder sites hold; `emit_arm_is_recorded_byval_agg` is a thin Type-taking wrapper for the arm sites. The CPS mirror's missing term was added and is **provably inert** -- across all 2131 fixtures only 33 reach that bridge with a by-value init type, and in every one either the Expr-level predicate already suppresses it or the init is recorded as `int64_t`/pointer/nothing, never the aggregate. Emitted C byte-identical. A consistency repair, not a fix for an observed miscompile | [`cps-let-binder-bridge-lacks-position-check`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cps-let-binder-bridge-lacks-position-check.md) |
| a control form (`let`/`do`) wrapping an `if` whose arms are carrier producers -- each arm was bridged carrier->concrete by `emit_if`, then the ENCLOSING form bridged the already-concrete merge temp again (`operand of type 'tur_adt_...' where arithmetic or pointer type is required`). The same `if` as the whole function body was always fine | two halves. `bridge_control_value_to_byvalue_temp` (the do/let companion its own comment already named) gained `emit_arm_is_recorded_byval_agg`, the same one-predicate change that fixed the `emit_if` arms. But the precondition that fix assumed did NOT hold: `emit_if` declares its by-value merge temp with `emit_temp_decl` DIRECTLY, bypassing `emit_control_result_temp_decl`, which is the wrapper that records the temp's emitted C type -- so the temp was by-value but invisible to the side table the predicate consults. Recording it is the other half, and without it the predicate answers false and nothing changes | [`control-form-around-if-double-unboxes-carrier-arms`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/control-form-around-if-double-unboxes-carrier-arms.md) |
| `^mut` rebinding of a concrete heap container (merge-temp position) -- carrier where chokepoint 1 says typed pointer, travelling with a spec-materialization hole (a generic call in a `set!` RHS never interned its spec: LINK error past tur check) | chokepoint 1's concrete-heap rule extracted to `emit_repr_concrete_heap_ptr_c_name` and shared by the let-bind decl, the merge-temp decl, and its ctype mirror (the existing int<->ptr bridge reconciles a carrier tail); `emit_abi_scan_expr` gains its missing `EX_SET` case | [`mut-map-reassign-missing-spec-link-error`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/mut-map-reassign-missing-spec-link-error.md) |
| capturing closure -> nominal thin `TY_FN` param whose signature carries an **effect row** (the report's LAST row; concrete and tyvar signatures were already fat-normalized) | the CPS increment (2026-08-16): effect-annotated fn params join `fn_param_type_is_fat_normalized`; the E2a registry call sites dispatch fat (slot 0 = a registered capturing-lambda entry with an env-taking `__cps` twin, slot 1 = the fatshim's stashed bare-fn entry); threadable capturing lambdas are CPS-admitted with the direct thunk's env-unpack preamble; the effect_check walkers peel the shim. Capturing PERFORMING callbacks -- previously no working spelling -- thread the handler chain too. Thin remainder (cfnptr/variadic/arity>5 effectful) keeps a call-site TUR-E0007 | [`poly-result-hof-capturing-closure-sigbus`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/poly-result-hof-capturing-closure-sigbus.md) |
| generic closure return over a type application (struct `Cons`) -- the `(type-app ? ?)` shell at the checker AND the never-emitted `ctor_Cons` at link | Defect A: result-graft recovery at the thunk-type clobber in `elab_call.c` (the binding's own ground `result_full_type` survives the swap; the `elab_fns.c` grounding gate is untouched). Defect B: `inner_app` clone trigger + body-type-derived clone result + head-keyed clone resolution at the thunk direct-call, so the per-spec inner-closure clone is both emitted and the one actually invoked | [`generic-closure-return-type-app`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/generic-closure-return-type-app.md) |
| typeclass method result at **float** (any width) -> generic (carrier) call argument | producer bit-cast keyed on the method's DECLARED result kind (the same type the consumer keys its reinterpret on -- paired by construction); an int-declared method keeps its value conversion | [`method-result-float-spec-return-value-converts`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/method-result-float-spec-return-value-converts.md) |
| `float32`-ascribed literal -> any mixed C expression | ascribe elaborator retypes a float literal in place (`(:: 7.1 float32)` == `7.1f32`), so it emits at single precision instead of as the double literal the promotion rules then dominated | [`float32-ascribed-literal-compares-as-double`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/float32-ascribed-literal-compares-as-double.md) |
| `float32` generic (carrier) call result -> concrete consumer | elaboration: `call_wrap_reinterpret_owning` admits the carrier<->float32 pair (its silent mixed-size bail dropped the requested reinterpret, typing the call `int`); emit's size-mismatch reinterpret arm reads float pairs through the union overlay | [`float32-generic-call-result-printed-as-carrier`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/float32-generic-call-result-printed-as-carrier.md) |
| `TY_CONTRACT` in type-ARGUMENT position -- the payload kept a live contract type at every downstream boundary | peeled to its base in BOTH type-application loops (`rt_peel_type_arg_contract`), warning `TUR-W0380` that the payload predicate is not enforced; `TY_CONTRACT` then joined `type_has_concrete_codegen_layout` by delegating to its base | [`contract-type-arg-not-peeled-to-base`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/contract-type-arg-not-peeled-to-base.md) |
| method result (carrier) -> typed `(Result A B)` defn boundary | increment 2: continuation-wrapper ABI paired with the entry point dispatch actually selects | [`result-monad-bind-typed-boundary-miscompiles`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/result-monad-bind-typed-boundary-miscompiles.md) |
| by-value aggregate returned by a CAPTURING continuation -> int64 `tur_poly_fn_t.fn` carrier sink (nested `bind` / multi-step `do-m`) | signature-keyed fat spill shim: reads the real entry point out of the closure env's `__fn` slot and boxes the aggregate, the fat twin of the row above (which only covered named wrappers) | [`nested-bind-over-result-typed-boundary-segfaults`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/nested-bind-over-result-typed-boundary-segfaults.md) |
| method result (carrier) -> generic call argument | increment 2 | [`class-method-result-into-generic-invalid-c`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/class-method-result-into-generic-invalid-c.md) |
| by-value struct -> Vec / Map element slot | increment 3: width-independent boxed element protocol | [`vec-byvalue-struct-element-invalid-c`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/vec-byvalue-struct-element-invalid-c.md) |
| closure VALUE -> pass-through return / ascribe-around-let / nested fat HOF | fat-normalization stage 2 | [`fn-typed-value-return-ascribe-miscompiles`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fn-typed-value-return-ascribe-miscompiles.md) |
| fn value read out of a container element, then called | fat-normalization stage 2 | [`fn-payload-in-container-undeclared-temp`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fn-payload-in-container-undeclared-temp.md) |
| let-ALIASED carrier fn param in tail position; carrier vs boxed-result `if` unification | alias provenance in the tail walkers + poly-to-fat at the `if` join | [`fn-value-carrier-fat-seam-residuals`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/fn-value-carrier-fat-seam-residuals.md) |
| closure handle -> `double`-typed element slot | two-types-one-C-name collision, resolved upstream (both findings) | [`concrete-codegen-layout-kind-enumerations-drift`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/concrete-codegen-layout-kind-enumerations-drift.md) |
| carrier fn value -> monomorphized-ctor arg slot; carrier value -> pointer-typed fn return | stop re-deriving the emitted C type from a `Type`. The ctor's real param C type is recorded at ADT-app registration and looked up at the call site; the return site asks the typed AST whether the tail emits the carrier instead of sniffing the emitted string | [`macos-int-conversion-carrier-pointer-straddles`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/macos-int-conversion-carrier-pointer-straddles.md) |

A structural note the last closed row exposes: the representation decision today is
not one function but (at least) three hand-maintained `TypeKind` switches in
`src/compiler/types.c` -- `type_c_name`, `type_has_concrete_codegen_layout`
(fails closed: a missing kind silently falls back to the carrier), and
`append_type_mangle` -- and
codegen is correct only when all three agree. The triplication is removed
(increment 4 stage 1) for payload-free kinds: the
`TY_SIMPLE_REPR_ROWS` table in `types.c` carries one row per simple kind
with all three answers, and each switch expands the rows with its own
projection -- adding a kind without all three answers is a build failure,
not a silent drift. Payload-carrying kinds keep per-switch arms; two CI
guards ratchet the whole arrangement
(`tests/check-typekind-mangle-exhaustive.sh` parses the table + residual
arms and now also checks `type_c_name` exhaustiveness;
`tests/check-monomorph-name-collision.sh` reads what they emit). The
position axis -- one `repr-of(type, position)` routine for the per-SITE
choices -- is staged in
[docs/archive/repr-decision-function-plan.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/repr-decision-function-plan.md).

A strong diagnostic signal that a *bridge exists but is not consulted*: an
intervening `let` fixing the repro (verified for
`class-method-result-into-generic-invalid-c` -- the binding applies the
carrier->concrete bridge the direct composition skips).

## C-name accessors: which result may I hold?

Every representation in this guide has a **C spelling**, and the emitter reaches
it through an accessor returning `const char *`. Whether you may HOLD that
pointer across another call is part of the accessor's contract, and it is
invisible at the call site -- two accessors that look identical can differ.

The rule now, and it is uniform: **every C-name accessor returns a string that
is stable for the whole compilation.**

- `type_c_name` / `emit_type_c_name` intern every composed name via
  `intern_type_name` (`types.c`), so the pointer is stable and may be collected,
  stashed, and printed later.
- `adt_field_c_type`'s ROS pointer-box spelling (`"T *"`) interns too. It used
  to be a function-scoped `static char ptrbuf[128]`.
- `ensure_static_fatbox` returns an owned per-`EmitCtx` string
  (`ctx->fatbox_names[i]`), freed with the keys. It used to be a
  function-scoped `static char name[96]`.

Why this is worth a section rather than a comment: emitters routinely gather one
name per field or per parameter into an array and only then write the
declaration. Against a shared buffer every entry aliases it, so **every name is
the LAST name** -- and the result is well-formed C with the wrong type in it. No
crash, no ASan report, no compiler diagnostic. It surfaces downstream as a
`-Wincompatible-pointer-types` at some unrelated call site, or not at all.

The tree has been bitten twice: `EmitSigEntry.ret_ctype` handed out an interior
pointer callers held across further emission (43 fixtures, caught by ASan), and
`adt_field_c_type` mistyped a `(Result Rational ArithError)` monomorph's
`ok_val` as the error arm. The second was found by a representation change, not
by a test -- which is the point. **A by-value sum makes "two pointer-boxed
fields on one constructor" ordinary**, so consolidating a representation is
exactly what takes one of these latent and makes it live.

Two guards:

- `tests/check-static-cname-buffers.sh` (ctest `tur_static_cname_buffer_lint`)
  fails on any `const char *` function in `src/compiler/` holding a
  function-scoped `static char buf[]`, with the four audited-benign sites
  allowlisted by name. This is the one that catches the NEXT instance.
- `tests/fixtures/ros-pointer-box-distinct-arms/` pins the two-distinct-arms
  shape at runtime; `run.sh` fails a fixture whose cc emits
  `-Wincompatible-pointer-types`, so a regression is loud.

Resolved report:
[docs/archive/c-name-accessors-share-static-buffers.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/c-name-accessors-share-static-buffers.md).

## Finding more missing cells

`tests/type-fuzz-src.py` walks this matrix mechanically: it generates
correct-by-construction programs routing known values through random
wrapper x boundary compositions and asserts check-accepted implies
compiles + links + runs + prints the predicted output. Shapes reproducing
the open cells above are excluded via its `known_bug_slug` table and
pinned by `--known-probes` instead, so a red run means a NEW cell.

### Two kinds of boundary, and only one used to be fuzzed

The wrapper x boundary compositions above are all boundaries the **compiler owns
both ends of**: it emits the producer and the consumer in the same pass, and the
value never leaves its static type. A **runtime seam** is the other kind -- the
payload is parked in a runtime data structure (a session channel, a protocol
router, a generator frame, a future slot, a fiber slot, a transactional cell) by
one piece of emitted code and read back later by another. The slot has one C
type, so the payload is cast in and out, and a plain C cast of a `double` is a
value conversion that truncates.

That is a distinct column of this matrix, and it went unfuzzed while `float` was
a first-class axis everywhere else -- which is how three seams shipped carrying
every payload as a bare `int64_t`. `--seam-frac` (default 0.30) routes a share of
cases through a seam instead of a crossing, `--seam <name>` isolates one, and
`--seam-matrix` prints the deterministic seam x payload verdict table, which is
how a fix here is verified.

Two properties of that axis are worth knowing before extending it. Its oracle
**prints** the payload rather than comparing it against the generator's literal:
a seam that erases its payload to `int` makes `(= v 7.25)` a `TUR-E0042` reject,
and a reject is not a failure, so a comparing oracle files the defect as "the
generator emitted an illegal program" and stays green. And a seam reject is
classified apart from a generator reject (`SEAM_REJECT`), because for a seam it
means the checker refused the payload -- a feature-surface hole, reported rather
than failed. Three seams that are already correct (`perform`/`resume`,
`any`/`cast`, `tvar`) are generated deliberately as positive controls, so a run
where every seam fails is distinguishable from a broken emitter.

Open seam cells:
[router-payloads-are-int64-only](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/router-payloads-are-int64-only.md),
[generator-yield-payload-is-int64-only](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/generator-yield-payload-is-int64-only.md),
[async-await-payload-is-int64-only](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/async-await-payload-is-int64-only.md).
The fix convention to reuse rather than reinvent is the `union { double d;
int64_t i; }` bit-reinterpret the direct/fiber effect path already ships
([fiber-effect-float-result-truncated](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/fiber-effect-float-result-truncated.md)),
or `any`'s approach of not riding the slot at all.

The convention-level fix for the closure rows -- normalize every non-carrier
fn boundary onto the fat protocol instead of deciding representation
per-boundary -- is complete; see
`docs/archive/fn-value-fat-normalization-plan.md`. The campaign-level
strategy governing that plan and its successors (which seams consolidate in
which order, the probe/blast-radius discipline, and the performance
guardrails) is `docs/archive/representation-consolidation-meta-plan.md`;
this guide's open-cells table is that campaign's live scoreboard, and the
closed-cells table below it is the record of what the campaign has already
consolidated.

## Detecting the mechanism, not the shape

Everything above finds a defect by **reaching its shape**: a generator
composes wrappers and boundaries, and a finding is a composition nobody had
written. That is how this class was found for two years, one seam at a time --
and it is why it kept coming back: the next defect is always in a shape the
generators do not produce (a class method calling a fn-valued extra, a
function stored in a struct field, a `float32` in an `any`). Three checks now
look at the MECHANISM instead, whatever shape reaches it.

**Float<->int value conversions in emitted C**
(`tests/check-emitted-float-conversions.py`, ctest
`tur_emitted_float_conversions`, and inside both source fuzzers as
`BUG_value_conversion`). `-Wfloat-conversion` sees implicit conversions only;
the emitter spells a carrier crossing as an explicit `(int64_t)(d)`, which
never warns. clang's AST records every conversion, so the check reports each
`FloatingToIntegral` / `IntegralToFloating` in emitted program code that is
none of: an `as` the program asked for (spelled `TUR_AS(T, x)`, a preamble
macro), an exact integer literal, or hand-written inline C (a `defn` body
named in the trailing `/* tur:inline-c-fns: ... */` list, or a file-scope
block between `/* tur:inline-c-begin/end */`). **The corpus is at zero** (2512
programs), so every finding is new. Its first corpus run found a live one: a
`float32` widened to `any` was stored by value conversion (2.5 came back as
2) under a tag `type-of` could not name. When you add a DELIBERATE conversion
to the emitter, spell it `TUR_AS`; anything else it reports is the bug.

**Mismatched indirect calls** (clang `-fsanitize=function` in trap mode:
`tests/run-fnsan.sh` over the fixture corpus, the `fnsan` CI job, and
`tests/fuzz_arm.py` in all four source fuzzers -- each proven armed by a
canary first). It sees a call through a function pointer whose type disagrees
with the callee's definition -- the thin/fat, int64/double and
int64/tagged-`any` confusions of the fn-value rows above. It is exact, so it
also traps on ABI-benign `bool`/pointer vs `int64_t` mismatches. **The corpus
is at zero** (2026-10-02, after the slot-0 decision in "Slot 0's signature"
above), so it gates: a fixture trap fails the `fnsan` job and a fuzzer trap is
the failing `BUG_fnptr_trap` (`TUR_FUZZ_FNSAN_STRICT=0` makes it report-only
for a session chasing something else). The sweep's history is in
[emitted-c-indirect-calls-are-not-type-exact](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/emitted-c-indirect-calls-are-not-type-exact.md).

**An unresolved type tag** (runtime). A value widened to `any` while its type
is still a type VARIABLE used to be tagged with the bare `TY_TYVAR` kind, a
box no consumer understands (`type-of` "unknown", `is?` false, a silent wrong
branch). It now gets a reserved tag and `TUR_TAG` traps on it with an internal
error, so an erased body that should have been specialized fails loudly where
the value was widened.

## Maintenance -- keep this guide truthful

This guide is load-bearing for triage; a stale representation inventory is
worse than none. Every report in the table above carries a closing task
pointing back here. If you:

- add, remove, or merge a **representation** (e.g. the fat-normalization
  plan collapsing the closure zoo),
- add or fix a **bridge** (a cell in the matrix), or
- resolve one of the linked **reports**,

then update the corresponding section here in the same PR: fix the
inventory, move the cell from the open-cells table down into the
closed-cells table with a one-line resolution note, and note the new
invariant. When a report is archived, update its row's link to
`docs/archive/`.
