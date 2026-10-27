# Return-Type-Directed Dispatch + Reader-Table `<Type>` Support: Finishing `tur/schema` SC5 & SC7

> **Status:** Phase RT landed (return-type-directed dispatch). The
> carrier-return bridge then unblocked SC5 (typed decode to a real by-value
> struct) and RD (the `#json-str<T>` reader family); both shipped. **SC7 is now
> complete:** the Validation combinator layer + phantom-param-inference enabler,
> *and* the `Functor`/`Applicative`/`Alternative` typeclass instances over the
> phantom `(Schema a)` wrapper -- including applicative struct building. The final
> carrier/concrete duality was collapsed by a transparent int-newtype
> representation (see `docs/sc7-carrier-duality-plan.md`, now RESOLVED). See
> "Implementation status" below.
>
> **Depends on:** `stdlib/schema.tur` SC0--SC4 (shipped),
> `stdlib/typeclass.tur` (dictionary-passing typeclasses), `tur/json`,
> `tur/result`, `tur/vec`.
>
> **Related:**
> - `docs/schema-plan.md` -- the parent plan; SC5/SC7 sketches live there
> - `docs/upcoming/json-reader-macro-plan.md` -- the `#json(...)` reader macro
>   whose `<Type>` slot this plan activates
> - `docs/guides/schema-guide.md` -- user-facing guide (extend in RD5)
>
> **Last updated:** 2026-05-31

---

## Implementation status (2026-05-31)

**Phase RT -- DONE and tested.** Return-type-directed dispatch is implemented
and exercised by fixtures; the full suite is green (1174 passed, 0 failed).

- **RT1** (`where`-clause parsing): `(defn f [...] : T where (Class a) ...)`
  parses into `FnDef.constraints`; `TypeConstraint` gained `tyvar` +
  `return_resolved`. Absent a `where` clause, behavior is unchanged.
- **RT2** (expected-type channel): `Elab.expected_type`, pushed by
  `elab_ascribe` around the inner expression of `(:: e T)`. NULL everywhere it
  is not explicitly pushed.
- **RT3/RT4** (resolution): typeclass methods may now declare a tyvar
  return/parameter (`(default-of [] : a)`). `elab_try_return_dispatch`
  (`elab_typeclasses.c`) intercepts a bare-name call to a method whose dispatch
  tyvar appears only in the return, unifies the (bare or structured, e.g.
  `(Result a E)`) return against `Elab.expected_type`, looks up the instance
  (structs discriminated by `StructDef` identity), and emits a direct call to
  the impl. Diagnostics cover the unascribed and missing-instance cases.
  Return-dispatch methods *with* parameters are supported: such a parameter
  keeps its declared type (it is not rewritten to the instance type), and the
  instance method's tyvar return is substituted to the instance type.
- Fixtures: `rt-return-dispatch-basic`, `rt-return-dispatch-param`,
  `errors/rt-return-dispatch-unascribed`, `errors/rt-missing-instance`.

**SC5 / RD -- DONE via the carrier-return bridge.** The headline "typed decode
to a user struct" (`(:: (decode! raw) User)` returning a real `User` struct)
now works. The blocker was that a typeclass instance method whose declared
return is the dispatch tyvar lowered to the `int64` carrier while its body
resolved to a by-value struct, so the function signature disagreed with the
dictionary slot and the resolved call site (both already concrete), producing
"incompatible types ... 'User' but 'int64_t'" / "invalid initializer". The fix
(`emit_carrier_return_override`, `emit_core.c`) detects a non-carrier by-value
struct body type and emits that concrete struct type in both the function
definition (`emit_fns.c`) and its forward declaration (`emit_module.c`), so all
three sites agree -- no boxing needed, since the dict slot and ascribed call
site already use the concrete type. ADTs, type-apps and parametric structs keep
the consistent `int64` carrier untouched. SC5's `HasSchema`/`decode!` and RD's
`#json-str<T>` reader macro (under `-Xschema-reader`) build on this.

