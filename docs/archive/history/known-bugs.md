# Known Bugs and Footguns (Archived)

> **Archived log.**  This file is a historical record of subtle bugs,
> footguns, and surprising behaviour discovered during development.  Nearly
> every entry below has been fixed (see each entry's Status and the
> [Fixed Issues](#fixed-issues) summary; consult `git log` for the
> implementation details).
>
> The handful of entries that remain open (KB-021, KB-022, KB-025, KB-026,
> KB-027, KB-029, KB-030, KB-034) have been migrated to a forward-looking
> plan with concrete fix proposals and effort estimates:
> [../upcoming/known-bugs-followups-plan.md](../../upcoming/known-bugs-followups-plan.md).
> Their entries here are kept for context but the plan is the source of truth.

A running log of subtle bugs, footguns, and surprising behaviour discovered
during development.  Each entry describes the symptom, root cause, workaround,
and fix status.

---

## KB-001 — `run-turi.sh` silently ignored fixtures missing from the allowlist

**Discovered:** 2026-05-26
**Status:** Fixed 2026-05-27 — `run_turi_fixture` emitted
`SKIP <name> (not in turi allowlist)` and the summary line surfaced an
"allowlist gap" count so missing fixtures were visible. **Retired 2026-06-13
(W5 flip):** the allowlist (`TURI_FIXTURES_DEFAULT`) is gone entirely --
`run-turi.sh` now runs every fixture under `--interpret` minus the
`requires.*` marker skips and the auto-detected inline-C carve-outs, so a new
fixture is run by default and can no longer go invisible. The workaround this
entry describes is obsolete.

### Symptom (before fix)

A newly created fixture directory under `tests/fixtures/` passed when run
directly (`./build/tur run tests/fixtures/<name>/input.tur`) but was never
mentioned in the `turi_fixture_tests` CTest run — no PASS, no FAIL, no SKIP.
The fixture was effectively invisible to CI.

### Root cause

`tests/run-turi.sh` maintains an explicit opt-in allowlist
(`TURI_FIXTURES_DEFAULT`).  Before the fix, `run_turi_fixture()` silently
`return`ed without printing anything when a fixture was absent from this list.
There was no "SKIP (not in allowlist)" message, so the omission was invisible
in test output.

### Fix

`run_turi_fixture()` now writes a `SKIP_ALLOWLIST` result file and prints a
visible `SKIP <name> (not in turi allowlist)` line.  The tally loop
aggregates them into an "allowlist gap" count emitted alongside the run
summary.  Auto-inclusion via `requires.compiled` markers (suggested as the
alternative fix) remains a future option; the visibility change is enough
to catch regressions today.

---

## KB-002 — `TUR_TEST_FILTER` did not filter `turi_fixture_tests`

**Discovered:** 2026-05-26
**Status:** Fixed 2026-05-27 — `tests/run-turi.sh` now accepts
`TUR_TEST_FILTER` as an alias for `TURI_FILTER`.

### Symptom (before fix)

Running:

```sh
TUR_TEST_FILTER="adt-param-tyvar" ctest --test-dir build -R turi_fixture_tests
```

ran the full turi fixture suite, ignoring the filter.

### Root cause

`tests/run-turi.sh` read `TURI_FILTER` for its per-fixture grep pattern.
`TUR_TEST_FILTER` was unrelated and not forwarded by the CMake test
definition for `turi_fixture_tests`.  The two env-var names were easy to
confuse.

### Fix

The default-resolution for `TURI_FILTER` is now
`"${TURI_FILTER:-${TUR_TEST_FILTER:-}}"`, so either name works.
`TURI_FILTER` still wins when both are set, preserving the existing
explicit-override behaviour.

---

## KB-004 — `::` coercion cannot bridge `:int`-returning stdlib functions to typed accessors

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 — ACB Phase 3 (`emit_carrier_bridge` in the
`EX_ASCRIBE` path) inserts the `*(ConcreteType *)(intptr_t)` dereference when
the inner expression is a carrier `int64_t` and the ascribed type is a concrete
aggregate.  Regression fixture: `tests/fixtures/typed-slots/coerce-carrier-to-struct/`.

---

## KB-007 — `derive-show-struct` / `derive-show-nested` fixtures used wrong return cast in inline C

**Status:** Fixed 2026-05-27 — both fixtures now return
`(const char *)(intptr_t)out` from the `str-concat` inline-C body.

### Symptom (before fix)

```
error: incompatible integer to pointer conversion returning 'int64_t' from a function with result type 'const char *'
```

Both `tests/fixtures/derive-show-struct/input.tur` and
`tests/fixtures/derive-show-nested/input.tur` defined `str-concat` with the
inline C body ending in:

```c
return (int64_t)(intptr_t)out;
```

but the declared return type was `:cstr` (`const char *`), so the compiler
rejected the cast as an incompatible integer-to-pointer conversion.

### Fix

Changed the final `return` line in the `str-concat` inline C block in both
fixtures to:

```c
return (const char *)(intptr_t)out;
```

---

## KB-008 — `defn-spaced-compound`: kind mismatch on `(-> int int)` type annotation

**Status:** Fixed — `fn_type_from_form` (`src/compiler/elab_fns.c`)
routes `->`, `fn`, `forall`, and `exists` heads through
`type_expr_from_form` so the spaced annotation form (`f : (-> int int)`)
resolves identically to the keyword form.  See KB-020 for the
generalised follow-up that adds `lref`.

---

## KB-009 — `result-question-op`: `?` operator lowering returns `:int` where `:bool` expected

**Status:** Fixed -- `__tur-q-is-err?` and `__tur-q-ok-val` helpers added to
`stdlib/result.tur`; `?` no longer requires an `(unsafe ...)` wrapper at call
sites.  See [docs/result-question-bool-plan.md](../result-question-bool-plan.md).

### Symptom (resolved)

```
error: if condition must be bool, got int
```

inside `(? (get-value b))` -- the `?` operator lowered to helpers that did not
exist yet, so the elaborator resolved the condition to an `:int` default.

### Fix applied

Added `__tur-q-is-err? [r :ptr<void>] #{} :bool` and
`__tur-q-ok-val [r :ptr<void>] #{} :int` to `stdlib/result.tur`.  The `?`
lowering in `src/compiler/elab_forms.c` calls these two helpers by name; both
are `#{}` (safe), so no per-call-site `(unsafe ...)` is needed.  Applying `?`
to a non-Result expression now produces the informative diagnostic
"function `__tur-q-is-err?` arg 1: expected ptr<void>, got int" instead of
the opaque type-mismatch above.

---

## KB-010 — `vec-eq-ascribed`: `vec_new()` emits `int64_t` result, incompatible with struct type

**Status:** Fixed 2026-05-28 — ACB Phase 3 (`emit_carrier_bridge` in the
`EX_ASCRIBE` path, which is the `(:: (vec-new) (Vec int))` pattern) inserts the
correct carrier-to-concrete bridge.  The `let [v :(Vec int) ...]` variant is not
valid Turmeric syntax; the valid form goes through `::` ascription and is now
handled.  Regression fixture: `tests/fixtures/typed-slots/let-vec-new/`.

---

## KB-011 — `stdlib/arrow.tur` does not load: typeclass methods called as free functions

**Discovered:** 2026-05-27
**Status:** Fixed (2026-05-28) -- `arrow-id` and `arrow-comp` removed (Option A);
`stdlib-arrow-load` fixture and `tur_stdlib_checks` CMake target added.
See [docs/stdlib-arrow-typeclass-plan.md](../stdlib-arrow-typeclass-plan.md).

### Symptom

```
$ ./build/tur check stdlib/arrow.tur
stdlib/arrow.tur:325:3: error: 'arr' is not a function or continuation
323 | ;;; Example:
324 | ;;;   (arrow-id)  ; => arr (fn [x] x)
325 | (defn arrow-id [^arr a]
326 |   (arr (fn [x] x)))
    |   ^^^^^^^^^^^^^^^^
```

The file fails on load.  No test exercises `arrow.tur` directly (the
`stdlib-arrow` fixture inlines its own helpers), so the breakage has
gone unnoticed.

### Root cause

`Arrow` is declared as a typeclass with method names `arr`, `>>>`,
`first`, `second` (`stdlib/arrow.tur:33`).  Two helpers later in the
same file call those names as if they were free functions:

```turmeric
(defn arrow-id [^arr a]
  (arr (fn [x] x)))            ; arr is an Arrow method

(defn arrow-comp [^arr a b c f g]
  (>>> f g))                   ; >>> is an Arrow method
```

These calls bypass typeclass dispatch.  The elaborator looks up `arr`
in the value scope, finds only the typeclass method (not a free
function), and rejects the call with "is not a function or
continuation".

### Workaround

`stdlib/arrow.tur` is not auto-loaded by `src/main.c`, so this bug
doesn't break compiled programs unless they explicitly
`(load "stdlib/arrow.tur")`.  Tests that need Arrow combinators
inline the implementations directly — see
`tests/fixtures/stdlib-arrow/input.tur` as the canonical pattern.

### Fix needed

See [docs/stdlib-arrow-typeclass-plan.md](../stdlib-arrow-typeclass-plan.md)
for the options under consideration (the leading recommendation is to drop
the broken helpers, since typeclass dispatch needs a receiver they don't
have).

---

## KB-012 — `Eq` instance on parametric `defstruct` segfaults on call

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 — ACB Phase 4 bridges concrete aggregate arguments
to carrier before dictionary dispatch (in `emit_expr.c` `EX_CALL` / `fn_expr`
path).  Parametric instance bodies (e.g. `Eq [Tuple2]`) already declared
`int64_t` parameters (carrier ABI); the callsite now matches.  Regression
fixture: `tests/fixtures/typed-slots/tuple2-eq-method/`.

---

## KB-015 — Repeated calls to a specialized typed accessor emit a spurious reinterpret

**Discovered:** 2026-05-26 (TS3.4 audit)
**Status:** Fixed 2026-05-28 — ACB Phase 5: in `emit_expr.c` `EX_REINTERPRET`
handling, after the `spec->call_expr == inner_call` check fails, the code now
scans `specialized_call_exprs[]` for the inner call node.  When found, the clone
already returns the concrete type and the reinterpretation is skipped.  Regression
fixture: `tests/fixtures/typed-slots/cons-double-twice/`.

---

## KB-016 -- Codegen snapshots stale after Tuple2/3/4/5 additions and ACB Phase 1 field changes

**Discovered:** 2026-05-27
**Updated:** 2026-05-28 (ACB Phase 1 adds additional drift)
**Status:** Fixed 2026-05-28 -- all 49 stale `expected.c` snapshots regenerated;
`hkt-instances` also fixed for the KB-032 `Functor` conflict (renamed to
`TestFunctor`).  All affected fixtures now pass.

---

## KB-017 -- Effect row type annotation syntax not supported in `fn` type expressions

**Discovered:** 2026-05-27
**Status:** Fixed (no longer reproduces 2026-05-28) -- all 10
happy-path fixtures (`effect-fn-type-annot`, `effect-poly-bracket`,
`effect-poly-infer`, `effect-poly-map`, `effect-poly-typeclass`,
`effect-row-compose`, `effect-row-ho`, `effect-row-var-unused`,
`effect-subtype-assign`, `effect-subtype-ho`) and the 5 corresponding
error fixtures (`errors/effect-fn-type-mismatch`,
`errors/effect-poly-escape`, `errors/effect-row-occurs`,
`errors/effect-row-var-mismatch`, `errors/effect-subtype-violation`)
now pass.  The `(fn [arg-types...] #{row} ret-type)` form is accepted
in type-annotation position.  Closing this entry; consult the git log
on `src/compiler/elab_types.c` for the implementation detail.

---

## KB-018 -- `(handler E V R)` type expression not supported

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 -- `handler` is now routed through
`type_expr_from_form` from the parameter-type path, matching the
existing keyword-form behaviour.

### Root cause

`type_expr_from_form` already had a `(handler E V R)` case (gated on
`-Xeffect-types`) but the parameter-type path in `fn_type_from_form`
intercepted compound forms first and treated `handler` as the head of
a generic TY_APP application -- which failed the arrow-kind check and
emitted TUR-E0012 instead of the intended "requires -Xeffect-types"
diagnostic.

### Fix

Added `e->sym_handler_type` to the KB-008/KB-020 special-case list in
`fn_type_from_form` so `(handler E V R)` reaches the dedicated
handler-type case in `type_expr_from_form`, regardless of whether the
annotation is spaced (`h : (handler ...)`) or keyword-prefixed
(`h :(handler ...)`).

---

## KB-019 -- Session type annotation syntax causes kind mismatch

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 -- `Session`, `Send`, `Recv`, `Choose`,
`Branch`, `Rec`, `Timeout`, `Role`, and `project` are now routed
through `type_expr_from_form` from the parameter-type path.  All 13
previously-blocked happy-path fixtures pass.

### Root cause

`type_expr_from_form` already handled `(Session proto)` and the nested
protocol constructors, but the parameter-type path in
`fn_type_from_form` intercepted compound forms first and treated each
head as the start of a generic TY_APP application -- which failed the
arrow-kind check on `Session` (kind `*`) and surfaced TUR-E0012 before
the protocol handler could fire.

### Fix

Added the session/role/project constructor heads to the KB-008/KB-020
special-case list in `fn_type_from_form` so the dedicated protocol
rules in `type_expr_from_form` are reached for every annotation form.

---

## KB-020 -- Spaced compound type annotations fail for `->` and `lref`

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 -- the KB-008 fix already covered `->`;
added `lref` to the special-case list in `fn_type_from_form`
(`src/compiler/elab_fns.c`).  Both `(defn consume-lref [p : (lref int)] ...)`
and `(defn apply-linear [f : (-> ^linear int int) ...] ...)` now elaborate.

### Fix

The KB-008 special-case in `fn_type_from_form` was already routing
`->`, `fn`, `forall`, and `exists` heads through `type_expr_from_form`.
Extended it to also route `lref` (the only other compound built-in
constructor that was still falling through to the generic TY_APP path).
If additional built-in heads ever need the same treatment (e.g. `rc`
gains a list-form constructor), add them to the same list.

---

## KB-021 -- Typeclass dispatch vtable ABI mismatch for struct-typed instances

**Discovered:** 2026-05-27
**Status:** Open -- codegen bug.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-021](../../upcoming/known-bugs-followups-plan.md).

