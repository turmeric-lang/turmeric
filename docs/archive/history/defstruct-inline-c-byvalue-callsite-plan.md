---
title: Defstruct By-Value Param + Inline-C Body -- Callsite Mismatch Plan
category: Planning
description: Codegen bug fix -- when a defn with an inline-C body declares a `>16-byte` defstruct parameter, the formal is emitted by value (correctly, so inline-C uses `opts.field`), but the call-site emitter still applies the large-struct pass-by-pointer transform and passes `&temp`, producing a `T *` vs `T` type mismatch that clang refuses.
---

# Defstruct By-Value Param + Inline-C Body -- Callsite Mismatch -- Plan

> **Status:** Draft Plan
> **Last Updated:** 2026-06-02
> **Type:** Compiler bug fix (codegen)
> **Related:**
> - `src/compiler/emit_fns.c:423` -- formal-parameter ABI selection (correctly skipped for inline-C bodies)
> - `src/compiler/emit_expr.c:2067-2083` -- call-site `&temp` pass-by-pointer transform (the bug)
> - `src/compiler/types.c:2785-2806` -- `type_struct_pass_by_ptr` (drives both decisions; >16 byte threshold)
> - `src/compiler/emit_fns.c:377` -- `body_is_inline_c` predicate that the formal side already uses
> - `docs/upcoming/httpd-middleware-async-plan.md` -- discovered while shipping the M2 cookie middleware (`CookieOpts` defstruct); worked around by going with positional args
> - `docs/upcoming/variadic-rest-closure-cast-plan.md` -- sibling codegen bug surfaced during the same plan

---

## Symptom

A defn whose body is inline-C and whose parameter is a defstruct larger than
16 bytes fails to compile:

```turmeric
(defstruct CookieOpts
  [name : cstr value : cstr path : cstr domain : cstr
   max-age : int secure : int http-only : int same-site : cstr])    ; 64 bytes

(defn cookie [name : cstr value : cstr] : CookieOpts
  (make-struct CookieOpts name value "/" "" -1 0 1 "Lax"))

(defn httpd-set-cookie! [conn : ptr<void> opts : CookieOpts] : nil
  ```c
  /* opts.field access -- by-value semantics inside inline-C */
  fprintf(stdout, "%s=%s\n", opts.name, opts.value);
  ```)
```

At a call site:

```turmeric
(httpd-set-cookie! c (cookie "session" "new"))
```

clang refuses the generated C:

```
error: passing 'CookieOpts *' (aka 'struct CookieOpts *') to parameter of
incompatible type 'CookieOpts' (aka 'struct CookieOpts'); remove &
httpd_set_cookie_((void *)(intptr_t)(c), &__t2);
                                         ^~~~~
note: passing argument to parameter here
static void httpd_set_cookie_(void *, CookieOpts);
                                                ^
```

The function is declared by value (matching its inline-C body's
`opts.name` access pattern), but the call site passes the address of a
temp. Type mismatch.

## Why this matters

Defstructs >16 bytes are common at the surface of any "options bag" API
(the canonical Turmeric solution to >5-param defns, documented in
CLAUDE.md). Any such API written with an inline-C body for the consumer
side -- which is the natural choice for stdlib code that bridges to C
libraries or hand-written serialisation -- hits this bug. Concretely it
blocked the M2 cookie middleware's `CookieOpts` API
(`docs/upcoming/httpd-middleware-async-plan.md` phase M2) and forced a
fall-back to 9 positional parameters. Future phases of that plan likely
hit it again:

- M4 CORS -- `CorsOpts` defstruct: `allow-origin`, `allow-methods`,
  `allow-headers`, `expose-headers`, `allow-credentials`, `max-age` (6
  fields, ~48 bytes including cstrs). Consumer inline-C wants
  `opts.allow_origin` etc. to format the response headers.
- M7 multipart -- `Part` defstruct: `name`, `filename`, `content-type`,
  `data`, `data-len` (~40 bytes). Consumer inline-C reads the data
  pointer + length.

The pattern is *the* idiomatic shape the language pushes you toward for
configurable middleware. Until this is fixed, every such API has to choose
between a positional-arg API (loses self-documenting field names) and a
defstruct API with a *Turmeric-language* (non-inline-C) consumer
(awkward when the consumer wants to call into a C library directly).

## Root cause

The formal-parameter ABI emitter (`emit_fns.c:423`) and the call-site
argument emitter (`emit_expr.c:2067-2083`) **disagree** on whether a
`>16-byte` defstruct parameter is by value or by pointer.

### Formal side -- emit_fns.c:423