**SC7 -- DONE (combinator layer + typeclass instances).** The Validation
combinators shipped as plain functions over the int-carrier schema:
`schema/always` (pure), `schema/never` (empty), `schema/ap` (accumulating
applicative), `schema/field-of` (extract), `schema/fmap` (transform alias),
`schema/alt` (two-arm union), plus `schema/ap-fat` (closure-aware apply), with
`SCHEMA_ALWAYS/NEVER/AP/FIELD/AP_FAT` discriminants + decoder cases. The
phantom-param-inference enabler landed: `make-struct` fills a type parameter
absent from all fields from the RT `expected_type` channel (`elab_structs.c`), so
`(:: (make-struct Schema 0) (Schema cstr))` type-checks.

The `Functor`/`Applicative`/`Alternative` *typeclass instances* over the phantom
`(defstruct Schema [A] (raw :int))` wrapper now ship, with chaining and
applicative struct building. The final carrier/concrete duality was collapsed by
a **transparent int-newtype** representation: a parametric struct with a single
`:int` field has one C representation (int64) at every site, so HKT results flow
with their `(Schema b)` type and chain across `fmap`/`ap`/`alt-or`. The chainable
return is a small gated dispatch override (`elab_typeclasses.c`). See
`docs/sc7-carrier-duality-plan.md` (RESOLVED) for the full mechanism. Proof
fixtures: `schema-hkt-functor`, `schema-hkt-alternative`,
`schema-applicative-user`, `schema-applicative-user-errors`.

The `Functor`/`Applicative`/`Alternative` *typeclass instances* (the `<$>`/`<*>`/
`<|>` operator sugar and applicative *struct* building) were deferred on three
independent compiler obstacles. **All three of the original obstacles are now
cleared** (2026-06-01); what remains is a narrower type-level feature (a
chainable HKT method return) plus the by-value aggregate carrier for *struct*
building:

1. **(CLEARED) HKT-kind registration for the wrapper.** A user-defined
   *parametric* struct (`(defstruct Schema [A] (raw :int))`) now reports its
   arity-based kind (`* -> *`) instead of `*`, so `(definstance Functor [Schema])`
   kind-checks and dispatches by `StructDef` identity exactly like the built-in
   `rc<T>`. Fix: `type_effective_kind` (`src/passes/kind_check.c`) keys on
   `def->n_type_params`; the kind-inference pass still refuses to let a parametric
   struct *promote* a `*`-declared class (so `Measurable [Box]` stays concrete).
   Proof fixture: `hkt-user-parametric-struct`.
2. **(CLEARED) Closure application in the decoder.** `schema/transform` and
   `schema/fmap` now carry a *fat closure* handle (the standard `{ thunk, env }`
   layout, via a `^fat` parameter) and the inline-C decoder invokes it through
   `TUR_APPLY1`, so a *capturing* closure -- not just a top-level function -- can
   map a decoded value. Proof fixture: `schema-transform-closure`. (The
   accumulating `schema/ap` function-arm still takes a top-level fn; see the
   remaining-work note below.)
3. **(CLEARED) The typeclass-method closure ABI.** A typeclass method receives
   its function argument as a 16-byte `tur_poly_fn_t` `{env, fn}`, whereas the
   `^fat`/decoder path expects the single-int64 fat handle. The new `EX_POLY_TO_FAT`
   conversion (`elab_call.c` at the `^fat` site, emitted in `emit_expr.c`, backed
   by the `__tur_poly_to_fat1` preamble thunk in `emit_module.c`) boxes the
   `tur_poly_fn_t` into `{ __tur_poly_to_fat1, fn, env }` so a Functor/Applicative
   instance can hand its closure argument straight to a `^fat` schema combinator,
   capturing closures included. Proof fixture: `hkt-instance-closure-to-fat`.