### Symptom

Calling a typeclass method on a monomorphic struct instance fails to
compile when the vtable entry expects the struct by value but the
callsite passes an `int64_t` carrier:

```c
// generated callsite:
dict_Eq_Vec_singleton.eq_(__cmp_a, __cmp_b);
// __cmp_a / __cmp_b are Vec__int (struct), but the cast target is:
// bool (*)(Vec__int, Vec__int) -- expects struct by value
// while the actual stored function expects int64_t (carrier ABI)
```

C rejects this as an incompatible argument type.

### Affected fixtures

`ptc4-basic`, `map-of-tvec-eq`, `mutmap-eq`, `option-of-tvec-eq`,
`result-of-typed-eq`, `set-of-tvec-eq`, `vec-of-tvec-eq`.

Also related: `vec-eq-ascribed`, `vec-eq-ascribed-multi` (KB-010).

### Root cause

Two calling conventions are in use simultaneously:

1. **Carrier ABI** (legacy): instances store and call via `int64_t`;
   the instance body casts the carrier to a struct pointer.
2. **By-value ABI** (new): the callsite casts the vtable entry to
   `bool (*)(StructType, StructType)` and passes the struct directly.

The vtable is populated with a carrier-ABI function (the instance body
takes `int64_t`) but the callsite casts to the by-value ABI.  One of
the two sides needs to be made consistent.

