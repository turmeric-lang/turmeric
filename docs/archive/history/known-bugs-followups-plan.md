# Known-Bugs Follow-ups Plan

This plan tracks the remaining open issues that were carried in the (now
archived) `docs/archive/history/known-bugs.md` log.  Each section describes the problem,
the confirmed root cause, the proposed fix, the affected fixtures, and an
effort estimate.

The historical bug log (with the full list of already-fixed entries) lives
at [../archive/history/known-bugs.md](../archive/history/known-bugs.md).

All items below were re-verified against the build at the time this plan was
written; the affected fixtures originally failed under `tests/run.sh` (run with
`ASAN_OPTIONS=detect_leaks=0`, matching CI).

**Status: all items are now resolved (DONE).**  Each section below records the
implemented resolution, and the whole `tests/run-stdlib-checks.sh` allowlist
(including `typeclass.tur`) now passes.

## Status overview

| ID | Title | Severity | Effort |
|----|-------|----------|--------|
| KB-021 | Typeclass dispatch ABI mismatch for struct-typed instances -- DONE | High | Large |
| KB-022 | GADT HKT constraint unification (`equal-cong`) -- DONE | Low | Medium |
| KB-025 | GADT skolem-escape check missing -- DONE | Medium | Medium |
| KB-026 | Implicit-tyvar acceptance suppresses intended diagnostics -- DONE | Medium | Medium |
| KB-027 | `stdlib/rc.tur` Functor on `ptr<void>` (kind error) -- DONE | Low | Medium |
| KB-029 | `stdlib/session.tur` tuple return-type syntax -- DONE | Low | Medium |
| KB-030 | Orphan-instance checker rejects instances on built-ins -- DONE | Medium | Medium |
| KB-034 | Calling a `:ptr<void>` value with 2+ args segfaults -- DONE | Medium | Medium |

---

## KB-021 -- Typeclass dispatch ABI mismatch for struct-typed instances

**Severity:** High (blocks typed-collection equality dispatch).

### Symptom

Calling a typeclass method on a struct instance fails to compile: the vtable
stores a carrier-ABI function (`int64_t` params) but the callsite casts the
vtable entry to a by-value signature (`bool (*)(StructType, StructType)`), or
vice-versa.  `result-of-typed-eq` shows the related "incompatible type for
argument 1 of `vec_push_` / `ok_` / `result_eq_`" form.

### Affected fixtures

`ptc4-basic`, `map-of-tvec-eq`, `option-of-tvec-eq`, `result-of-typed-eq`,
`set-of-tvec-eq`, `vec-of-tvec-eq`, `vec-eq-ascribed`, `vec-eq-ascribed-multi`.

### Root cause

Two calling conventions coexist for typeclass dispatch:

1. **Carrier ABI** -- instances store/call via `int64_t`; the body casts the
   carrier to a struct pointer.
2. **By-value ABI** -- the callsite casts the vtable entry to
   `bool (*)(StructType, StructType)` and passes the struct directly.

`type_uses_carrier_in_dispatch` (`src/compiler/emit_expr.c:17`) is the current
arbiter: it selects carrier for parametric structs (`n_type_params > 0`),
`TY_APP`, and `TY_ADT`, and by-value otherwise.  The ACB phases (3-5) and
KB-031 made many cases consistent, but the typed-collection-of-typed-struct
dispatch (`Eq [Vec]`, `Eq [Map]`, etc.) still has a side that disagrees.

### Proposed fix

Pick one ABI for *all* struct-valued typeclass dispatch and make the
instance-body codegen (`elab_typeclasses.c`) and the dictionary callsite
(`emit_expr.c`, `EX_CALL` / `fn_expr` and the `dict_*_singleton.method` path
near line 831) agree on it.  Recommendation: standardise on the **carrier
ABI** for dictionary-dispatched methods (keeps the vtable uniform and matches
the parametric path that already works); ensure the by-value callsites for
non-parametric structs are bridged to carrier before the call.  Add a single
predicate that both sides consult, replacing the current asymmetric checks.

Validate by removing the (carrier vs by-value) divergence one fixture family
at a time, checking `expected.stdout` and regenerating any `expected.c`
snapshots.

### Effort

Large -- touches the dispatch ABI contract; needs careful snapshot review.

### Resolution (DONE)

