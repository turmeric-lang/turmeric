---
title: Curried + Closure Call Cast Rough Edges Plan
category: Planning
description: Two related codegen bugs surfaced by the httpd middleware plan -- a partial-application wrapper omits the `(intptr_t)` cast when storing a bare-defn argument into its env, and a let-bound closure call omits the same cast when passing a `:ptr<void>` value into an `:int` parameter. Both fail with clang `-Wint-conversion`.
---

# Curried + Closure Call Cast Rough Edges -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Compiler bug fix (codegen) -- two sibling issues
> **Related:**
> - `src/compiler/elab_call.c:1310-1332` -- PAP capture-binding construction (Issue A site)
> - `src/compiler/emit_expr.c` EX_CALL closure-thunk emission path (Issue B site)
> - `docs/archive/history/variadic-rest-closure-cast-plan.md` -- the original "missing
>   (intptr_t) cast" plan that fixed EX_CONS_LIST; this plan is the same
>   family of bug in two other emit sites.
> - `docs/archive/history/defstruct-inline-c-byvalue-callsite-plan.md` -- the
>   companion call/formal-ABI sync plan; same general "two emitters need to
>   agree" pattern.
> - `docs/archive/history/httpd-middleware-async-plan.md` -- discovered during PR 5
>   (M4 CORS + M5 Basic Auth); worked around by avoiding partial application
>   and direct closure-value composition in the M5 fixture.

---

## Symptoms

Two distinct compile failures, each from a small Turmeric program that
the language otherwise reads as valid.

### Issue A -- PAP env init missing `(intptr_t)` cast on a bare-defn capture

```turmeric
(defn callback [u : cstr p : cstr] : int 1)
(defn use-fn [realm : cstr verifier : int next : int] : int next)

(defn main [] : int
  (let [p (use-fn "x" callback)]    ;; partial application captures callback
    (p 0))
  0)
```

clang refuses:

```
error: incompatible pointer to integer conversion initializing 'int64_t'
(aka 'long long') with an expression of type
'int64_t (const char *, const char *)' (aka 'long long (const char *, const char *)')
[-Wint-conversion]
int64_t __papc862_863 = callback;
        ^               ~~~~~~~~
```

The partial-application wrapper (`__papN`) captures `callback` (a bare
top-level defn -- so a *function pointer* at the C level) into its env
struct's `int64_t` field without an intermediate `(intptr_t)` cast.

### Issue B -- Let-bound closure call missing cast for `void *` -> `int64_t`

```turmeric
(defn print-int [v : int] : nil
  ```c printf("v=%d\n", (int)v); ```)

(defn main [] :int
  (let [_tag 42
        wrap (fn [n :int] :ptr<void>
               (let [_t _tag]
                 (print-int (+ n _t))
                 ```c return NULL; ```))
        base (fn [c : ptr<void>] : nil
               (let [_t _tag]
                 ```c (void)c; ```))
        r    (wrap base)]          ;; void* base -> int n
    0))
```

clang refuses:

```
error: incompatible pointer to integer conversion passing 'void *'
to parameter of type 'int64_t' (aka 'long long') [-Wint-conversion]
void * r_872 = __fn_858(wrap_863, base_871);
                                  ^~~~~~~~
note: passing argument to parameter 'n' here
static void * __fn_858(void * __env_p_861, int64_t n) { ... }
```