### Workaround

Avoid calling typeclass methods that dispatch to struct-type instances
(e.g. `Eq [Vec]`, `Eq [Map]`, `Eq [MutableMap]`).  Use the
library-specific comparison helpers instead (`vec-eq?`, `map-eq?`,
etc.).

### Fix needed

Choose one ABI for struct-valued typeclass dispatch and make both the
instance-body codegen and the callsite cast agree on it.  The by-value
ABI is more type-safe; the carrier ABI requires explicit pointer casts
in every instance body but keeps the vtable uniform.

---

## KB-022 -- `gadt-equal-cong`: HKT function-type parameter rejected at callsite

**Discovered:** 2026-05-27
**Status:** Open -- HKT limitation.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-022](../../upcoming/known-bugs-followups-plan.md).

### Symptom

```
error [TUR-E0001]: function 'equal-cong' arg 1: expected
  (type-app (type-app Equal tyvar) tyvar), got Equal
(let [pf (equal-cong (Refl))])
                      ^^^^^^
```

`equal-cong` is declared with a kind-polymorphic parameter `^f` and
takes `eq : (Equal a b)`.  At the callsite, `(Refl)` has type
`(Equal a a)` (monomorphic in one variable), but the checker expects
`(Equal a b)` with two distinct type variables.

### Root cause