Carrier-ABI types (`TY_APP`, `TY_ADT`, parametric structs) have two coexisting
C representations: the `int64_t` carrier (a heap-pointer handle returned by
carrier-ABI stdlib functions like `(vec-new)` / `(some x)`) and a by-value
concrete struct (a struct constructor literal, or an ABI-specialized clone
returning the concrete type, e.g. `(tuple2 a b)`).  Function signatures and
dictionary dispatch use the carrier, but value-position `let` bindings declared
the concrete struct -- so an ascribed `(:: (vec-new) (Vec int))` local became a
by-value `Vec__int` while `vec-push!` and `.eq?` dispatch expected the carrier,
producing "incompatible type for argument 1 of `vec_push_` / `ok_` /
`result_eq_`".

The fix standardises on a single shared arbiter, `type_uses_carrier_abi`, that
both the declaration path and the dispatch callsites consult:

- A `let` binding now declares the C representation its initialiser actually
  yields (`emit_binding_repr_c_name`): the `int64_t` carrier for a
  carrier-returning call or carrier var, the concrete struct for a struct
  literal, a by-value var, or an ABI-specialized concrete result.
- The dictionary-dispatch callsites bridge concrete->carrier only for genuinely
  by-value producers (struct literals, by-value vars/params, concrete ABI-spec
  results), tracked via a new `Binding.emit_byvalue_carrier_abi` flag set at each
  declaration site; an already-carrier value passes through unbridged.
- Type ascription to a carrier-ABI target keeps the carrier representation
  instead of dereferencing it into a by-value copy.
- The ABI-spec lookup is factored into `find_matched_abi_spec`, reused by both
  the call emit path and the binding-representation decision.

Also fixed a latent typo in two affected fixtures (`some?`/`ok?` predicates used
where the `some`/`ok` constructors were intended); the prior build failure had
masked it.

Verified: all eight fixtures (`ptc4-basic`, `vec-eq-ascribed`,
`vec-eq-ascribed-multi`, `map-of-tvec-eq`, `option-of-tvec-eq`,
`result-of-typed-eq`, `set-of-tvec-eq`, `vec-of-tvec-eq`) pass under
`tests/run.sh`.

---

## KB-022 -- GADT HKT constraint unification (`equal-cong`)

**Severity:** Low (one advanced GADT fixture).

### Symptom

```
error [TUR-E0001]: function 'equal-cong' arg 1: expected
  (type-app (type-app Equal tyvar) tyvar), got Equal
```

`equal-cong` has a kind-polymorphic parameter `^f` and takes `eq : (Equal a b)`.
At the callsite `(Refl)` has type `(Equal a a)` (one variable), but the checker
expects `(Equal a b)` with two distinct variables.

### Affected fixtures

`gadt-equal-cong` (needs `-Xgadt`).

### Root cause

HKT kind inference for `^f` constrains the `Equal` type arguments differently
from the monomorphic `Refl` constructor; unification fails to match
`(Equal a a)` against `(Equal a b)` when a kind-variable annotation is present.

### Proposed fix

Investigate HKT constraint unification for GADT constructors where the same
type variable appears in multiple positions of the constructor's return type.
The unifier should allow `(Equal a a)` to refine `(Equal a b)` by binding
`b := a` rather than rejecting on arity/shape.

### Effort

Medium -- isolated to the GADT/HKT unifier, but type-system work.

### Resolution (DONE)

Two coordinated fixes, both small:

1. **Unifier (`elab_call.c`, `call_collect_type_bindings`)** -- a bare
   GADT/ADT value (`TY_ADT`) is now accepted as an argument for a
   parameterised parameter type (`TY_APP`) when their heads agree. `(Refl) :
   Equal` passed where `(Equal a b)` is expected matches on the `Equal` head
   and leaves the named tyvars `a`/`b` unbound (the parameter is polymorphic,
   so any instantiation is sound), instead of rejecting on the
   `TY_APP`-vs-`TY_ADT` shape mismatch.

2. **Codegen (`emit_module.c`, `emit_abi_fn_skip_generic`)** -- `equal-cong`'s
   result type permanently mentions the unbound kind variable `f` (`(Equal (f
   a) (f b))`), so it can never be ABI-monomorphized and no specialization
   clone is ever produced. The generic-unsafe suppression that normally
   defers such functions to per-callsite clones now tracks whether a direct
   (non-specialized) carrier call to the binding was observed during the ABI
   scan, and emits the carrier definition in that case so the call resolves.
   Every type in `equal-cong`'s signature lowers to the `int64_t` carrier, so
   the single carrier definition is valid C.