`wrap` is a let-bound closure with thunk `__fn_858`.  The thunk's
second parameter is `int64_t n` (matching `wrap`'s declared `n :int`).
The call site emits `__fn_858(wrap_env, base)`, passing `base` (a
`void *` -- it is itself a let-bound closure value) into the int64_t
slot.  The implicit `(intptr_t)` coercion the fat-closure ABI uses
everywhere else is missing here.

---

## Why this matters

Both bugs block the most idiomatic ways to compose middleware:

- **Issue A** kills `(compose-middleware base (mw-basic-auth realm verify))`
  -- the partial-application path that lets a user configure a
  middleware in one expression and slot it straight into a chain.
  PR 5 of the httpd plan worked around it by calling `mw-basic-auth`
  with all three args directly and skipping `compose-middleware`.
- **Issue B** kills `(compose-middleware base let-bound-closure)` and
  `compose-middleware-of` callsites where a closure value participates
  as either the chain head or a middleware factory result.  PR 5 worked
  around it by manual nesting; PR 8 worked around it by going through
  `compose-middleware-of` with fat-closure middlewares only.

Both are in the same family as the variadic-rest cast bug fixed by V1
of the rest-closure plan, and the defstruct-by-value bug fixed by DS1
of the inline-C-callsite plan: **a codegen emitter knows the source
value is a pointer and the destination is an `int64_t`, but forgets
the standard `(int64_t)(intptr_t)(...)` coercion.**

Until they ship, every middleware factory that takes config has to
choose between (a) restructuring as an `n-arg` defn called fully
saturated each time, or (b) building manual closures with let-captured
primitives only.  Both work; both are uglier than the partial-app
shape the rest of the language pushes you toward.

---

## Root causes

### Issue A -- elab_call.c PAP capture-binding type

`src/compiler/elab_call.c:1310-1332` builds the PAP wrapper's env
struct.  For each provided arg, it interns a `__papcN` capture binding
and types it via `type_from_kind(cap_kind)`:

```c
TypeKind cap_kind = fn_type.as.fn.arg_kinds[fn_is_closure ? (i + 1) : i];
Type cap_type = type_from_kind(cap_kind);
Binding *cap_b = binding_new(e, cap_sym, cap_type, false, false, call->span);
```

For a primitive arg (int, bool) this produces the right Turmeric type
and the env field is `int64_t` (or whatever the kind says).  When the
arg is a bare top-level *defn* used as a value, the codegen elsewhere
already knows to emit a function pointer; but the *PAP env field*
emitter pastes the raw expression into the int64_t init slot without
the `(intptr_t)` coercion the rest of the call boundary applies.

The same `__papN` env init then propagates the missing cast forward;
when the wrapper later forwards the captured value to the underlying
callee, that callee gets the bit-pattern of a function pointer
implicitly cast through int64_t (which works at runtime), but the
compiler never sees the cast and rejects the assignment.

### Issue B -- emit_expr.c closure-thunk EX_CALL emission

The EX_CALL emitter has a special path for calling a let-bound closure
value: it reads `wrap`'s fat closure pointer, threads it as `self` (env)
to the thunk, and calls the thunk with the user-supplied args.  Each
thunk parameter slot has a declared C type (`int64_t n` for `(n :int)`,
`void *c` for `(c :ptr<void>)`, etc.).  The emitter pastes each arg's
emitted value into the call **without** running it through the same
"is destination `int64_t`?  cast via `(int64_t)(intptr_t)(...)`"
adapter that fixed-arity defn calls use.

For an arg whose source type is `:ptr<void>` (a closure value, the
result of `make-struct`, an opaque handle, etc.) being passed into an
`:int` thunk parameter, that adapter is exactly the missing piece.

The shape of the fix is symmetric: when the actual is a `void *` (or
otherwise a non-integer scalar) and the formal is `int64_t`, wrap with
`(int64_t)(intptr_t)`.

---

## Fixes

### Issue A -- emit the cast on PAP env init

Two implementation shapes; either is fine.

**Option A1 -- Cast at the env init site.**
Find where `__papN` env struct fields are emitted (downstream of
`elab_call.c:1310-1332`, in the emit phase that walks the PAP
metadata) and wrap each capture value with `(int64_t)(intptr_t)(...)`
unconditionally.  Same idiom as the V1 fix at `EX_CONS_LIST`.

**Option A2 -- Cast at the place the env is initialized in C.**
A slightly different spot but the same effect: in the generated `__papN`
constructor function, wrap each capture argument before writing it to
the env slot.

Recommendation: Option A1.  The single emit site that pastes the value
into `int64_t __papcN = <expr>;` is the natural place; matches the V1
shape.

### Issue B -- emit the cast on closure-thunk EX_CALL args

In the closure-thunk emit path inside `emit_expr.c::EX_CALL`, before
pasting each actual into the thunk-arg list, ask: "is the formal an
`int64_t`-shape and the actual a non-int scalar?" -- and wrap with
`(int64_t)(intptr_t)(...)` when yes.

Conservatively, the cast is a no-op for already-int actuals (the
compiler accepts redundant casts).  An unconditional wrap (same as
Option A in the variadic-rest plan) is the simplest viable fix.

Recommendation: unconditional wrap.  This is also the variadic-rest
plan's V1 shape, and a sibling fix.

---

## Phases

### Phase CC0 -- Reproducer fixtures

Two minimal fixtures pinning each bug:

- `tests/fixtures/curried-call-pap-cast/` -- exactly the Issue A program above.
- `tests/fixtures/closure-thunk-arg-cast/` -- exactly the Issue B program above.

Under the bug they go in `tests/fixtures/errors/*-mismatch/` with
`expected.diag` matching the clang error string; after the fix they
move to the happy suite with the expected stdout `v=42` (for B) and
`ok` or similar (for A).

### Phase CC1 -- Apply Issue A cast at PAP env emit

One-line addition to the PAP env init emitter.  Verify:

- `bash tests/run.sh` -- zero `FAIL` lines.
- The CC0 A fixture flips to happy.
- M5 basic auth's `httpd-mw-basic-auth` fixture continues to pass via
  its existing direct-call workaround; the partial-app form
  `(compose-middleware base (mw-basic-auth realm verify))` now also
  compiles (add an A2 fixture to verify, if desired).

### Phase CC2 -- Apply Issue B cast at closure-thunk EX_CALL

One-line addition to the closure-thunk EX_CALL arg emit.  Verify:

- `bash tests/run.sh` -- zero `FAIL` lines.
- The CC0 B fixture flips to happy.
- M4 / M5 fixtures continue to pass; the macro-driven
  `compose-middleware base (mw-cors-with ...)` shape now works directly,
  removing the partial-app workaround from PR 5.

### Phase CC3 (optional) -- audit other emit paths for the same family

The pattern "actual is `:ptr<void>` / function pointer, formal is
`int64_t`, codegen pastes the value verbatim" surfaced **three** times
in the httpd plan (variadic rest -- fixed by V1; closure thunk call;
PAP env init).  Spot-check the remaining call boundaries:

- `EX_CALL` against a generic / typeclass thunk
- Closure constructor env field initialization for `:ptr<void>` captures
  (a *separate*, sister bug that emits `void` instead of `void *` in
  the env struct; see "Out of scope" below)
- Reactor / fiber-driver synthetic closure construction (`tur_local_spawn`
  callsites in stdlib)

Resolve any additional reproducers as their own sibling fixes.

---

## Out of scope (related but separate)

While re-reproducing Issue B in current main, a sibling bug surfaced:
when a let-bound `(fn [c :ptr<void>] :nil ...)` closure value is
captured by another closure, the captor's env struct is emitted with
the captured-field type as `void` (not `void *`):

```c
struct __env_866 { tur_thunk_void___int64_t_t __fn; void base; };
```

This is a closure-env codegen issue, not a call-emit issue, and worth
its own focused plan.  It's surfaced by the same idiom (manually-
chained closures used in middleware) but the fix lives elsewhere in
the codebase.  Cross-link from CC3 once filed.