The HKT kind inference for `^f` constrains the `Equal` type arguments
differently from the monomorphic `Refl` constructor.  The type
unification fails to match `(Equal a a)` against `(Equal a b)` in the
presence of a kind-variable annotation.

### Fix needed

Investigate HKT constraint unification for GADT constructors where the
same type variable appears in multiple positions in the constructor's
return type.

---

## KB-023 -- `gadt-stdlib-vec-stdlib`: GADT `Vec` name collides with stdlib `Vec` struct

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 -- `stdlib/gadt-vec.tur` renamed the GADT
from `Vec` to `GVec` and all associated constructors and functions
(`GVNil`, `GVCons`, `gvec-nil`, `gvec-cons`, `gvec-len`, `gvec-sum`,
`gvec-head-or`, `gvec-tail`, `gvmap`, `gvzip-with`).  The
`gadt-stdlib-vec-stdlib` fixture was updated to use the new names.

### Symptom (before fix)

Loading `stdlib/gadt-vec.tur` produced:

```
warning: GADT 'Vec' shadows existing struct 'Vec'; uses of ':Vec'
         in type annotations resolve to the GADT
```

### Root cause

The stdlib auto-loads `stdlib/vec.tur` which defines a `Vec` struct.
`stdlib/gadt-vec.tur` then defined a GADT also named `Vec`, creating a
shadowing warning and a potential naming conflict.

### Remaining limitation

`gvzip-with` takes its combining function as `:ptr<void>` and calls it
with two arguments.  Passing a plain 2-arg `defn` via `ptr<void>` is
not supported by the current thunk calling convention (the call emits a
2-arg thunk cast that dereferences the function address as a struct,
causing a segfault).  The `gadt-stdlib-vec-stdlib` fixture omits the
`gvzip-with` test for this reason; a future fix should either change
`gvzip-with` to use `[f :fn]` (fat-closure) or add a 2-arg
`__gvec-call-fn2` helper that bypasses the thunk cast.

---

## KB-024 -- `errors/defstruct-copy-noncopy-compound-field`: type name missing in diagnostic

**Discovered:** 2026-05-27
**Status:** Fixed 2026-05-28 -- `elab_structs.c` now tracks the resolved
compound Type for the F_LIST field-type path and uses it for the :copy
diagnostic, so the message now reads
`defstruct: field 'r' has non-copy type lref<int>`.

### Symptom

```
error: defstruct: field 'r' has non-copy type : and cannot be used in :copy struct
```

Expected (from `expected.diag`):

```
defstruct: field 'r' has non-copy type lref<int>
```

The type name is printed as `:` (the colon character only) instead of
`lref<int>`.

### Root cause

When the field type is written in compound form `(lref int)`, the
F_LIST handling in `elab_structs.c` only stored the resolved Type on
`full_type` when its kind was `TY_APP` / `TY_EXISTS` / `TY_FORALL` (or
when the struct had type parameters).  For `(lref int)` the resolved
kind is `TY_LREF`, so `full_type` stayed NULL and the diagnostic fell
through to a branch that printed `type_name_form->as.sym->name` --
but `type_name_form` is an F_LIST in this path, so the union read
yielded a stray `:` character.