Verified: `gadt-equal-cong` passes; the full `tests/run.sh` suite shows no new
failures (the remaining `gadt-refine-escape`, `kinds-kind-variable`, and
`typeann-diag-hint` failures are the still-open KB-025/KB-026 items).

---

## KB-025 -- GADT skolem-escape check missing

**Severity:** Medium (soundness gap; a negative fixture cannot pass).

### Symptom

`errors/gadt-refine-escape` (with `-Xgadt`) expects the diagnostic
`skolem type variable escapes match arm`, but the program compiles cleanly
(exit 0).

### Root cause

When a GADT match arm binds a value to the GADT's phantom type variable and
that value escapes through a concrete (`:int`) return annotation, no
skolem-escape check is performed.

### Proposed fix

Add a skolem-escape check after GADT match-arm elaboration (in the GADT match
path of `elab_forms.c`): if the type of any match-arm result contains an
unresolved skolem variable that is not bound by the surrounding function's
type signature, emit `skolem type variable escapes match arm`.

### Effort

Medium -- requires tracking skolem provenance through match-arm result types.

### Resolution (DONE)

Two coordinated fixes:

1. **Signature-tyvar tracking (`elab_fns.c`)** -- `elab_defn` and the `fn`
   elaborator now accumulate the named type variables that appear in the
   enclosing function's signature (parameter types + return type) into a new
   `Elab.sig_tyvars` set (additive across nesting, so closures see their own
   plus the outer function's). This is the authoritative answer to "is `a`
   bound by the surrounding function's type signature?".

2. **Escape check + field refinement (`elab_structs.c`)** -- the GADT match
   path now:
   - refines a type-variable field via the *scrutinee's* instantiation when
     the constructor's return annotation does not pin it to a concrete kind
     (matching a `(Box t)` binds the `a` field to `t`), so a properly
     polymorphic arm yields the function's own signature variable rather than
     the GADT's internal parameter name; and
   - widens the skolem-escape diagnostic: it fires for any arm-body result of
     kind `TY_TYVAR` that is either anonymous or a named variable absent from
     `sig_tyvars`. `(defn my-unbox [b] :int (match b (MkBox x) x))` -- where
     `x : a` escapes through the concrete `:int` return -- is now rejected with
     `skolem type variable escapes match arm`.

The field refinement (1->2a) is what keeps the legitimate polymorphic forms
(`[b : (Box a)] : a` and the differently-named `[b : (Box t)] : t`) compiling
while the genuinely-unsound form is rejected.

Verified: `errors/gadt-refine-escape` now produces the expected diagnostic; the
full `tests/run.sh` suite passes except for the still-open KB-026 fixtures
(`kinds-kind-variable`, `typeann-diag-hint`).

---

## KB-026 -- Implicit-tyvar acceptance suppresses intended diagnostics

**Severity:** Medium (UX/soundness regression; two negative fixtures fail).

### Symptom

Two negative fixtures fail because diagnostics they expect no longer fire:

- `errors/kinds-kind-variable` expects `unsupported return type keyword` for a
  `:a` return type; it now compiles cleanly.
- `errors/typeann-diag-hint` expects the helpful hint
  `parameter 'n' looks like it was followed by a type annotation; use
  [n :Type] or [n : Type]` when a parameter is written `[n : nope]`; it now
  emits a confusing `operator lookup failed for '*'` instead.

### Root cause

The GS4 compatibility rule (`src/compiler/elab_fns.c:1070-1077`) makes any
unknown lowercase type keyword (return type *or* parameter type) resolve to a
named type variable (`TY_TYVAR`) instead of an error.  This intentionally
enables generic binder forms (`:a`, `: a`) but, as a side effect, it
swallowed the two diagnostics above and lets `[n : nope]` silently become a
type-variable parameter.

### Proposed fix

Decide the intended boundary between "implicit type variable" and "typo /
unsupported type", then make the two consistent.  Options:

1. **Context-gated tyvars** -- only treat an unknown lowercase keyword as a
   type variable when it also appears in a binder position the function
   actually quantifies over (e.g. it was introduced by a `forall`/type-param
   list or appears in a parameter type).  A bare `:a` return with no
   corresponding binder reverts to the `unsupported return type keyword`
   diagnostic, and a lone `[n : nope]` reverts to the type-annotation hint.
2. **Accept and update fixtures** -- if implicit tyvars are intentional
   everywhere, retarget both fixtures: move `kinds-kind-variable` to a
   happy-path fixture and drop / rewrite `typeann-diag-hint`.  This is lower
   effort but loses a deliberate UX hint.

Recommendation: option 1, preserving the diagnostics.

### Effort

Medium -- the parameter and return-type paths in `elab_fns.c` both need the
gating logic; option 2 is small but degrades UX.

### Resolution (DONE)

Implemented option 1 (context-gated tyvars) as a post-signature pass in
`elab_defn` (after params + return type are parsed, before body elaboration),
plus two helpers in `elab_fns.c`:

- `fn_type_mentions_named` -- does a type mention a given named tyvar?
- `fn_name_is_adt_tyvar` -- is a name the declared type parameter of some
  in-scope ADT/struct (e.g. `a` from `(defgadt Witness [a] ...)`)?

The gate distinguishes a genuine type variable from a typo:

- **Bare-keyword parameter** (`[n : nope]`): kept as a type variable when the
  name is declared in the function's type-param / kind-var list, is an
  ADT/struct type parameter, or relates >=2 signature type positions
  (appears in another param type or the return).  Otherwise it is a typo and
  is demoted to the unresolved opaque type (`TY_STRUCT` with NULL def), which
  restores the `use-after-move` + "parameter looks like it was followed by a
  type annotation" hint when the binding is misused.  The ADT-type-param
  exemption is what keeps GADT-refined params such as `[w :Witness v :a] :int`
  working (`a` is `Witness`'s parameter, refined per match arm).
- **Bare return type variable** (`:a`): kept only when it is declared or
  appears in a parameter type (the binder it would be quantified by).  A `:a`
  return with no such binder -- as in `(defn map [^f a x] :a x)` -- now emits
  `unsupported return type keyword`.  (The ADT-type-param exemption is
  deliberately *not* applied here, so a bare `:a` return stays an error even
  though stdlib ADTs use `a`.)

Verified: both `errors/kinds-kind-variable` and `errors/typeann-diag-hint`
produce their expected diagnostics; the legitimate generic forms
(`[x : a] : a`, explicit `[a] [x : a] : a`, and GADT refinement in
`gadt-refine-witness`) still compile; the full `tests/run.sh` suite is green.

---

## KB-027 -- `stdlib/rc.tur` Functor on `ptr<void>` (kind error)

**Severity:** Low (file not auto-loaded; not in the stdlib-check allowlist).

### Symptom

```
$ ./build/tur check stdlib/rc.tur
stdlib/rc.tur:59:1: error [TUR-E0012]: kind mismatch: typeclass 'Functor'
  parameter 1 expects kind '* -> *', but 'ptr<void>' has kind '*'
stdlib/rc.tur:104:14: error: typeclass 'Foldable' is not defined
```

### Root cause

1. `(definstance Functor [ptr<void>])` is a kind error -- `Functor` needs a
   type constructor of kind `* -> *`, but `ptr<void>` is kind `*` (a v1
   approximation acknowledged in a source comment).
2. `Foldable` / `Traversable` live in `stdlib/typeclass.tur`, which is not
   auto-loaded, so they are out of scope for a standalone `tur check`.

### Proposed fix

Replace the `ptr<void>` instances with a newtype wrapper that is a real type
constructor of kind `* -> *` (e.g. `(defstruct Rc [inner :ptr<void>])` used as
`Rc a`).  Once the kind is correct, express `Functor`/`Foldable`/`Traversable`
without approximation.  Alternatively, add `typeclass.tur` to the auto-load
list -- but only after resolving its Functor/Applicative/Monad overlap with
`typeclass-functor.tur`.  Then add `stdlib/rc.tur` to
`tests/run-stdlib-checks.sh`.

### Effort

Medium -- stdlib redesign of the Rc type constructor.

### Resolution (DONE)

Resolved by dispatching on the built-in `rc<T>` type constructor instead of
`ptr<void>`, building on the KB-030 orphan-checker change:

1. **Dispatch on `rc`, not `ptr<void>` (`stdlib/rc.tur`)** -- `Functor`,
   `Foldable`, and `Clone` instances now target the built-in `rc` constructor.
   `rc` resolves to `TY_RC`, which the kind system treats as kind `* -> *`, so
   the `Functor`/`Foldable` instances are well-kinded (no more "expects kind
   '* -> *', but 'ptr<void>' has kind '*'").  The redundant `ptr<void>` mirror
   of the `Clone` instance (and its `__clone_rc_shallow` / `__foldable_rc_*`
   helpers) was removed; the `Foldable` bodies now inline the same C the
   `Functor` instance uses.
2. **`TY_RC`/`TY_WEAK` kind (`kind_check.c`)** -- `type_effective_kind` now
   reports `KIND_ARROW` for `TY_RC`/`TY_WEAK` so the secondary (belt-and-
   suspenders) kind validation agrees with the elab-time check that these are
   type constructors.  The bottom-up kind inference additionally skips the
   `STAR -> ARROW` promotion for `rc`/`weak` args, so a `STAR`-declared class
   used as a concrete carrier handle (`Clone [rc]`) is not mistakenly lifted to
   higher kind.
3. **Local `Foldable` class stub (`stdlib/rc.tur`)** -- `Foldable` is not
   auto-loaded (unlike `Functor`/`Clone`), so rc.tur declares the class locally
   with the same signature as `stdlib/typeclass.tur` (accepted by the
   idempotent-redefinition rule).  This keeps rc.tur standalone-checkable
   without a global auto-load that would have changed the `Foldable` ownership
   seen by existing HKT fixtures.
4. **Orphan ownership** -- with KB-030's built-in-home registry extended to map
   `TY_RC`/`TY_WEAK` (and the `rc`/`weak` names) to `rc.tur`, all three
   instances are credited to their home module.

`stdlib/rc.tur` now type-checks standalone and was added to
`tests/run-stdlib-checks.sh` (29 passed).  The full `tests/run.sh` (1025) and
`tests/run-turi.sh` suites remain green.

Note: `stdlib/docstrings.tur` (auto-generated) still carries stale entries for
the removed helpers; it was already drifted from the current stdlib, so a full
`just docs` regeneration is left as separate housekeeping rather than mixing
unrelated doc churn into this fix.

---

## KB-029 -- `stdlib/session.tur` tuple return-type syntax

**Severity:** Low (session file not in the stdlib-check allowlist).

### Symptom

```
$ ./build/tur check -Xsessions stdlib/session.tur
stdlib/session.tur:70: error: unsupported type expression form
  (expected symbol, keyword, or list)   ... :[(int (Session ...))]
```

The KB-019 session-kind blocker is resolved; the remaining failure is a tuple
return-type annotation `:[(int (Session ...))]`.

### Root cause

The type-annotation elaborator (`src/compiler/elab_types.c:1370`) does not
accept the `:[(...)]` tuple-return-type form.

### Proposed fix

Either (a) teach the elaborator to accept `:[(...)]` as a tuple type
(lowering to the `Tuple2`/`TupleN` stdlib structs), or (b) rewrite the
affected `session.tur` signatures to return a named tuple struct.  Option (b)
is the lower-risk, self-contained path: it closes the bug without a compiler
change and lets `stdlib/session.tur` join `tests/run-stdlib-checks.sh`.

### Effort

Medium -- (a) is a parser/elaborator feature; (b) is a contained stdlib edit.

### Resolution (DONE)

Took the contained-stdlib path, slightly refined.  The single offending
signature was `echo-client-call`, whose body is `(recv ch)` -- and `(recv ch)`
returns the *internal* session recv-pair (`TY_SESSION_RECV_PAIR`,
`[echoed-int, updated-Session]`), which is destructured specially by
`(let [[v ch] (recv ch)] ...)` and has no surface type syntax.  Neither a
`TupleN` lowering (option a) nor a named-tuple struct (option b) matches what
the body actually yields, so the return-type annotation was simply dropped and
left to inference: the inferred type is precisely the recv-pair the body
produces and that callers already destructure.  The behaviour-documenting
docstring is unchanged.

`stdlib/session.tur` now checks clean under `-Xsessions` and was added to
`tests/run-stdlib-checks.sh` (30 passed).  No compiler change was required; the
`:[(...)]` parser gap noted in the original plan remained an unimplemented
feature but is no longer on any stdlib file's path.

### Follow-up: residual parser-surface gap CLOSED (2026-06-04)

The general tuple-type surface gap is now closed. Option (a) shipped, refined:
in type-annotation position a bracket list `[T1 T2 ... Tn]` (2 <= N <= 8) is
sugar for the stdlib `TupleN` struct application `(TupleN T1 ... Tn)`,
mirroring the value-level tuple destructure `(let [[a b] e] ...)`. It nests
recursively and reports a clear diagnostic for out-of-range arities. Lowering
lives in `src/compiler/elab_types.c` (the `F_VEC` branch of
`type_expr_from_form`); covered by `tests/fixtures/tuple-type-bracket-sugar`
and `tests/fixtures/errors/tuple-type-bad-arity`.

This does **not** change `session.tur`: `(recv ch)` still yields the internal
`TY_SESSION_RECV_PAIR`, a type distinct from `Tuple2`, so `[int (Session ...)]`
would denote the wrong type. Session recv-pairs still have no surface spelling
and that signature stays inferred. Two pre-existing defects surfaced while
landing the sugar and are filed separately (not regressions):
`docs/reported/tuplen-struct-param-passed-by-pointer-codegen-mismatch.md` and
`docs/reported/polymorphic-return-type-instantiation-collapses-to-first-tyvar.md`.

---

## KB-030 -- Orphan-instance checker rejects instances on built-ins

**Severity:** Medium (blocks standalone `tur check` of several stdlib files).

### Symptom

```
$ ./build/tur check stdlib/str.tur
stdlib/str.tur:114:1: error [TUR-E0013]: orphan instance: typeclass 'Eq' is
  defined in a different module and none of the type arguments belong to this
  module ...   (definstance Eq [str] ...)
```

The same shape blocks `Ord [str]`, `Show [str]`, `Clone [str]`, and was the
root cause behind the `clone-vec` / `backtrack-clone-*` fixtures (instances on
the built-in `int` / `vec`).  Those fixtures have been worked around by
renaming their local class to `TestClone`; the underlying checker limitation
remains.

### Root cause

The orphan check (`src/compiler/elab_typeclasses.c:1868-1888`) only credits a
type argument as "owned by this module" when it is a `TY_STRUCT` with a
matching `origin_file_id`.  Built-in primitives (`str`, `cstr`, `int`, `bool`,
...) have no `defstruct` and therefore no owning file, so any instance for
them outside the typeclass-defining file is flagged orphan.

### Proposed fix

Give the orphan checker a notion of "the designated home file for a built-in
primitive type" (a small table mapping built-in `TypeKind` -> stdlib file), and
treat an instance as non-orphan when one of its type args is a built-in whose
home file is the current file.  Then move the primitive instances to their
designated homes (e.g. `Eq [str]` in `typeclass-eq.tur` or a `str`-home file)
and add the affected files to `tests/run-stdlib-checks.sh`.

Lower-effort alternative (no compiler change): move `Eq [str]`, `Ord [str]`,
`Show [str]`, `Clone [str]` into the files that define those typeclasses,
satisfying the existing rule -- but this fights the natural module layout and
risks load-order issues, so the checker change is preferred.

### Effort

Medium -- orphan checker change plus a built-in-home registry.

### Resolution (DONE)

Implemented the preferred path (a built-in-home registry consulted by the
orphan checker) plus the kind-inference fix that the change exposed:

1. **Built-in-home registry (`elab_typeclasses.c`)** -- a built-in primitive
   that resolves to an opaque `TY_STRUCT` (`def == NULL`, e.g. `str`, `rc`,
   `weak`) has no `origin_file_id`, so it could never satisfy the existing
   ownership rule.  `builtin_type_home_basename` maps each such name to its
   designated home stdlib file (`str -> str.tur`, `rc`/`weak -> rc.tur`), and
   the orphan check now also credits a type arg whose built-in home basename
   matches the current file's basename (via `diag_file_path` +
   `tc_path_basename`).  `(definstance Eq [str] ...)` in `stdlib/str.tur` is
   therefore no longer flagged orphan.

2. **Kind-inference fix (`kind_check.c`, `kind_infer_from_instances`)** --
   removing the orphan error unmasked a latent problem: an opaque struct
   reference reports `type_effective_kind == KIND_ARROW` (the right answer for
   a genuine HKT constructor like `option`/`vec`, validated separately against
   an explicit `^f`), which the bottom-up inference used to *promote* a
   STAR-declared class such as `Eq [a]` to `* -> *` -- breaking every concrete
   `Eq [int]` / `Eq [bool]` instance.  The inference now skips the STAR->ARROW
   upgrade for opaque-struct args; real constructor args (`TY_APP`, `TY_REC`)
   still drive inference, and the explicit-`^f` validation path is unchanged.

`stdlib/str.tur` now type-checks standalone and was added to
`tests/run-stdlib-checks.sh`.  The full `tests/run.sh` (1025) and
`tests/run-turi.sh` suites remain green.

Follow-up folded in: `stdlib/typeclass.tur` previously still failed its
standalone check because of orphan `Clone [int]` / `Clone [bool]` / `Clone
[cstr]` (and the sized-numeric) instances.  These primitives resolve to
concrete `TY_*` kinds with no `type_arg_syms` entry, so they are handled by the
kind-keyed `builtin_kind_home_basename` table instead: the bare scalar
primitives (`int`, `bool`, `cstr`, and the sized numeric kinds) home to
`typeclass.tur`, the comprehensive typeclass module where their canonical
instances live (data types such as `rc`/`weak` continue to home to their own
module).  The mapping is consulted only in the orphan-check fallback and is
purely permissive, so it credits these instances in `typeclass.tur` without
relaxing the check anywhere else.  The full `tests/run-stdlib-checks.sh`
allowlist now passes.

---

## KB-034 -- Calling a `:ptr<void>` value with 2+ args segfaults

**Severity:** Medium (limits higher-order combinators).

### Symptom

```turmeric
(defn call2 [f : ptr<void> a : int b : int] : int (f a b))
(defn add  [a : int b : int] : int (+ a b))
(call2 add 10 20)   ; => Segmentation fault
```

One-arg `:ptr<void>` calls work when `f` is a partial-application closure.

### Root cause

The N-arg `:ptr<void>` callback path (`src/compiler/emit_expr.c:1442-1479`)
assumes a *fat-closure* layout: it dereferences `fn_ptr` as a pointer to a
function pointer (`(*(thunk *)(fn_ptr))(fn_ptr, args...)`).  A plain
non-capturing function passed directly is a raw code address, so the
dereference reads machine code as a pointer and the call crashes.  It happens
to work for 1-arg partial-application closures because the closure struct's
first field is the thunk pointer.

### Proposed fix

Have combinators that call their argument with 2+ values take `[f :fn]`
(fat-closure `tur_poly_fn_t`) instead of `:ptr<void>`, so the callsite emits
`fn.fn(fn.env, a, b)` rather than the struct-dereference thunk.  Affected
stdlib function: `gvzip-with` in `stdlib/gadt-vec.tur` (see KB-023's remaining
limitation).  Longer term, the codegen could detect a raw-function `:ptr<void>`
and emit a direct call instead of the fat-closure dereference, but converting
the slots to `:fn` is the safer, type-checked fix.

### Effort

Medium -- per-combinator signature change, or a codegen discrimination in the
`:ptr<void>` call path.

### Resolution (DONE)

Took the recommended per-combinator signature change (the safer, type-checked
path).  `gvzip-with` in `stdlib/gadt-vec.tur` now declares its callback as
`[f :(fn [int int] :int)]` instead of `[f :ptr<void>]`.  A typed `fn`
parameter is lowered to a `tur_poly_fn_t` and any callee -- a bare
non-capturing function (`add`) or a capturing closure -- is wrapped into that
fat-closure layout, so `(f x y)` dispatches via `fn.fn(fn.env, x, y)` instead
of the `:ptr<void>` path that blindly dereferences the value as a
function-pointer table and crashed on a raw code address.

The `gadt-stdlib-vec-stdlib` fixture (which previously documented gvzip-with as
"not yet supported") now exercises `(gvzip-with add [1 2] [10 20])` and asserts
the sum `33`, locking in the fix as a regression test.

The underlying codegen path (`emit_expr.c`, `TY_PTR_VOID` N-arg call) still
assumes a fat-closure layout for `n >= 1`; calling a *raw* function directly
through a `:ptr<void>` value remains unsafe by construction in v1.  The
longer-term codegen discrimination (detect a raw-function `:ptr<void>` and emit
a direct call) is left as a future enhancement -- using a typed `fn` parameter
is the type-checked way to express a multi-argument callback.