---

## Risk

- **Low for both.**  The fix is to add a coercion that the compiler
  accepts as a no-op for the existing passing cases and is the needed
  coercion for the failing cases.  Same risk profile as the V1 fix in
  the variadic-rest plan.
- **Snapshot churn.**  Likely small.  No existing stdlib fixture
  exercises a PAP wrapping a bare defn, nor a let-bound closure
  receiving a `:ptr<void>` value into an `:int` slot (which is exactly
  why the bugs slipped through).  The CC0 fixtures are net-new.

---

## Open questions

1. **Where exactly does the PAP env init get emitted in C?**  The
   `__papcN` binding is built in `elab_call.c`; the env *struct*
   declaration and the per-field init lines are emitted later in the
   pipeline.  Confirm the file and emit site when implementing CC1.
2. **Are there closure-thunk variants the EX_CALL cast does not
   reach?**  E.g., the rank-2 polymorphic call path may route through
   a different emitter.  CC3 covers the audit; flag if the spot fix is
   tighter than the audit suggests.
3. **Does the unconditional `(int64_t)(intptr_t)(...)` wrap ever
   change observed semantics for non-pointer non-integer types** (e.g.
   `:bool` or `:float`)?  Likely no -- int casts of an `int64_t` rvalue
   are no-ops -- but worth a fixture in CC2 that exercises a `:float`
   thunk arg to be sure.
