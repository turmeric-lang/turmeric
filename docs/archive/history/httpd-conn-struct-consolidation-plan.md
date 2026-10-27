# HttpdConn Struct Consolidation Plan

> **Status:** Implemented (2026-06-03)
> **Last Updated:** 2026-06-03
> **Type:** stdlib / inline-C maintainability + memory safety

---

## Implementation notes (2026-06-03)

- The canonical `HttpdConn` / `HttpdHeader` / `HttpdAttr` definitions are
  now hoisted exactly once via `__tur_include__` in `httpd/autolink-hint`
  (`stdlib/httpd.tur`); all 30 local redeclarations were removed.
- Regression guard: `tests/check-httpd-conn-single-def.sh` (ctest target
  `tur_httpd_conn_single_def`) fails if any local `} HttpdConn;` /
  `} HttpdHeader;` / `} HttpdAttr;` reappears outside the hoisted directive.
- Guard fixture: `tests/fixtures/httpd-async-mw-attr` drives
  `httpd-set-attr!` / `httpd-req-attr` through the async fiber path. Built
  against the pre-consolidation tree it fails with
  `stack-buffer-overflow on '_conn'` (the exact `attr_list` OOB); against
  the consolidated tree it passes.
- While building that fixture, a **separate, pre-existing** bug surfaced:
  inline-C fat-closure dispatchers (e.g. the verifier call in
  `httpd-mw-basic-auth-check`) crash when handed a closure that captures
  nothing. It is unrelated to the struct layout and is tracked in
  [noncapturing-closure-inline-c-dispatch-plan.md](noncapturing-closure-inline-c-dispatch-plan.md).
  The guard fixture therefore sets the attribute directly rather than
  through `mw-basic-auth`.

---

## Overview

`stdlib/httpd.tur` implements the entire HTTP server in inline-C blocks.
The central per-connection record, `HttpdConn`, is **redeclared in full
inside 30 separate inline-C blocks**. Because inline-C blocks each compile
to an independent C function body, every block that touches a connection
must restate the struct layout, and there is no single source of truth.

Over the server's phased development (M0..M7, A0..A4, H5..H8/MW2) fields
were appended to `HttpdConn` one phase at a time, but only *some* of the 30
copies were updated in each phase. As a result the tree currently contains
**7 distinct, divergent layouts** of the same struct. They happen to be
prefix-compatible (each new field is appended at the end), so reading an
*early* field through a stale copy is accidentally safe -- but reading a
*late* field through a copy that does not declare it reads past the
allocated object and crashes.

This plan consolidates the 30 copies into a single canonical definition.

---

## Background: the bug this prevents

Phase MW2 added `void *attr_list` (the request-attribute linked list) as
the last field of `HttpdConn`. Two code paths allocate a connection:

| Allocation site | Struct layout | Has `attr_list`? |
| --- | --- | --- |
| `httpd_handle` (pooled/sync path, ~`httpd.tur:188`) | full, 18 fields | yes |
| async worker (`~httpd.tur:2392`, struct at `2344`) | 17 fields | **no** |

`httpd-set-attr!` / `httpd-req-attr` (`httpd.tur:3588`, `3645`) read and
write `attr_list` at the *full-layout* offset. A request served by the
**async** path stores its `HttpdConn` on the stack using the shorter
17-field layout; calling `httpd-set-attr!` on such a connection writes the
attribute pointer *past the end of the stack object*, corrupting the
adjacent stack frame. This is a latent SEGV waiting for any async fixture
that uses request attributes (Basic Auth, rate-limit, session middleware).

> Note: a *separate*, already-fixed bug in the same area -- implicit
> `strdup` declaration truncating a 64-bit pointer to `int` under
> `-std=c99` -- was what made the **sync**-path attr fixtures
> (`httpd-mw-basic-auth`, `-basic-auth-attr`, `-rate-limit`) crash. That
> was fixed in the compiler by emitting `#define _DEFAULT_SOURCE 1` ahead
> of any hoisted `__tur_include__` (see `src/main.c`
> `hoist_tur_include_directives`). The struct drift documented here is the
> remaining, async-path hazard and is not yet exercised by a fixture.

---

## The 7 current layouts

All share the prefix
`int fd; int64_t handler; char method[16]; char path[1024]; char
version[16]; char *body; int body_len; int resp_status; char *resp_body;
int resp_body_len; int client_keep_alive;` and then diverge by which
trailing fields are present:

| # | Trailing fields beyond `client_keep_alive` | Copies | Example lines |
| --- | --- | --- | --- |
| 1 | (none) | 9 | 953, 967, 984, 1005 |
| 2 | `+route_params` | 2 | 3139, 3231 |
| 3 | `+route_params +tls +req_headers +resp_headers` | 6 | 1122, 1161, 1200, 3952 |
| 4 | `... +req_json_cache` | 3 | 1538, 1623, 1681 |
| 5 | `... +req_multipart_parts` | 2 | 2055, 2214 |
| 6 | `... +fiber_group` | 4 | 2344, 2946, 2971, 3003 |
| 7 | `... +attr_list` (canonical / full) | 3 | 3593, 3650, 3701 |

(Line numbers approximate; regenerate with
`grep -n "int fd; int64_t handler;" stdlib/httpd.tur`.)

Only layout #7 is correct. Every other copy is a truncation that is safe
only as long as nothing it touches reaches into a field it omits.

---

## Goals / Non-Goals

### Goals

- One canonical `HttpdConn` definition; all 30 inline-C blocks reference
  the same layout.
- No behavior change to the generated server; the consolidated layout is
  byte-identical to the current full (layout #7).
- Eliminate the async-path `attr_list` out-of-bounds hazard.
- A guard that fails the build if a future inline-C block reintroduces a
  divergent copy.

### Non-Goals

- Rewriting the httpd server in C source files (`src/runtime/httpd.c`).
  That is a larger architectural change; out of scope here.
- Changing the request-attribute API or its storage representation.

---

## Approach

Inline-C blocks cannot share a `typedef` across blocks directly, but the
codebase already has a mechanism for hoisting file-scope C declarations:
the `__tur_include__` directive (see `httpd/autolink-hint` in
`httpd.tur:68`, hoisted by `hoist_tur_include_directives` in
`src/main.c`).

### Step 1 -- Define `HttpdConn` once at file scope

Add the canonical full layout (plus the `HttpdAttr` / `HttpdHeader`
helper structs it depends on) to the `httpd/autolink-hint` block as
`__tur_include__` directives, so the typedef lands at file scope exactly
once:

```turmeric
(defn httpd/autolink-hint [] : int
  ```c /* __tur_autolink__: -lturi -pthread */
  /* __tur_include__: typedef struct __HttpdHeader { char *name; char *value; struct __HttpdHeader *next; } HttpdHeader; */
  /* __tur_include__: typedef struct __HttpdAttr { char *key; char *value; struct __HttpdAttr *next; } HttpdAttr; */
  /* __tur_include__: typedef struct { int fd; int64_t handler; char method[16]; char path[1024]; char version[16]; char *body; int body_len; int resp_status; char *resp_body; int resp_body_len; int client_keep_alive; void *route_params; int64_t tls; void *req_headers; void *resp_headers; int64_t req_json_cache; void *req_multipart_parts; void *fiber_group; void *attr_list; } HttpdConn; */
  return 0;
  ```)
```

### Step 2 -- Delete the 30 local redeclarations

Remove each `typedef struct { ... } HttpdConn;` (and the duplicated
`HttpdHeader` / `HttpdAttr`) from the individual inline-C blocks, leaving
the field *uses*. Each block now resolves `HttpdConn` against the single
hoisted typedef.

Care points:
- A handful of blocks declare `HttpdConn` *plus* extra local typedefs
  (e.g. `HttpdAsyncReqCtx`, `HttpdAsync`, `httpd_tls_io_fn`). Keep those
  locals; only the shared structs move to file scope.
- Watch for `-Wunused-local-typedefs`: those warnings disappear once the
  duplicates are gone.

### Step 3 -- Guard against regressions

Add a check (test or `tools/` lint) that greps `httpd.tur` for
`} HttpdConn;` and fails if more than zero local copies exist, so the
single-definition invariant is enforced going forward.

### Step 4 -- Verify

- `bash tests/run.sh` -- zero `FAIL` (all httpd-* fixtures still green).
- Add a new fixture `httpd-async-mw-attr` that drives `httpd-set-attr!`
  through the **async** server path and reads it back, locking in the
  fix for the out-of-bounds case that no current fixture covers.

---

## Risks

- The hoisted typedef must be byte-for-byte the union of every field used
  anywhere in the file. A missing field becomes a compile error (good --
  loud), but a *wrong order* would silently corrupt offsets. Diff the new
  layout against layout #7 before deleting any copy.
- `__tur_include__` hoists to the very top of the generated file. The
  recently-added `#define _DEFAULT_SOURCE 1` (now emitted ahead of hoisted
  includes) must remain first; the typedef directives contain no system
  `#include`, so ordering among them is irrelevant.

---

## Estimated scope

- ~30 inline-C edits in one file (`stdlib/httpd.tur`), mechanical.
- 1 new file-scope directive block.
- 1 regression guard + 1 new async-attr fixture.
- No compiler changes required (the `_DEFAULT_SOURCE` ordering fix is
  already in `main`/this branch).
