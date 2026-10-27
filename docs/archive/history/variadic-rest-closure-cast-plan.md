---
title: Variadic Rest Missing Function-Pointer Cast Plan
category: Planning
description: Codegen bug fix -- `EX_CONS_LIST` (the variadic-rest collector) emits `__tur_cons_of(head, tail)` with no `(int64_t)(intptr_t)` cast on `head`, so passing a function-pointer-shaped value (bare defn name, capture-less `(fn ...)` literal) as a rest arg fails to compile under `clang -Wint-conversion`.
---

# Variadic Rest Missing Function-Pointer Cast -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Compiler bug fix (codegen)
> **Related:**
> - `src/compiler/emit_expr.c:4116-4132` -- `EX_CONS_LIST` emitter (the bug)
> - `src/compiler/emit_module.c:1787-1795` -- `__tur_cons_of` definition
> - `src/compiler/elab_fns.c:2205,2695` -- `g_has_variadics` flips for typed rest
> - `docs/upcoming/httpd-middleware-async-plan.md` -- discovered while shipping
>   `compose-middleware` (M8 core); worked around by going macro-only
> - CLAUDE.md "Function Arity Style Guide -- Genuine variadic interfaces" -- documents
>   the typed-rest contract this bug breaks

---

## Symptom

A function-pointer-shaped value passed as a variadic rest arg fails the C
compile with `-Wint-conversion`. Concretely, a defn whose rest type is a
type variable bound to a callable type:

```turmeric
(defn compose [A] [base : int & xs : A] : int ...)

(defn mw-outer [n : int] : ptr<void> ...)
(defn mw-inner [n : int] : ptr<void> ...)

(compose base mw-outer mw-inner)   ;; rest args 1, 2 are bare defn names
```

emits, at the call site:

```c
compose((int64_t)(intptr_t)(base), __tur_cons_of(mw_outer, __tur_cons_of(mw_inner, 0LL)));
```

The fixed-arity `base` parameter receives a proper `(int64_t)(intptr_t)`
cast, but the cons-list collector pastes each rest arg's emitted value
verbatim into `__tur_cons_of(head, tail)`. Since `mw_outer` is a
`void *(*)(int64_t)` and `__tur_cons_of` expects `int64_t h`, clang refuses:

```
error: incompatible pointer to integer conversion passing
'void *(*)(int64_t)' to parameter of type 'int64_t'
[-Wint-conversion]
__tur_cons_of(outer_mw, __tur_cons_of(mid_mw, __tur_cons_of(inner_mw, 0LL)))
              ^~~~~~~~
```

This is loud (compile-time stop, not silent miscompile), but it blocks any
genuinely-variadic interface that wants to take callable values at runtime.

## Why this matters

The typed-rest contract (CLAUDE.md, "Function Arity Style Guide") promises
that `& rest :T` accepts any value whose type unifies with `T`, including
user-defined types and polymorphic `:A`. Closures are first-class values
elsewhere -- they pass through `httpd-call`, `httpd-new`, channel sends,
`reactor-add-fd` callbacks. The variadic rest collector is the one place
that breaks the abstraction.

Workarounds available today:
- **Macro instead of function.** `compose-middleware` shipped as a macro
  (`(compose-middleware base mw1 mw2 mw3)` -> `(mw1 (mw2 (mw3 base)))`) --
  cleaner for statically-known chains but cannot vary by runtime data.
- **Wrap each callable in a fat closure that captures something.** Forces
  every call site to write `(let [_x 0] (fn [n] ...))` boilerplate just to
  promote a thin function to a fat closure.

Once the cast lands, idiomatic shapes light up:

```turmeric
;; runtime pipeline composition
(let [stack (effect-stack handler-a handler-b handler-c)] ...)

;; parallel await groups (Track A of the httpd middleware/async plan)
(let [results (parallel-await req1 req2 req3)] ...)

;; tap fan-out
(broadcast value sink-a sink-b sink-c)
```

None of these need a macro; all of them want to take a variable number of
callable values chosen at runtime.

## Root cause

`src/compiler/emit_expr.c:4116-4132`:

```c
case EX_CONS_LIST: {
    uint32_t n = e->as.cons_list_.n;
    if (n == 0) return strdup("0LL");
    char *tail = strdup("0LL");
    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        char *head = emit_value(ctx, body, e->as.cons_list_.items[i]);
        Buf cell; buf_init(&cell);
        buf_printf(&cell, "__tur_cons_of(%s, %s)", head, tail);  // <-- no cast
        ...
    }
    return tail;
}
```

`emit_value` returns the C expression for the rest item *at its native C
type*. For `:int`, that's already `int64_t` -- no cast needed. For an
opaque/handle that's stored as `int64_t`, same. For a function-pointer
type (bare top-level defn name or a capture-less `fn` literal that the
compiler decided to emit thin), the C type is the function-pointer type
itself, and pasting it into the first parameter of `__tur_cons_of(int64_t,
int64_t)` is the `-Wint-conversion` we see.

Every other call boundary in the codegen goes through a coercion-emit path
that knows when to insert `(int64_t)(intptr_t)`. The cons-list collector
short-circuits that path.

## Fix

Insert the same coercion `EX_CONS_LIST` already needs at the call boundary.
The element's source-language type is available on `e->as.cons_list_.items[i]`
(or, if not directly, via the AST node's `type` field that elaboration
already attached). Two implementation shapes worth considering:

### Option A -- Always cast through `(int64_t)(intptr_t)`

Wrap every `head` value unconditionally:

```c
buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
```

- **Pros:** one-line fix; no type inspection needed; matches the pattern at
  fixed-arity call sites where the int/ptr<void> coercion is applied
  unconditionally.