### Fix

Introduced a separate `compound_type` local that is set whenever the
field came from an F_LIST form, regardless of the resolved kind.  The
:copy diagnostic now prefers `full_type` (when set, preserving the
existing storage path) and falls back to `compound_type` (used purely
for the diagnostic) before resorting to the bare-symbol fallback.

---

## KB-025 -- `errors/gadt-refine-escape`: skolem escape not detected

**Discovered:** 2026-05-27
**Status:** Open -- missing check.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-025](../../upcoming/known-bugs-followups-plan.md).

### Symptom

The fixture `errors/gadt-refine-escape` expects a GADT skolem-escape
diagnostic but the compiler accepts the program without error.

```turmeric
(defn my-unbox [b] : int
  (match b
    (MkBox x) x))   ;; x has type 'a' (skolem), expected to escape
```

### Root cause

`my-unbox` is declared `:int` but the match arm binds `x` to the
GADT's phantom type variable `a`.  The compiler should detect that a
skolem type variable is escaping its scope through the `:int` return
annotation and emit an error.  Currently no escape check is performed,
so the program compiles silently.

### Fix needed

Add a skolem-escape check after GADT match-arm elaboration: if the
type of any match-arm result contains an unresolved skolem variable
that is not bound by the surrounding function's type, emit an error.

---

## KB-026 -- `errors/kinds-kind-variable`: kind variable annotation produces no error

**Discovered:** 2026-05-27
**Status:** Open -- missing validation.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-026](../../upcoming/known-bugs-followups-plan.md).
Now also covers the related `errors/typeann-diag-hint` regression (the
implicit-tyvar acceptance suppresses the parameter type-annotation hint).

### Symptom

The fixture `errors/kinds-kind-variable` expects an error when a kind-
variable annotation `^f` is used in `defn` parameters, but the compiler
accepts the program silently.

```turmeric
(defn map [^f a x] : a
  x)
```

The `:a` return type is also not a built-in type; the fixture expected a
"unsupported" diagnostic for this.

### Root cause

Kind-variable annotations (`^f`) on function parameters are silently
erased at runtime (Phase H4 complete).  The return type `:a` is also
not rejected -- it appears to be accepted as a type variable or
unresolved symbol without producing an error.  The fixture was written
expecting both annotations to trigger diagnostics, but neither does.

### Fix needed

Determine the intended semantics: if `^f` and `:a` in this position
should be errors (e.g. outside a `defclass`/`definstance` context),
add validation in the elaborator and emit clear diagnostics.  If they
are intentionally silently erased, update `expected.diag` to reflect
the accepted form.

---

## KB-027 -- `stdlib/rc.tur` fails `tur check`: kind mismatch on `Functor [ptr<void>]`

**Discovered:** 2026-05-28
**Status:** Open -- stdlib design limitation.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-027](../../upcoming/known-bugs-followups-plan.md).

### Symptom

```
$ ./build/tur check stdlib/rc.tur
stdlib/rc.tur:59:1: error [TUR-E0012]: kind mismatch: typeclass 'Functor'
  parameter 1 expects kind '* -> *' (a type constructor),
  but 'ptr<void>' has kind '*'
stdlib/rc.tur:104:14: error: typeclass 'Foldable' is not defined
```

`tur check stdlib/rc.tur` fails on two distinct issues.  The file is not
in `tests/run-stdlib-checks.sh`.

### Root cause

1. `(definstance Functor [ptr<void>])` is a kind error: `Functor` requires
   a type constructor of kind `* -> *` (something like `Option` or `List`),
   but `ptr<void>` is a concrete type of kind `*`.  The comment in the file
   acknowledges this is a v1 approximation
   (`; Uses ptr<void> as the type constructor representation in v1`).

2. `Foldable` and `Traversable` are defined in `stdlib/typeclass.tur`, which
   is not in the auto-loaded stdlib list, so they are not in scope when the
   file is checked standalone.

### Fix needed

Replace the `ptr<void>` instances with a newtype wrapper (e.g. `(defstruct Rc
[inner :ptr<void>])`) that introduces a proper type constructor of kind `* -> *`.
Once the kind is correct the `Functor`, `Foldable`, and `Traversable` instances
can be expressed without approximation.  Alternatively, add `typeclass.tur` to
the auto-load list (after resolving its own Functor/Applicative/Monad overlap
with `typeclass-functor.tur`).

---

## KB-028 -- `stdlib/ref.tur` fails `tur check`: `Clone` typeclass not in scope

**Discovered:** 2026-05-28
**Status:** Fixed 2026-05-28 -- created `stdlib/typeclass-clone.tur`
(minimal `Clone` stub auto-loaded alongside `typeclass-eq.tur` and
`typeclass-functor.tur`) and moved the `Clone [ref]` instance there
(since `ref` is a built-in with no home module, the orphan rule requires
the instance to live with the typeclass definition).  The `Clone [Ref]`
instance remains in `ref.tur` because `Ref` is defined there.

### Symptom (before fix)

