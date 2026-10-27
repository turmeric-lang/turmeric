# Non-Capturing Closures vs Inline-C Fat-Closure Dispatch Plan

> **Status:** Implemented (2026-06-03)
> **Last Updated:** 2026-06-03
> **Type:** codegen / runtime ABI -- closure calling convention

---

## Outcome

Implemented via the existing `^fat` parameter marker rather than a wholesale
codegen change (Option A) or per-site runtime tagging (Option B).

A blanket "always box `:int`-erased closures" (Option A) was rejected after
audit: a large set of stdlib higher-order functions (`option.tur`,
`result.tur`, `list.tur`, `vec.tur`, `rc.tur`, `arrow.tur`, the `cmp_fn`
equality paths, ...) deliberately receive a closure as an `:int` handle and
call it as a **bare** function pointer (`((fn_t)(intptr_t)f)(x)`), *not* via
`fat[0]`. Always boxing would break every one of those. The two conventions
(bare-ptr vs fat) genuinely coexist; `^fat` is the per-parameter discriminator
that already distinguishes them.

Changes:

- **Marked the closure-entry boundary params `^fat`** in `stdlib/httpd.tur`:
  `mw-basic-auth` (`verifier`, `next`); `httpd-new` / `httpd-new-pool` /
  `httpd-new-async` / `httpd-new-async-with-limit` / `httpd-new-tls`
  (`handler`); `router-add` (`handler`); `compose-middleware-of` (`base`);
  and every `mw-*` factory (`next`). At each of these the bare non-capturing
  fn first appears as a `TY_FN`, so the call site auto-shims it into a fat
  `{ thunk, env }` box (`EX_FN_TO_FAT`). Internal `fat[0]` dispatchers
  (`httpd-call`, `httpd-mw-fold`, the handler/verifier inline-C) are left
  untouched -- by the time a handle reaches them it is already fat.
- **Extended the `^fat` call-site logic** (`src/compiler/elab_call.c`) to pass
  through a *computed* `:int` argument (a non-literal -- an already-erased fat
  handle, e.g. the result of `compose-middleware`) unchanged, instead of
  rejecting it. This is what lets a `^fat` boundary sit on the same plumbing
  that threads composed handlers as `:int`. A non-zero `:int` *literal* still
  errors (the diagnostic half), and a bare `TY_FN` is still shimmed.
- **Retired the "must capture a free variable" folklore** in the httpd module
  docstrings / comments (`mw-basic-auth`, `httpd-call`, `compose-middleware`,
  the module header) and dropped the incidental `_dummy 0` capture from the
  `mw-basic-auth` example.
- **Added fixture** `tests/fixtures/httpd-mw-basic-auth-noncapture` -- a
  verifier that captures nothing; previously SEGV (async) / hang (pool), now
  prints the authorized body. The capturing `httpd-mw-basic-auth-attr` fixture
  is unchanged and still passes. Full suite: 1299 passed, 0 failed.

---

## Overview

Several inline-C blocks in the stdlib invoke a user closure that was
handed to them as an `:int` handle by reading it as a **fat closure** --
an `int64_t[]` whose slot 0 is the code pointer and whose own address is
passed back as the environment:

```c
int64_t *fat = (int64_t *)(intptr_t)verifier;
typedef int64_t (*fn2_t)(void *, const char *, const char *);
int64_t ok = ((fn2_t)(intptr_t)fat[0])((void *)fat, user, pass);
```

This assumption holds **only when the closure captures at least one free
variable.** A closure that captures nothing is lowered by codegen to a
*bare function pointer* (the raw address of the generated `__fn_*`), not a
heap-allocated `[code, env, ...]` record. Dereferencing such a value as
`fat[0]` reads the first 8 bytes of the function's machine code as a
pointer and calls through it -- undefined behavior that manifests as a
SEGV (or, depending on what the misread bytes happen to encode, a hang or
silent misbehavior).

This was discovered while building `tests/fixtures/httpd-async-mw-attr`
for the HttpdConn consolidation (see
[httpd-conn-struct-consolidation-plan.md](httpd-conn-struct-consolidation-plan.md)).
It is **independent of that struct work** and independent of the
sync/async server distinction.

---

## Reproduction

A `mw-basic-auth` verifier that captures nothing:

```turmeric
verify (fn [u : cstr p : cstr] : int
         (if (= 1 (cstr-eq-const-time u "admin"))
           (cstr-eq-const-time p "s3cret")
           0))
composed (mw-basic-auth "app" verify base)
```

- **Async server** (`httpd-new-async`): SEGV.
- **Pool server** (`httpd-new-pool`): hangs (the misread "ok" value makes
  auth neither cleanly pass nor challenge).