**Remaining work (narrower than the original obstacles).** Two things still stand
between the cleared primitives and the full operator sugar: (a) a Functor method
declared `:int` loses the `(Schema b)` wrapper on return, so results are not
chainable without re-ascription -- a *chainable HKT method return* (`(f b)`); and
(b) applicative *struct* building still ends in a by-value aggregate the int64
decoder carrier cannot hold without boxing, and `schema/ap`'s direct C call still
rules out multi-argument currying. An end-to-end experiment (2026-06-01) showed
(a) is **not** independent of (b): chaining requires `(Schema a)` to have one
consistent C representation, but a parametric wrapper instantiated at a concrete
element has *both* a by-value layout and the int64 carrier, and codegen mixes
them. The concrete multi-site diagnosis and a proposed one-representation fix are
in [docs/sc7-carrier-duality-plan.md](sc7-carrier-duality-plan.md).

What *does* work today, and what each phase needs:

- Return-dispatch for **int-represented** types (primitives, and any type whose
  runtime representation is `int64`) works end to end, including methods with
  parameters -- see `rt-return-dispatch-param`. A `HasSchema`/`decode!` design
  built on int-represented carriers (e.g. returning the validated JSON node)
  works with the current machinery.
- **SC5** (struct-typed `decode`): needs the carrier-return bridge above, or
  the SC7 phantom `Schema a` wrapper (whose runtime layout is `int`) so the
  decoded value is int-represented and the ABI is uniform.
- **RD** (`#json-str<T>` et al.): RD1 (capture `<Type>` and emit
  `(:: node Type)`) is independent and small; RD2+ desugar to `decode!`/`decode`
  and therefore inherit the SC5 status. The `<Type>` slot in
  `try_read_json` (`reader.c:1358`) is still parsed-and-ignored.
- **SC7** (phantom `Schema a` + Functor/Applicative/Alternative): unblocks SC5
  representationally (int-backed wrapper) and is the recommended next step.

---

## Why this is two compiler features, not just stdlib

SC5 (`HasSchema` + generic `decode`) and SC7 (`Functor`/`Applicative`/
`Alternative` for `Schema`) are blocked on capabilities the elaborator does
not have today. Naming them precisely keeps the plan honest:

1. **Return-type-directed typeclass dispatch (RT).** `HasSchema`'s methods put
   the dispatch type variable *only in the return position*:

   ```turmeric
   (defclass HasSchema a
     (schema-of [] : (Schema a)))     ; `a` appears only in the result

   (defn decode [raw : ptr<void>] : (Result a (Vec SchemaError))
     ...)                              ; `a` appears only in the result
   ```

   Today's dispatcher (`elab_typeclasses.c`, the loop at ~line 2461) builds a
   `TypeClassDispatchKey` from **`obj->type`** -- the first *argument's* type --
   and `typeclass_solve_constraints` (`typeclass.c:263`) resolves constraints
   from `arg_types` only. A method whose dispatch variable is in the return
   has no argument to key on, so it cannot be resolved. Worse, `elab_form` runs
   in pure **synthesis (infer) mode**: it takes no expected-type parameter, and
   `elab_ascribe` (`elab_types.c:1753`) elaborates the inner expression *first*
   and overrides its type afterward, so an enclosing `(:: e T)` or
   `let [x : T e]` never tells `e` what `T` is.

2. **Reader-table `<Type>` capture (RD).** The reader already *parses*
   `#json<Type>(...)` (`reader.c:1358-1377`) but **throws the type away**. The
   `#json-str<T>` / `#json-str?<T>` / `#json-file<T>` family from the schema
   plan needs the reader to (a) keep that type and (b) emit it as an ascription
   the RT machinery can consume.

RT is the load-bearing piece: once a call's result type can steer instance
selection, SC5 and SC7 are mostly ordinary stdlib plus a thin reader layer.

---