```
$ ./build/tur check stdlib/ref.tur
stdlib/ref.tur:82:14: error: typeclass 'Clone' is not defined
stdlib/ref.tur:101:14: error: typeclass 'Clone' is not defined
```

### Root cause

`Clone` is defined in `stdlib/typeclass.tur`.  That file is not in the
auto-loaded stdlib list (`stdlib_files[]` in `src/main.c`), so `Clone` is
not in scope when `ref.tur` is checked in isolation.

---

## KB-029 -- `stdlib/session.tur` excluded from `tur check` allowlist

**Discovered:** 2026-05-28
**Updated:** 2026-05-28 -- the KB-019 blocker is resolved; the file
still trips a different issue.
**Status:** Open -- now blocked on tuple return-type syntax, not
session types.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-029](../../upcoming/known-bugs-followups-plan.md).

`tur check -Xsessions stdlib/session.tur` no longer reports a session
kind mismatch (KB-019 is fixed).  It now fails on a single line that
uses a tuple return-type annotation:

```
stdlib/session.tur:70: error: unsupported type expression form
  (expected symbol, keyword, or list)
... :[(int (Session ...))]
       ^^^^^^^^^^^^^^^^^^
```

The remaining work is to either teach the type-annotation elaborator
to accept the `:[(...)]` tuple-return-type form, or rewrite the
affected signatures to use a named tuple struct.  Once that resolves,
`stdlib/session.tur` can be added to `tests/run-stdlib-checks.sh`.

---

## KB-030 -- `stdlib/str.tur` fails `tur check`: orphan instance for `Eq [str]`

**Discovered:** 2026-05-28
**Status:** Open -- orphan-instance checker does not recognise compiler built-ins.
Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-030](../../upcoming/known-bugs-followups-plan.md).
This is the same root cause that broke the `clone-vec` / `backtrack-clone-*`
fixtures (orphan instances on built-in `int`/`vec`); those fixtures were
worked around via the KB-035 rename, but the checker limitation remains.

### Symptom

```
$ ./build/tur check stdlib/str.tur
stdlib/str.tur:114:1: error [TUR-E0013]: orphan instance: typeclass 'Eq' is
  defined in a different module and none of the type arguments belong to this
  module; move the instance to the module that defines the typeclass or one
  of the type arguments
(definstance Eq [str]
```

The file is not in `tests/run-stdlib-checks.sh`.

### Root cause

The orphan-instance rule (TUR-E0013) requires that a `(definstance TC [T]
...)` lives either in the file that defines `TC` or in the file that defines
`T`.  `Eq` is defined in `stdlib/typeclass-eq.tur`.  `str` is a compiler
built-in -- it has no `defstruct` or other definition in any `.tur` file, so
the orphan checker finds no file that "owns" `str` and rejects the instance.

### Workaround

None for standalone `tur check`.  At runtime the instances work correctly
because they are loaded alongside the rest of the stdlib and the orphan check
is not repeated at link time.

### Fix needed

The orphan checker needs a mechanism to mark certain built-in primitive types
(`str`, `cstr`, `int`, `bool`, etc.) as "owned" by a designated file so that
instances for those types can legitimately appear there.  Alternatively, the
`Eq [str]`, `Ord [str]`, `Show [str]`, and `Clone [str]` instances could be
moved into `typeclass-eq.tur` / `typeclass.tur` where the typeclasses are
defined, satisfying the existing orphan rule without any compiler changes.

---

## KB-031 -- ACB Phase 1 regression: struct args passed as pointer in typeclass callsites

**Discovered:** 2026-05-28
**Status:** Fixed 2026-05-28 — `type_uses_carrier_in_dispatch` helper in
`src/compiler/emit_expr.c` restricts the carrier bridge (CK_CONCRETE →
CK_CARRIER) to parametric structs only (`TY_APP`, `TY_ADT`, `TY_STRUCT` with
`n_type_params > 0`).  Non-parametric struct instance bodies (e.g. `Clone [Opt]`)
use concrete by-value ABI; the callsite now matches.  Fixtures `clone-option`,
`clone-list`, and `clone-pair` all pass.

---

## KB-032 -- `defclass Functor` in fixtures conflicts with auto-loaded `stdlib/typeclass-functor.tur`

**Discovered:** 2026-05-28
**Status:** Fixed 2026-05-28 (initial), completed 2026-05-28 (see KB-035).
The initial fix renamed `Functor` to `TestFunctor` in three fixtures
(`dump-kinds-basic`, `hkt-closure-capture`, `errors/hkt-orphan-instance`;
option a), but ~16 more happy-path fixtures with the same conflict were
left red.  KB-035 extends the same rename to the full set.

### Symptom (before fix)

Fixtures that define their own `Functor` typeclass failed at `emit-c` time
with a duplicate-definition error, and fixtures that define
`definstance Functor [...]` failed with an orphan-instance error:

```
FAIL dump-kinds-basic -- emit-c failed
  input.tur:3:1: error: typeclass 'Functor' is already defined

FAIL hkt-closure-capture -- build failed
  input.tur:40:1: error [TUR-E0013]: orphan instance: typeclass 'Functor'
    is defined in a different module ...

FAIL errors/hkt-orphan-instance -- diagnostic mismatch
  tc/functor.tur:2:3: error: typeclass 'Functor' is already defined
  (expected "orphan instance: typeclass 'Functor' is defined in a
   different module")
```

### Root cause

`stdlib/typeclass-functor.tur` defines `Functor` and is unconditionally
auto-loaded by `main.c` (since commit `2046ec3`, TS5+TS6 PR #86).
Any fixture that tries to define its own `Functor` class triggers a
duplicate-definition error.

### Fix

Renamed the local `Functor` class to `TestFunctor` in
`tests/fixtures/dump-kinds-basic/input.tur`,
`tests/fixtures/hkt-closure-capture/input.tur`,
`tests/fixtures/errors/hkt-orphan-instance/tc/functor.tur`, and
`tests/fixtures/errors/hkt-orphan-instance/input.tur`.  Updated
`expected.diag` and regenerated `expected.c` for `dump-kinds-basic`.

---

## KB-033 -- `any` type guard bypassed when using spaced annotation syntax

**Discovered:** 2026-05-28
**Status:** Fixed 2026-05-28 -- the bare-symbol path in
`type_expr_from_form` now applies the same `-Xunion-types` /
`-Xintersection-types` flag check as the keyword `:any` path.

### Symptom

The `any` top type is supposed to require `-Xunion-types` or
`-Xintersection-types`.  Without either flag, compiling a file that
uses `any` in a parameter annotation should fail with:

```
error: 'any' type requires -Xunion-types or -Xintersection-types
```

However, using the *spaced* annotation form `x : any` (with a space
before the colon) bypasses the check entirely and compiles successfully:

```sh
# FAIL -- exits 0, emits C, no diagnostic
./build/tur emit-c tests/fixtures/errors/any-type-disabled/input.tur
```

```
; any-type-disabled fixture
(defn f [x : any] :nil nil)
```

The gate at `elab_types.c:344` is reached only via the compact form
`x :any`; the spaced `: any` path went through the bare-symbol branch
which called `typekind_from_symbol("any")` (returning `TY_ANY`) and
returned the type without ever checking the feature flags.

### Affected fixtures

`errors/any-type-disabled`.

### Fix

Added the same `TY_ANY`-flag guard to the bare-symbol branch in
`type_expr_from_form` so the spaced annotation form (`x : any`) now
emits the same `'any' type requires -Xunion-types or
-Xintersection-types` diagnostic as the keyword form (`x :any`).

---

## KB-034 -- Calling a `:ptr<void>` function with 2 arguments segfaults

**Discovered:** 2026-05-28
**Status:** Open -- thunk calling-convention limitation.  Migrated to
[../upcoming/known-bugs-followups-plan.md#kb-034](../../upcoming/known-bugs-followups-plan.md).

### Symptom

Any Turmeric function that receives a function argument typed `:ptr<void>`
and then calls it with two arguments segfaults at runtime:

```turmeric
(defn call2 [f : ptr<void> a : int b : int] : int (f a b))

(defn add [a : int b : int] : int (+ a b))

(defn main [] : int
  (println (call2 add 10 20))   ; => Segmentation fault
  0)
```

Single-argument `:ptr<void>` calls work correctly when `f` is a partial
application closure (e.g. `(mul 2)`):

```turmeric
(defn call1 [f : ptr<void> x : int] : int (f x))
(println (call1 (mul 2) 5))   ; => 10  (correct)
```

### Root cause

When a function `f` is called with N arguments and `f` has type `:ptr<void>`,
the codegen emits an N-arg thunk call:

```c
/* 2-arg case */
(* (tur_thunk_int64_t_int64_t_int64_t_t *)(f))(f, a, b)
```

The cast `(tur_thunk_int64_t_int64_t_int64_t_t *)(f)` treats `f` (the raw
function address) as a pointer to a function pointer.  Dereferencing that
reads the first bytes of the function's machine code as an integer, then
tries to call that integer as a function pointer -- which crashes.

This works for 1-arg partial application closures because the closure struct
layout has the thunk pointer `__fn` as its first field, so dereferencing the
struct pointer yields the correct function pointer.  A plain 2-arg `defn`
passed directly has no such struct wrapper.

### Workaround

For `gvzip-with` and similar combinators: wrap the 2-arg function in a
unary closure via partial application at the call site.  This is only
possible if the function is curried (i.e. the caller has one value to
capture):

```turmeric
;; Instead of:  (gvzip-with add v1 v2)
;; If you want to add a fixed offset k, use:
(gvzip-with (add k) v1 v2)   ;; (add k) is a 1-arg closure
```

There is no general workaround for calling a non-capturing 2-arg function
through a `:ptr<void>` slot.

### Fix needed

Change functions that need to call their argument with 2 values to use
`[f :fn]` (fat-closure, `tur_poly_fn_t`) instead of `:ptr<void>`.  The
fat-closure type stores `(fn_ptr, env)` and the call site emits
`fn.fn(fn.env, a, b)` rather than the broken struct-dereference thunk.
Affected stdlib functions: `gvzip-with` in `stdlib/gadt-vec.tur`.

---

## KB-035 -- Auto-loaded `Functor`/`Clone` typeclasses broke ~19 HKT/Clone fixtures

**Discovered:** 2026-05-28
**Status:** Fixed 2026-05-28 -- completes KB-032 and resolves the analogous
`Clone` fallout from KB-028.

### Symptom (before fix)

After `stdlib/typeclass-functor.tur` (KB-032) and `stdlib/typeclass-clone.tur`
(KB-028) became auto-loaded, every fixture that defined its own `Functor` or
`Clone` typeclass failed -- either with `typeclass 'Functor' is already
defined` or with a TUR-E0013 orphan-instance error on a built-in type arg:

```
FAIL hkt-functor-option -- typeclass 'Functor' is already defined
FAIL clone-vec          -- orphan instance: typeclass 'Clone' is defined in a
                           different module ... (definstance Clone [int])
```

KB-032 only renamed three fixtures, leaving ~16 happy-path HKT fixtures and
the three `Clone` fixtures red under `tests/run.sh`.

### Fix

Applied the KB-032 rename pattern to the full set: the locally-defined
`Functor` class/instances became `TestFunctor` and the locally-defined `Clone`
class/instances became `TestClone` (the method names `fmap` / `clone` are
unchanged because they dispatch via `.fmap` / `.clone`).  No `expected.c`
snapshots were involved.

Fixtures fixed (Functor): `hkt-functor-option`, `hkt-monad-option`,
`hkt-functor-laws`, `hkt-do-m`, `hkt-closures`, `hkt-closures-defers-refs`,
`hkt-witness-basic`, `hkt-witness-multi-instance`, `hkt-fn-constraints`,
`hkt-fn-implicit-kind`, `hkt-fn-kind-param`, `hkt-dispatch-unambiguous`,
`hkt-partial-app`, `hkt-stdlib-suite`, `kinds-inference`, `type-app`.

Fixtures fixed (Clone): `clone-vec`, `backtrack-clone-rc`,
`backtrack-clone-ref`.

The underlying orphan-on-built-in checker limitation is tracked as KB-030
(see the follow-ups plan).  After this change `tests/run.sh` is down to the
12 failures owned by KB-021 (8), KB-022 (1), and KB-025/KB-026 (3).

---

## Fixed Issues

Brief log of previously-tracked bugs that have been fully resolved.  Refer
to the git history (and the diffs of the commits that mention each KB
identifier) for implementation detail.

- **KB-003** — GADT match arm: `TY_TYVAR` field binding caused overload-resolution failure.  Fixed in TP6: scrutinee `TY_APP` unwrapping now propagates concrete type args to match-arm bindings.
- **KB-004** — `::` coercion did not bridge carrier `int64_t` to concrete aggregate.  Fixed 2026-05-28 (ACB Phase 3): `emit_carrier_bridge` inserted in `EX_ASCRIBE` path.
- **KB-005** — `emit_expr.c` ADT match crashed when scrutinee type was `TY_APP`.  Fixed in TP6: the emit side now unwraps the `TY_APP` chain before reading `.as.adt_.def`, matching the elab side.
- **KB-006** — 48 `expected.c` codegen snapshots were stale.  Fixed by regenerating snapshots with `./build/tur emit-c` after verifying runtime output.  Reduced unique test failures from ~142 to ~95.
- **KB-010** — `vec_new()` emits `int64_t` result, incompatible with annotated struct type.  Fixed 2026-05-28 (ACB Phase 3): carrier bridge in `EX_ASCRIBE` path handles the `(:: (vec-new) (Vec int))` pattern.
- **KB-012** — `Eq` instance on parametric `defstruct` segfaulted on call.  Fixed 2026-05-28 (ACB Phase 4): dictionary dispatch bridges concrete aggregate args to carrier for parametric dispatch types.
- **KB-013** — Sized numeric type arguments (`i32`, `f32`, …) collapsed to generic kind in struct-app mangling.  Fixed 2026-05-26: added the short-form sized type names to `typekind_from_symbol` and `typekind_from_name`.
- **KB-014** — Aggregate struct-app let-bindings used the `int64_t` carrier instead of the concrete C struct type.  Fixed 2026-05-26 as a downstream effect of the KB-013 fix; once the struct-app argument resolves correctly, the let-binding picks up the concrete C type.
- **KB-015** — Repeated calls to a specialized typed accessor emitted a spurious reinterpret.  Fixed 2026-05-28 (ACB Phase 5): `EX_REINTERPRET` now scans `specialized_call_exprs[]` so the skip fires correctly on the second call.
- **KB-016** — 49 `expected.c` codegen snapshots stale after Tuple2/3/4/5 additions and ACB Phase 1 field changes.  Fixed 2026-05-28: all snapshots regenerated with runtime output verified.
- **KB-031** — ACB Phase 4 regression: non-parametric struct args passed as pointer in typeclass callsites.  Fixed 2026-05-28: `type_uses_carrier_in_dispatch` restricts carrier bridge to parametric structs only.
