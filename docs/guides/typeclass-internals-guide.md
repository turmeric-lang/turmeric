---
title: Typeclass Dictionary Internals
category: Compiler Internals
description: How `definstance` lowers to a C dictionary struct + singleton, how method-field C types are resolved, and the closure-handle convention for methods whose return type is itself a function.
---

# Typeclass Dictionary Internals

This guide documents how a `definstance` is lowered to C and, in particular, the
rule for resolving a method field's C type -- including the case that
historically miscompiled: a method whose **return type is a closure**.

## Dictionary lowering

Each `definstance C [T]` emits, at file scope (see
`src/compiler/emit_stmt.c`, `EX_INSTANCE_DEF`):

1. A **dictionary struct** `dict_<Class>_<TypeArgs>` with one function-pointer
   field per method. The field's C signature mirrors the method impl's emitted
   C signature: return type, then one parameter slot per method parameter
   (poly-fn params become `tur_poly_fn_t`; by-pointer structs become
   `const T *`).
2. A **global singleton** `dict_<Class>_<TypeArgs>_singleton` whose fields point
   at the per-instance method impls (`__inst_<Class>_<method>_<TypeArgs>`, or
   the binding name computed in `elab_definstance`).

Dispatch (`EX_DICT` with a method name, in `src/compiler/emit_expr.c`) loads the
function-pointer field and the indirect-call path casts it to
`ret_t (*)(...)` before invoking it. **The dict-field type, the impl-signature
type, and the call-site cast type must all agree** -- they are three views of
the same function pointer.

## Method-field C-type resolution

The field's return type is chosen by the same priority `emit_fns.c` uses for the
impl signature (`emit_stmt.c`):

1. `emit_carrier_return_override` -- the body returns a by-value struct that
   resolves a dispatch tyvar (e.g. `HasSchema decode -> User`): use the struct.
2. `binding->type.as.fn.result_full_type` -- the method's fn type carries a full
   return Type. **This is the load-bearing path for closure returns** (below).
3. `result_kind` via `emit_type_from_kind` -- primitives and bare references.
4. `body->type` fallback.

## The closure-handle convention

A first-class **closure value is carried as an `int64_t`** at runtime: a handle
to the heap fat-closure box `{ thunk, env... }` (or, for a captureless lambda, a
bare function pointer). This is the same handle `TUR_APPLY0/1/2/...` consume by
reading the thunk from slot 0. `type_c_name` lowers closure carriers
accordingly:

- A **boxed** `TY_FN` (a stored first-class closure) -> `void *`.
- A **bare** `TY_FN` reference -> its result type's C name; but a closure whose
  result is *itself a function* (a curried return, `(fn [int] (fn [int]
  int))`) or whose result is unknown -> **`int64_t`** (the handle). Recursing
  into the result kind there would bottom out at a zeroed `TY_FN` shell and emit
  an unknown-void carrier, silently dropping the handle.

### Why a closure-returning method needs `result_full_type`

When a method declares a function return type, e.g.

```turmeric
(defclass HasArr [a]
  (arr-of [self n : int] : (fn [int] int)))
```
```sweet-exp
defclass HasArr [a]
  (arr-of [self n : int] : (fn [int] int))
```

the instance-method elaborator (`elab_definstance`,
`src/compiler/elab_typeclasses.c`) builds the method's fn type with
`type_fn(param_kinds, n, return_type.kind)`. That stores only the return *kind*
(`TY_FN`) and **drops the full return Type**. Without the full type, codegen
falls back to `emit_type_from_kind(TY_FN)` -- a zeroed shell whose result kind is
`TY_UNKNOWN` -- and the dict field / impl signature lower to an unknown-void
carrier. The emitted impl then `return`s a closure handle from a `void`-returning
function: a silent miscompile.

The fix mirrors the regular `defn` path ("Issue 1b" in `elab_fns.c`): when the
declared return is a `TY_FN`, attach it as `result_full_type` so the carrier
resolves to the `int64_t` closure handle. Three sites must agree, and all three
now carry the handle:

| Site | File | Carrier |
| --- | --- | --- |
| dict struct field | `emit_stmt.c` (`EX_INSTANCE_DEF`) | `int64_t (*m)(...)` |
| method impl signature | `emit_fns.c` | `static int64_t __inst_...` |
| let-binding of the call result | `emit_expr.c` (`EX_LET`) | `int64_t v = (int64_t)(intptr_t)(...)` |

For a **curried** return (a closure returning a closure) the `result_full_type`
is a `TY_FN` whose result kind is again `TY_FN`; `type_c_name`'s curried-closure
rule (above) carries it as `int64_t` rather than recursing to unknown-void.

## Calling a closure returned from a method

The returned handle obeys the usual captureless-vs-fat distinction:

- A returned closure that **captures** free variables is a fat box -- apply it
  through `TUR_APPLY1(f, x)` (the standard protocol).
- A returned closure that **captures nothing** is a bare function pointer --
  apply it through a thin cast `((int64_t(*)(int64_t))(intptr_t)f)(x)`.

The `tests/fixtures/instance-closure-return-*` fixtures cover both, plus int and
struct captures, curried returns, and two methods composed at the call site.