## Architecture today (grounding)

### Typeclass machinery

| Concern | Where |
|---|---|
| `TypeClass`, `TypeClassInstance`, `TypeClassMethod` structs | `src/compiler/typeclass.h:18-83` |
| Class/instance registry (`TypeClassEnv`) | `typeclass.h:86-90`; `e->typeclass_env` |
| `defclass` elaboration | `elab_typeclasses.c` `elab_defclass` (~779-985) |
| `definstance` registration | `typeclass_env_register_instance` (~1911) |
| **Method dispatch (arg[0]-keyed)** | `elab_typeclasses.c` ~2461-2600 |
| Constraint solving (arg-typed) | `typeclass.c:263` `typeclass_solve_constraints` |
| Dictionary node synthesis | `make_dict_expr` (~2005), `EX_DICT`, `dict_<Class>_<type>` |
| Per-fn constraint set | `FnDef.constraints` (`elab_fns.c:2567`); `call_.dict_arg` on `EX_CALL` |
| HKT two-level dispatch key | `typeclass.h:120-132` `TypeClassDispatchKey` (`constructor_kind`) |

Dispatch is **fully static**: a resolved instance becomes a direct C call;
constrained polymorphic functions receive a dictionary argument
(`call_.dict_arg`). There is no runtime vtable for the common case.

### Expected-type flow

There is **none, in general**. `elab_form(e, form)` returns an `Expr` whose
`->type` is synthesized bottom-up. The only places an "expected" type is
consulted are local, inside `elab_call.c` (`call_collect_type_bindings`,
~line 86) where a *function parameter's* declared type binds type variables
from *argument* expressions. That is exactly the wrong direction for RT.

### Reader machinery

| Concern | Where |
|---|---|
| Hardcoded `#`-dispatch | `reader.c::read_form` switch (~2540) |
| `#json(...)` + `<Type>` parse (type discarded) | `reader.c:1342-1405` `try_read_json` |
| Form builders | `form_sym`, `form_list`, `form_str`; `json_call` (`reader.c:1042`) |
| Ascription form `(:: expr type)` | `elab_types.c:1753` `elab_ascribe` -> `EX_ASCRIBE` |
| Flag `-Xjson-reader` | `main.c:4331,7250`; `g_json_reader_enabled`; json.tur autoload `main.c:709` |
| User reader-macro registry | `reader_macros.c` (`reader-macros/define`) |
| Reader-macro fixtures | `tests/fixtures/json-reader-*` with a `flags` file |

---

## Phase RT -- Return-type-directed dispatch

The goal: when a call's result type variable is determined by an **expected
type** at the use site, resolve the typeclass constraint from that expected
type and select the instance/dictionary accordingly.

We deliberately avoid a full bidirectional rewrite of the elaborator (every
`elab_form` gaining an `expected` parameter is a very large, regression-prone
change). Instead we add a **narrow, opt-in expected-type channel** that covers
the two syntactic positions schema users actually write.

### RT1 -- `where`-clause syntax + return-only constraint detection

There is no `where` token today. Add it as the way to attach a class
constraint to a `defn`:

```turmeric
(defn decode [raw : ptr<void>] : (Result a (Vec SchemaError))
  where (HasSchema a)
  ...)
```

- **Parse:** in `elab_fns.c`, after the return type, accept an optional
  `where` form (a symbol `where` followed by one or more `(Class tyvar)`
  clauses). Intern `where` once (mirror `e->sym_ascribe`). Populate the
  existing `FnDef.constraints` (`ConstraintSet`) -- the field already exists
  and is consumed by dictionary passing.
- **Classify each constraint** as *argument-resolved* (the tyvar appears in
  some parameter type -- today's path, unchanged) or *return-resolved* (the
  tyvar appears only in the return type). Record a flag per constraint
  (extend `TypeConstraint` with `bool return_resolved` or derive it on the
  fly). Return-resolved constraints are the new case RT2/RT3 handle.