Instrumenting the verifier handle at creation prints a **code-segment
address** (e.g. `0x...9ec`, 4-byte aligned, inside `.text`), and ASan/UBSan
reports:

```
runtime error: load of misaligned address 0x...9ec for type 'int64_t',
  which requires 8 byte alignment
... SEGV in httpd_mw_basic_auth_check (the `fat[0]` load)
```

### Why the existing fixture does not hit this

`tests/fixtures/httpd-mw-basic-auth-attr` (the sync attr fixture) uses a
verifier that *captures* an outer binding:

```turmeric
_tag   "secret"
verify (fn [u : cstr p : cstr] : int
         (let [_t _tag]                      ; <-- forces a real fat closure
           (if (= 1 (cstr-eq-const-time u "admin"))
             (cstr-eq-const-time p "s3cret")
             0)))
```

The `_t _tag` reference is load-bearing: it forces the closure to capture
an environment, so it is represented as a fat closure and the `fat[0]`
dispatch works. Remove it and the fixture crashes. This implicit
requirement is undocumented and fragile.

---

## Scope: which dispatchers are affected

Every inline-C site that casts a user-supplied closure handle to
`int64_t *fat` and calls `fat[0]` assumes the fat layout. Audit (line
numbers approximate, regenerate with
`grep -n "fat\[0\]" stdlib/httpd.tur`):

| Site | Closure source | Risk |
| --- | --- | --- |
| `httpd-mw-basic-auth-check` (verifier call) | user verifier passed to `mw-basic-auth` | confirmed crash |
| sync handler dispatch (`httpd-handle`) | `conn->handler` (user handler) | a non-capturing top-level handler would crash |
| async handler dispatch (`httpd-async-fiber-body`) | `conn->handler` | same |
| `httpd-call` / middleware `next` dispatch | composed middleware closures | usually capture, but not guaranteed |
| `httpd-mw-fold` (`fn1_t`) | folded middleware list | same |

Other stdlib modules that take a closure as `:int` and dispatch via
inline-C (e.g. reactor callbacks, `threadpool`, `future`) should be swept
for the same pattern.

---

## Goals / Non-Goals

### Goals

- A closure passed to an inline-C dispatcher works **whether or not it
  captures** free variables.
- No silent "capture something to be safe" folklore; the representation is
  uniform or the dispatch tolerates both forms.
- A fixture that locks in the non-capturing case for at least the
  httpd verifier path.

### Non-Goals

- Reworking the closure representation wholesale for the typed/native call
  paths that already agree on a convention.

---

## Approach (options)

Pick one; (A) is the most robust, (B) the most local.

### Option A -- Uniform fat representation for `:int`-erased closures

Make codegen **always** box a closure as a fat record when it is erased to
an `:int` handle (the form these inline-C dispatchers receive), even when
the capture set is empty (slot 0 = code pointer, no env slots). Bare
function pointers remain an internal optimization only for *statically
typed, directly-called* function values, never for the erased handle. This
removes the footgun at the source; every existing `fat[0]` dispatcher just
works.

- Cost: a tiny heap box for non-capturing closures that escape as `:int`.
- Risk: must ensure no existing path relies on the bare-pointer form of an
  erased handle.

### Option B -- Tagged dispatch in the inline-C helpers

Teach the dispatchers to detect representation and branch. A fat closure's
slot 0 is a code pointer into `.text`; a bare function pointer *is* the
code pointer. If the runtime tags erased closures (e.g. low-bit tag, or a
distinguishable header word), `httpd-mw-basic-auth-check` et al. can call
the right way. More invasive per-site and easy to get wrong; only sensible
if a tag already exists.

### Option C -- Document + assert (stop-gap)

If neither A nor B is scheduled soon, at minimum:

- Make `mw-basic-auth` (and peers) reject a bare-function-pointer handle
  with a clear runtime error instead of crashing -- e.g. range-check the
  handle against the loaded `.text` segment, or require callers to pass a
  fat closure and assert it.
- Document the "verifier must be a fat closure" requirement on
  `mw-basic-auth` and stop relying on an incidental `_t _tag` capture in
  the fixture.

This is damage control, not a fix.

---

## Verify

- New fixture `httpd-mw-basic-auth-noncapture` (sync and/or async) with a
  verifier that captures nothing; must print the authorized body, not
  crash or hang.
- Re-run `tests/fixtures/httpd-mw-basic-auth-attr` (capturing verifier)
  unchanged -- no regression.
- `bash tests/run.sh` -- zero `FAIL`.

---

## Estimated scope

- Option A: codegen change in the closure-erasure path + a fixture. Medium.
- Option B: runtime tagging + per-site edits. Large.
- Option C: stdlib doc + a guard + fixture. Small (but leaves the trap).