- **Cons:** for an `int64_t` head that's already an `int64_t` rvalue (the
  common case -- numeric literal, `i+1`, struct field), the cast is a
  no-op but C still requires the operand to be an integer or pointer type.
  Casting `(int64_t)(intptr_t)(5LL)` is legal but reads odd, and may trip
  `-Wuseless-cast` style lints if anyone turns them on.

### Option B -- Cast only when the element type is non-integer

Inspect the source-language type of `e->as.cons_list_.items[i]` and emit:

```c
if (item_type_is_function_or_ptr(t)) {
    buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
} else {
    buf_printf(&cell, "__tur_cons_of(%s, %s)", head, tail);
}
```

- **Pros:** generated C reads cleanly for int/opaque rest args; explicit
  about why the cast is there when it appears.
- **Cons:** needs an `item_type` accessor on `EX_CONS_LIST` items (probably
  already present; elaboration types every expression). Slightly more
  surface to test.

**Recommendation: Option A.** The variadic rest already commits to storing
heterogeneous types as `int64_t` via `__tur_cons_of`; a uniform cast at the
collector is the simpler invariant. It also matches the cast pattern at the
fixed-arity call boundary, so the bug class doesn't return if a new value
kind is added later.

## Phases

### Phase V0 -- Reproduce as an error fixture

Land a positive fixture before changing the codegen, so the bug is pinned:

```
tests/fixtures/variadic-rest-closure/
  input.tur
  expected.stdout   ;; should be the success output AFTER the fix
```

`input.tur`:

```turmeric
(defn callable-a [n : int] : int (+ n 1))
(defn callable-b [n : int] : int (* n 2))

(defn first-of [A] [& xs : A] : A
  ;; Pull the head out of the rest list via the documented cons-cell shape.
  ```c
  typedef struct { int64_t head; int64_t tail; } __tur_cons_cell;
  __tur_cons_cell *p = (__tur_cons_cell *)(intptr_t)xs;
  return p ? p->head : 0LL;
  ```)

(defn main [] : int
  ;; Before the fix: clang rejects with -Wint-conversion here.
  (let [fns (first-of callable-a callable-b)]
    (println "ok"))
  0)
```

Until the fix lands, this fixture is the negative reproducer (gated under
`tests/fixtures/errors/variadic-rest-closure-cast/` with an `expected.diag`
matching the clang error string). After the fix, it moves into the happy
suite with `expected.stdout = ok`.

### Phase V1 -- Apply the cast in the EX_CONS_LIST emitter

Single-line change at `src/compiler/emit_expr.c:4124`:

```diff
-buf_printf(&cell, "__tur_cons_of(%s, %s)", head, tail);
+buf_printf(&cell, "__tur_cons_of((int64_t)(intptr_t)(%s), %s)", head, tail);
```

Confirm:

- `bash tests/run.sh` -- zero `FAIL` lines, including all 1219 existing
  passes and the V0 fixture flipping to the happy column.
- Regenerate any `expected.c` codegen snapshots that contain variadic call
  sites; commit alongside the codegen change (per CLAUDE.md fixture policy).
  The expected drift is small: every `__tur_cons_of(X, ...)` becomes
  `__tur_cons_of((int64_t)(intptr_t)(X), ...)`.

### Phase V2 -- Convert `compose-middleware` back to a function (optional)

Once V1 lands, `compose-middleware` in `stdlib/httpd.tur` can move from
macro to defn:

```turmeric
(defn compose-middleware [A] [base : int & mws : A] : int
  (httpd-mw-fold base mws))
```

with `httpd-mw-fold` (already present as the planned internal helper, but
removed during the macro pivot) re-introduced to walk the cons list and
apply each fat-closure middleware via the standard ABI.

Trade-off worth thinking through before doing this:

- **Pro:** runtime-built middleware lists become possible
  (`(compose-middleware base (if dev? mw-log 0) mw-cors)`).
- **Con:** loses the static-callability constraint the macro enforces.
  The macro form catches "you passed a non-callable" at elaboration; the
  function form pushes it to a runtime BUS error on first request.

Recommended path: ship both. Keep the macro as `compose-middleware-static`
for the common compile-time case; introduce the function as
`compose-middleware-of` for runtime-built chains. Names TBD; the point is
that V2 is *enabled* by V1, not forced by it.

## Out of scope

- Generalising the cast to other AST emit sites. Audit suggests
  `EX_CONS_LIST` is the only site that bypasses the call-boundary coercion;
  if any other emerges, file separately.
- Changing the cons-cell representation. `__tur_cons_cell { int64_t head;
  int64_t tail; }` is the documented public contract (CLAUDE.md "Cons-list
  manipulation in `#{Unsafe}` code"); we're not touching it.
- Polymorphic-rest typecheck behaviour. The typechecker correctly accepts
  the closure type as `A`; only the codegen step misses the cast. No
  elaboration-side changes are part of this plan.

## Risk

- **Low.** The cast is a no-op for integer rvalues (the existing case) and
  a needed coercion for pointer-ish rvalues (the broken case). It cannot
  introduce a regression for code that already compiles.
- **Snapshot churn.** Every variadic call site appears in at least one
  `expected.c` fixture. Re-run the snapshot regeneration loop from
  CLAUDE.md and commit in one go.

## Open questions

1. Does `emit_value` ever return an lvalue expression for a cons-list item?
   If so, the `(int64_t)(intptr_t)(...)` wrap is still well-formed, but
   worth a glance at the surrounding `emit_value` call sites to confirm we
   don't double-cast.
2. Should the variadic typechecker emit a friendlier error when someone
   declares `& xs :int` and passes a closure -- pointing them at `:A`?
   Probably yes, but it's a UX follow-up, not part of this bug fix.