**Backwards-compat:** absent a `where` clause, behavior is identical. No
existing fixtures use `where`, so codegen snapshots are unaffected.

### RT2 -- an expected-type channel into call elaboration

Add a single, explicit expected-type input rather than threading it through
every node. Two entry points feed it:

1. **Ascription `(:: e T)`** (`elab_ascribe`): before elaborating the inner
   form, push `T` onto a small expected-type stack on `Elab`
   (`e->expected_type`, a one-slot or arena stack). Elaborate the inner form,
   then pop. This is the minimal change to `elab_types.c:1767`.
2. **Typed `let` binding `let [x : T e]`**: where the binder carries a type
   ascription, push `T` while elaborating `e`. (Locate in the `let`
   elaborator; binding specs already parse the `: T` annotation.)

`elab_call.c` reads `e->expected_type` (and immediately clears it for nested
sub-calls, so it applies only to the *outermost* call of the ascribed
expression). Everything else ignores the channel -- it is null in the common
case, so there is no behavioral change where it is unused.

> **Design note.** We keep this as a context slot on `Elab` rather than a new
> parameter on `elab_form` specifically to bound the blast radius: only
> `elab_ascribe`, the `let` binder, and `elab_call` learn about it. If a future
> phase wants true bidirectional checking, this slot is the seam to generalize.

### RT3 -- resolve return-resolved constraints from the expected type

In `elab_call.c`, when the callee has a return-resolved constraint
(from RT1):

1. **Unify** the callee's declared return type (e.g. `(Result a (Vec
   SchemaError))`) against `e->expected_type` (e.g. `(Result User (Vec
   SchemaError))`) to bind the tyvar `a := User`. Reuse the existing
   type-binding collector (`call_collect_type_bindings`, `elab_call.c:86`),
   which already unifies a structured expected type against an actual and
   records tyvar bindings -- here applied to the *return* type instead of an
   argument.
2. **Look up the instance**: build a `TypeClassDispatchKey { typeclass =
   HasSchema, type_args = [User], n = 1, constructor_kind = KIND_STAR }` and
   call `typeclass_env_lookup_instance_by_key` (`typeclass.h:131`).
3. **Pass the dictionary**: set `call_.dict_arg` to the `EX_DICT` node for that
   instance via `make_dict_expr`, exactly as argument-resolved constraints do
   today.
4. **Diagnostics**: if `e->expected_type` is null (the result type is
   unknown), emit `TUR-E02xx: cannot infer type for return-directed
   constraint <Class> <tyvar>; add a type ascription, e.g. (:: <call> T)`. If
   no instance exists for the resolved type, emit the standard
   "no instance `<Class> <T>`" error (reuse the existing message path). The
   missing-instance case is a **compile-time** error, satisfying the SC5
   fixture `schema-decode-typed-missing-instance`.

### RT4 -- nullary / phantom-dispatch methods (`schema-of`)

`HasSchema`'s `schema-of` takes no value argument that carries `a` (it is
`[] : (Schema a)` or `[_ : a]` with an ignored witness). Two sub-cases:

- If the class method is **nullary in the dispatch type** (no parameter of
  type `a`), require it to be called only through `decode`/`decode!` or under
  an ascription, and resolve via RT3.
- Alternatively (simpler, recommended for v1) declare the method with an
  **ignored phantom witness** parameter typed `a` and have `decode` synthesize
  it as a null/`unit` value purely to drive argument-based dispatch -- making
  `schema-of` resolvable by the *existing* arg[0] path and confining the new
  RT machinery to `decode`/`decode!` alone. Pick this if RT3 proves fiddly for
  nullary methods; document the choice in the class docstring.

### RT testing

Error-path and behavior fixtures (compiled harness, `tests/fixtures/`):

- `rt-return-dispatch-basic` -- a tiny class `Default a (default-of [] : a)`
  with `int`/`cstr` instances; `(:: (default-of) cstr)` resolves the right
  instance. Proves RT independently of schema.
- `errors/rt-return-dispatch-unascribed` -- calling a return-dispatched fn
  with no ascription is the RT2/RT3 diagnostic.
- `errors/rt-missing-instance` -- ascribing to a type with no instance is the
  standard missing-instance error.

> **Risk register.** RT touches dispatch and dictionary passing -- the highest-
> value, highest-regression area of the compiler. Mitigations: (a) the
> expected-type channel is null everywhere it is not explicitly pushed, so all
> 1158 existing fixtures must stay green with zero snapshot churn; (b) gate the
> whole feature behind acceptance of the new `where` syntax -- code that never
> writes `where` never hits a new code path; (c) land RT1+RT2 (parse + channel,
> no resolution) first and confirm green, then RT3.

---

## Phase SC5 -- `HasSchema` typeclass + generic `decode`

With RT in place, SC5 is mostly stdlib in `stdlib/schema.tur`.

### SC5.1 -- the class and generics

```turmeric
(defclass HasSchema a
  (schema-of [_ : a] : int))      ; returns a schema value (see RT4 for the
                                  ; phantom-witness vs nullary choice)