```c
bool body_is_inline_c = (fd->body && fd->body->kind == EX_INLINE_C);
...
if (!fd->closure && !body_is_inline_c && type_struct_pass_by_ptr(param_ty)) {
    buf_printf(file, "const %s *", emit_type_c_name(ctx, param_ty));
} else {
    buf_puts(file, emit_type_c_name(ctx, param_ty));    /* by value */
    ...
}
```

When the body is inline-C, the formal is emitted as `T` (by value). This
is intentional and correct -- inline-C bodies read fields with `opts.field`
syntax that requires the value-typed parameter. Existing call sites like
`FileHandle` rely on this (its `ptr` field access in
`file-handle-ok?` is `fh.ptr`, only well-formed when `fh` is a struct
value).

### Call side -- emit_expr.c:2067

```c
if (!needs_fn_cast && !fn_binding->is_extern_c &&
    !matched_spec &&
    _callee_pbp &&
    type_struct_pass_by_ptr(e->as.call_.args[i]->type) &&
    !expr_is_pbp_param(ctx, emit_arg)) {
    char *_tmp = fresh_tmp(ctx);
    indent_buf(body, ctx->indent);
    buf_printf(body, "%s %s = %s;\n",
               emit_type_c_name(ctx, e->as.call_.args[i]->type), _tmp, raw);
    free(raw);
    Buf _ab; buf_init(&_ab);
    buf_printf(&_ab, "&%s", _tmp);    /* take address-of */
    ...
}
```

`_callee_pbp` is computed earlier by asking `type_struct_pass_by_ptr` on
the callee's declared parameter type -- a *type-only* decision. It does
not consult whether the callee's body is inline-C. So the call site
always emits `&temp` for >16-byte struct args even when the callee was
emitted by value.

`FileHandle` accidentally avoids this because it is 8 bytes (one
`void *`), so `type_struct_pass_by_ptr` returns false. The bug only
appears for structs that cross the 16-byte threshold AND have an
inline-C consumer.

## Fix

The formal side already chose the by-value ABI; the call site must
respect that choice. Two implementation shapes:

### Option A -- Call site consults the callee's body kind

At `emit_expr.c:2067`, gate the `&temp` transform on the same
`body_is_inline_c` predicate the formal side uses:

```c
bool callee_body_is_inline_c =
    fn_binding && fn_binding->fn_decl &&
    fn_binding->fn_decl->body &&
    fn_binding->fn_decl->body->kind == EX_INLINE_C;

if (!needs_fn_cast && !fn_binding->is_extern_c &&
    !matched_spec &&
    _callee_pbp &&
    !callee_body_is_inline_c &&        /* NEW */
    type_struct_pass_by_ptr(...) &&
    !expr_is_pbp_param(ctx, emit_arg)) {
    /* emit &temp */
}
```

- **Pros:** mirrors the existing exception on the formal side; the two
  emitters use the same predicate, so they cannot drift. Localized to one
  callsite branch.
- **Cons:** requires reaching `fn_decl->body` from a `Binding`. The
  binding currently exposes `closure_fn_binding`, `source_binding`, etc.,
  but not a direct `fn_decl` field everywhere a callee is resolved. May
  need a one-line accessor or a cached `is_inline_c` flag on the binding.

### Option B -- Cache an `emit_byvalue_struct_param` flag on the binding

When elaborating / emitting a defn, set
`fn_binding->emit_byvalue_struct_param = body_is_inline_c` (or a per-
parameter array, if mixed). At the call site:

```c
if (... && _callee_pbp && !fn_binding->emit_byvalue_struct_param && ...)
```

- **Pros:** O(1) check at the call site; no AST walk; analogous to the
  existing `emit_byvalue_carrier_abi` flag on `Binding` for the carrier
  ABI case (see `emit_expr.c:99-112`).
- **Cons:** one more flag to keep in sync. The flag must be set before
  any call site to the function emits, so emit-order discipline matters.

**Recommendation: Option A.** The bug is purely a synchronisation issue
between two emitters consulting the same property; expressing that
synchronisation directly (both call `body_is_inline_c(...)`) is the most
auditable fix. Option B is the right move only if profiling shows the AST
walk hurts.

## Phases

### Phase DS0 -- Reproduce as an error fixture

Land a positive fixture before changing the codegen, so the bug is
pinned. A minimal reproducer:

```
tests/fixtures/defstruct-inline-c-byvalue/
  input.tur
  expected.stdout
```

`input.tur`:

```turmeric
(defstruct Big
  [a : cstr b : cstr c : cstr d : cstr
   e : int  f : int  g : int  h : cstr])    ; >16 bytes

(defn mk [] : Big
  (make-struct Big "a" "b" "c" "d" 1 2 3 "h"))

(defn use-it [x : Big] : nil
  ```c
  fprintf(stdout, "%s %d\n", x.a, (int)x.e);
  fflush(stdout);
  ```)

(defn main [] : int
  (use-it (mk))
  0)
```

Until the fix lands, this fixture goes under
`tests/fixtures/errors/defstruct-inline-c-byvalue-mismatch/` with an
`expected.diag` matching the clang error. After the fix, it moves into
the happy suite with `expected.stdout = "a 1"`.

### Phase DS1 -- Suppress the call-site `&temp` for inline-C callees

Apply the Option A change at `emit_expr.c:2067`. Confirm:

- `bash tests/run.sh` -- zero `FAIL` lines, including the 1222 existing
  passes (cookie, header, mw-log, mw-compose, ...) and DS0 flipping to
  the happy column.
- Regenerate any `expected.c` codegen snapshots that contain the buggy
  pattern. Likely few, since today no stdlib defn pairs a `>16-byte`
  defstruct param with an inline-C body (the M2 plan deviation was
  driven by this very bug).

### Phase DS2 -- Restore the planned M2 CookieOpts surface (optional)

Once DS1 lands, the `CookieOpts` defstruct + `cookie` / `cookie-full`
constructors + `(httpd-set-cookie! conn opts)` API in
`docs/upcoming/httpd-middleware-async-plan.md` becomes implementable as
originally drafted. The current 9-arg positional API in `stdlib/httpd.tur`
can stay as the "primitive" or get replaced; the deviation note on the
`httpd-set-cookie!` docstring is the marker. Same trade-off as the
variadic-rest plan's V2: this is *enabled* by DS1, not forced by it.

DS2 also retroactively widens M4 (`CorsOpts`) and M7 (`Part`), so do it
before those PRs land if you want their original defstruct shape.

## Out of scope

- Reconsidering the 16-byte threshold or the by-ptr ABI itself. The plan
  is purely to bring the call site in line with the formal side -- both
  using the same `body_is_inline_c` exception. Whether the threshold is
  right is a separate ABI conversation.
- Adopting `T *` (pointer-typed) struct parameters in inline-C bodies as
  the canonical shape. That would let the call-site stay as-is, but
  forces every existing inline-C struct consumer to switch from `x.f` to
  `x->f` -- needless churn for the small structs that work today, and
  doesn't match the way C inline-C feels best.
- Closure parameters. `fd->closure` already gates the formal side; the
  call-site fix only matters for defns.

## Risk

- **Low.** The change only adds a guard that disables the existing
  `&temp` transform for one case (inline-C callee). Code paths that
  compile today are unaffected:
  - `>16-byte` struct + Turmeric body: callee already declared `const T *`,
    caller passes `&temp` -- unchanged.
  - `<=16-byte` struct: `_callee_pbp` is false, transform was already
    skipped -- unchanged.
  - Closure / extern-C / fn cast paths: already excluded by earlier
    guards -- unchanged.
- **Snapshot churn.** Limited. Today no stdlib fixture pairs a
  `>16-byte` defstruct param with an inline-C consumer (because such
  code does not compile). A new DS0 fixture is added by this plan; no
  pre-existing snapshot drift expected.

## Open questions

1. **Binding -> body access.** The cleanest Option A wording above
   assumes `fn_binding->fn_decl->body->kind == EX_INLINE_C` is reachable
   from the call-site emit context. Confirm the actual field name and
   nullability when implementing. The carrier-ABI path uses
   `fn_binding->emit_byvalue_carrier_abi` (a cached flag) for a similar
   distinction -- if `fn_decl` isn't directly reachable, falling back to
   Option B (cached flag) is the natural fix.
2. **Mixed-kind parameters.** A defn with multiple struct parameters,
   all `>16 bytes`, with an inline-C body, must have *every* such
   parameter use the by-value ABI on both sides. The current
   `body_is_inline_c` predicate is whole-function, so this is fine, but
   worth confirming with a fixture that has two big-struct params.
3. **`emit_carrier_bridge` interaction.** The carrier-ABI bridge code
   nearby (`emit_expr.c:2032-2040`) walks aggregates differently when
   the type uses carrier-ABI dispatch. Verify the fix does not
   inadvertently strip a needed bridge -- the existing
   `expr_emits_byvalue_carrier_abi` helper should keep that path
   independent, but worth eyeballing once the DS0 fixture is green.