## `definstance` is idempotent (reload-safe)

A `definstance C [T]` form is **idempotent**: re-running it -- whether at the
REPL, via `(reload)`, or by re-loading a module -- replaces the singleton entry
in the instance table rather than appending a duplicate. The most recent
elaboration wins. This means a typeclass-heavy session can call `(reload)`
freely without `instance already defined` errors and without ambiguous
resolution caused by stale duplicates.

## TUR-W0039: method name shadowing a free `defn`

A typeclass method name shares a global namespace with ordinary `defn`
bindings. When a `defn` and a `defclass` method use the same name in the same
scope, the compiler emits **TUR-W0039** at the shadowing site. Both bindings
coexist (the method resolves through the dictionary; the free `defn` is still
callable by its qualified name), but the warning flags the ambiguity for the
reader. Rename one of them when the shadowing is unintentional. See
[arrows-guide.md](arrows-guide.md) for the worked example that originally
surfaced the diagnostic.

## Associated type members

A class may declare **associated type members** alongside its value methods.
An associated type is a type-level projection from the instance type to another
type -- the Haskell `type Storage c = ...` / Rust `type Item;` pattern:

```turmeric
(defclass Container [t]
  (type Elem : Type)               ;; associated type member
  (make-empty [self : t] : int))   ;; ordinary value method

(definstance Container [(Vec int)]
  (type Elem = int)                ;; per-instance binding
  (make-empty [self] 0))

(definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0))
```
```sweet-exp
defclass Container [t]
  (type Elem : Type)               ;; associated type member
  (make-empty [self : t] : int)   ;; ordinary value method

definstance Container [(Vec int)]
  (type Elem = int)                ;; per-instance binding
  (make-empty [self] 0)

definstance Container [(Vec cstr)]
  (type Elem = cstr)
  (make-empty [self] 0)
```

In any type-annotation position, the projection `(Elem (Vec int))` resolves to
whatever the matching instance bound -- here `int`; `(Elem (Vec cstr))`
resolves to `cstr`:

```turmeric
(defn take [x : (Elem (Vec int))] : int (+ x 1))   ;; x : int
```
```sweet-exp
defn take [x : (Elem (Vec int))] : int {x + 1}   ;; x : int
```

Rules and representation:

- **Parsing.** `(type Name : Type)` in a `defclass` body is an associated type
  member; `(type Name = <type>)` in a `definstance` body binds it. The
  discriminator is a `type` head whose second element is a bare symbol (a
  method named `type` carries a `[params]` vector instead, so it is never
  misclassified). Members are stored on `TypeClass.assoc_type_names`; bindings
  on `TypeClassInstance.assoc_types` (`src/compiler/typeclass.h`).
- **Every member must be bound.** A `definstance` that omits a member, or binds
  a name the class never declared, is a hard error reported on the instance.
- **Resolution is precise.** `(Name T...)` matches the instance whose type
  arguments are structurally equal to the projection's arguments (an exact
  N-ary match, `typeclass_env_lookup_instance_exact`). This is *not* the
  kind-erased dispatch `typeclass_env_lookup_instance` (which cannot tell
  `Vec int` from `Vec cstr`); associated-type projection has its own
  `typeclass_env_resolve_assoc_type` / `typeclass_env_resolve_assoc_type_n`.
- **Erased.** Associated types live only in the elaborator's type-annotation
  parsers (`type_expr_from_form` and the `defn` signature parser
  `fn_type_from_form`, which must carry its own copy of the hook because it
  builds type applications without delegating). There is no dictionary field
  and no codegen -- zero runtime cost.
- **Scope.** Multi-parameter classes are supported: a projection names every
  class parameter (`(Acc (Vec int) int)`) and resolves by the exact N-ary
  match above. One output type per member; no type-level arithmetic.
  Functional dependencies are a separate mechanism, declared with the
  `| (a -> b)` clause on the class head (see
  [typeclass-guide.md](typeclass-guide.md#functional-dependencies)).

## See also -- the arrow / `Category` hierarchy

The arrow surface in `stdlib/arrow.tur` and `stdlib/kleisli.tur` is a
production-grade example of the dictionary-lowering rules above: `Category`
(the superclass of `Arrow` and `ArrowZero`), `Arrow`,
`ArrowChoice`, `ArrowZero`, and a
second
`Category` instance (`Kleisli`) all share method names and exercise both the
closure-handle convention and the method-vs-defn namespace rules. See
[arrows-guide.md](arrows-guide.md).

## See also

- [closure-returning-instance-method-codegen-plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/closure-returning-instance-method-codegen-plan.md)
  -- the plan this rule was extracted from.
- [`docs/archive/history/nested-closure-transitive-capture.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/nested-closure-transitive-capture.md) -- an *orthogonal*
  capture-set defect (a grandchild closure's free var not threaded through the
  middle closure), independent of the carrier type.
- [`docs/archive/history/intra-instance-method-dispatch-unsupported.md`](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/intra-instance-method-dispatch-unsupported.md) -- calling a
  sibling method via `(.other self ...)` inside an instance body.