(defn decode [raw : ptr<void>] : int
  where (HasSchema a)
  (schema-decode (schema-of (the-witness a)) raw))

(defn decode! [raw : ptr<void>] : int
  where (HasSchema a)
  (schema-decode! (schema-of (the-witness a)) raw))
```

`decode` returns a schema-decode `Result`; `decode!` panics. The `a` in the
result is resolved by RT3 from the binding/ascription. (`int` here is the
carried runtime representation; the *typed* surface comes from the ascription.)

### SC5.2 -- a derive helper for `defstruct`

Writing `schema-of` by hand is boilerplate. Provide a macro
`derive-schema` that, given a `defstruct` name and a field->schema mapping,
expands to the `definstance HasSchema T` with a `schema/object-new` +
`schema/field` chain:

```turmeric
(derive-schema User
  [["name" (schema/str)]
   ["age"  (schema/int)]])
```

This is a `defmacro` (no compiler change) emitting the SC0--SC1 builders.

### SC5.3 -- fixtures

- `schema-decode-typed-user` -- `defstruct User` + `HasSchema`; `(:: (decode
  (json/decode "...")) ...)` returns a value the program reads back. Asserts
  RT-driven dispatch end to end.
- `errors/schema-decode-typed-missing-instance` -- `decode` to a type with no
  instance is a compile-time error (RT3 diagnostic).

---

## Phase RD -- Reader-macro family (`#json-str<T>` et al.)

### RD1 -- capture the `<Type>` slot

In `try_read_json` (`reader.c:1358-1377`) the type name is already scanned but
discarded. Instead, intern it and **wrap the emitted node in an ascription**:

```
#json<User>({...})  ==>  (:: <existing json node tree> User)
```

The `<Type>` text is already validated as a single token; parse it into a type
Form (a symbol Form, or a list Form for applied types like `(Result User E)`
using the same `json`-side helpers plus `form_list`). Emit `(:: node Type)`
via `json_call`-style builders. With RT, the ascription drives `decode`
dispatch downstream; for bare `#json<T>` the ascription is also a useful
compile-time check.

### RD2 -- the runtime siblings

Add three new reader entries next to `try_read_json`. They share the
`<Type>(expr)` shape but differ in the inner payload:

| Reader form | Desugars to |
|---|---|
| `#json-str<T>(expr)`  | `(:: (decode! (json/decode expr)) T)` |
| `#json-str?<T>(expr)` | `(:: (decode  (json/decode expr)) T)` (returns the `Result`) |
| `#json-file<T>(path)` | `(:: (decode! (json/decode (io/read-file path))) T)` |

Unlike `#json`, the inner is an **ordinary Turmeric expression**, not a
verbatim JSON blob -- so the reader reads one balanced form with the normal
`read_form` (not `json_read_value`) between the parentheses, then builds the
desugared call. No JSON sub-parser is involved.

- Add `try_read_json_str`, `try_read_json_str_q`, `try_read_json_file`
  (or one parameterized helper taking the head symbol + whether to use
  `decode`/`decode!`/file). Register them in the `read_form` `#`-dispatch
  alongside `try_read_json` (`reader.c` ~2547), each gated on the flag.
- `#json-file` is lower priority; it is listed in the schema plan under
  "Future work." Implement `#json-str` / `#json-str?` first.

### RD3 -- flag + autoload

Reuse `-Xjson-reader` (it already gates `#json` and autoloads `json.tur`). The
new forms also need `schema.tur` and `result.tur` resolvable. Two options:

- **Reuse + extend autoload:** when `g_json_reader_enabled`, also ensure
  `schema.tur` is loadable. But schema.tur is intentionally *not* auto-loaded
  (snapshot hygiene). Prefer:
- **New flag `-Xschema-reader`** (recommended) that implies `-Xjson-reader`
  and additionally makes `schema.tur`/`result.tur` available for the desugared
  `decode` call. Add it in `main.c` next to the existing `-X` handlers
  (`main.c:4331`, `7250`) and a `g_schema_reader_enabled` global in
  `globals.h`. Gate the RD2 forms on this flag; keep `#json`/`#json<T>` on the
  original flag.

### RD4 -- fixtures

Mirror `tests/fixtures/json-reader-*`. Each carries a `flags` file with
`-Xschema-reader`:

- `schema-reader-json-str-runtime` -- `#json-str<User>(body)` with `body` a
  runtime `:cstr`; round-trips to a typed value (schema-plan SC5 fixture
  `schema-decode-json-str-runtime`).
- `schema-reader-json-str-result` -- `#json-str?<User>(bad-body)` returns an
  `err` with the expected field path; no panic (`schema-decode-json-str-result`).
- `errors/schema-reader-json-str-no-type` -- `#json-str<>(x)` is the
  empty-type diagnostic (reuse the existing TUR-E0270 path).

### RD5 -- docs

Extend `docs/guides/schema-guide.md` ("Not yet implemented" section becomes a
real section) and `docs/guides/json-guide.md` (the `#json<T>` slot is now
live). Note the `-Xschema-reader` flag and the `decode`/`decode!` panic-vs-
Result split.

---

## Phase SC7 -- `Functor` / `Applicative` / `Alternative` for `Schema`

SC7 needs a *type* the typeclass system can dispatch on. Today a schema is a
bare `:int`. Introduce a **phantom-typed wrapper** so HKT dispatch
(`TypeClassDispatchKey.constructor_kind == KIND_ARROW`, already supported)
can pick `Schema` instances.

### SC7.1 -- phantom `Schema a` type

```turmeric
(defstruct Schema [A] (raw :int))    ; A is phantom; raw is the SC0 schema ptr
```

All SC0 constructors return `(Schema a)` wrappers (or add thin wrappers so the
runtime layout is unchanged -- `raw` is the existing tagged pointer). The
decoder unwraps `.raw`. This is a non-breaking representational change:
`schema-decode` keeps taking the raw pointer; the wrapper exists for dispatch.

### SC7.2 -- the two trivial kinds

Add `SCHEMA_ALWAYS` (succeeds with a fixed value; backs `pure`) and
`SCHEMA_NEVER` (always fails; backs `empty`) discriminants and constructors
`schema/always` / `schema/never`, plus the decoder cases. These are pure SC0-
style additions to `stdlib/schema.tur`.

### SC7.3 -- `schema/ap` and `field`

- `schema/ap sf sa` -- **Validation** applicative: always decode *both* arms
  against the same input, concatenate their error vecs left-to-right, and only
  apply the decoded function to the decoded argument when both succeed. This is
  the same accumulation policy as SC3.
- `field "k" s` -- a `Schema` that decodes a whole object and extracts key
  `"k"` via schema `s`, so applicative chains build a struct field by field.

### SC7.4 -- the instances

```turmeric
(definstance Functor [Schema]
  (fmap [f s] (schema/transform s f)))

(definstance Applicative [Schema]
  (pure [v]    (schema/always v))
  (<*> [sf sa] (schema/ap sf sa)))

(definstance Alternative [Schema]
  (<|> [a b]   (schema/union (vec-of a b)))
  (empty []    (schema/never)))
```

Object schemas may then be written applicatively:

```turmeric
(definstance HasSchema User
  (schema-of [_]
    (<$> ->User (field "name" (schema/str))
         <*> (field "age" (schema/int)))))
```

### SC7.5 -- the deliberate `Monad` omission (docstring-mandated)

Per the schema plan, **do not** add `Monad Schema`. The lawful `>>=` for
Validation is fail-fast, which contradicts the accumulating `<*>` via
`ap = liftM2 ($)`. The `;;;` docstrings on `definstance Applicative [Schema]`
and on `schema/ap` must cover: (1) Validation semantics; (2) why `Monad` is
omitted; (3) the escape hatch (`schema/dispatch : Schema k -> (k -> Schema a)
-> Schema a`, or a distinct fail-fast `SchemaM` type) if monadic decoding is
ever needed; (4) the O(arms) performance note. Carry the same notes into
`docs/guides/schema-guide.md`.

### SC7.6 -- fixtures

- `schema-functor-transform` -- `fmap f s` == `schema/transform s f`.
- `schema-alternative-union` -- `<|>` == `schema/union` for two arms.
- `schema-applicative-user` -- build `User` via `<$>`/`<*>`; round-trip.
- `schema-applicative-error-accumulation` -- decoding `{"name": 42, "age":
  "x"}` yields **two** path-tagged errors, one per field.

---

## Suggested ordering

1. **RT1 + RT2** (parse `where`, add the expected-type channel; no resolution
   yet). Land green with zero snapshot churn.
2. **RT3 + RT4** (resolution + dictionary passing) with the `rt-*` fixtures.
3. **SC5** (class, `decode`/`decode!`, `derive-schema`, typed fixtures).
4. **RD1--RD5** (reader family + `-Xschema-reader` + fixtures + docs).
5. **SC7** (phantom `Schema`, ALWAYS/NEVER, `ap`/`field`, instances, docs).

RT is the long pole and the only deep compiler change; SC5/RD/SC7 are
incremental once it lands. Each phase ends with `bash tests/run.sh` showing
zero `FAIL` and -- because RT must not perturb existing programs -- zero
`expected.c` snapshot changes outside the new fixtures.

---

## Non-goals

- Full bidirectional type checking. RT adds a *narrow* expected-type channel,
  not general checking-mode elaboration.
- Runtime type tags. Decoding stays anchored on tagged JSON nodes (see the
  parent plan's "No runtime type tags" note).
- Serialization, `schema/coerce`, schema-gen, JSON-Schema import -- all remain
  in the parent plan's "Future work."

---

## Open questions

1. **`where` vs. inferred constraints.** Should return-only constraints be
   *inferred* from a tyvar appearing solely in the return type (no `where`
   needed), or always explicit via `where`? Explicit is safer for v1 (no
   silent dispatch changes); inference is more ergonomic later.
2. **Nullary method dispatch (RT4).** Phantom-witness parameter vs. true
   nullary resolution -- decide during RT3 implementation; the phantom-witness
   route is lower-risk.
3. **Flag surface.** One `-Xschema-reader` implying `-Xjson-reader`, or fold
   everything into `-Xjson-reader`? Separate flag keeps json-only users off
   the schema autoload path.
