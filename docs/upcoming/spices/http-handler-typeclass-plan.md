# Plan: `http` / `httpd` Handler Typeclass + JSON Codec Integration (U2, v1)

> Status: complete — P1+P2 landed (turmeric-spices PR #23); P3 (httpd JSON
> body codecs); P4 (http client codecs); P5 (tests/docs + negative fixtures).
> The two type-checking gaps P5 first hit (instance-method return not unified;
> `^Class` `defn` constraint not discharged at the call site) were fixed in
> turmeric main and are now enforced -- all four negative fixtures below fail
> to compile as intended.
> Tracks: spices-type-features-uplift-plan **U2 target — http/httpd**
> Scope: `spices/httpd/` and `spices/http/`; no compiler dependency expected
> Builds on: json `Encode`/`Decode` typeclasses (turmeric-spices PRs #20 +
> the `Encode [float]` round-trip fix); httpd `Request`/`Response` opaques
> (already `defopaque` in `httpd/types.tur`, U1 done)

## Motivation

The httpd server's user handler is a bare C-ABI function pointer typed as
`int -> int`:

```turmeric
;; httpd/server.tur
(defn server-start [port : int handler : (c-fn [int] int)] : ptr<void> ...)
```

The `int` in / `int` out are really a `Request` handle and a `Response`
handle, but the type system is told nothing. The server even has to
re-assert the type by hand inside the worker:

```turmeric
;; httpd/server.tur:297 — srv-worker-loop
(let [resp (:: (srv-call-handler handler req) :Response)
      wbuf (serialize-response resp)] ...)
```

So two things are wrong at the type layer:

| Today | Problem | Target |
|---|---|---|
| `handler : (c-fn [int] int)` | handle ints, not `Request`/`Response` | `Handler` instance with `handle : Request -> Response` |
| `(:: (srv-call-handler …) :Response)` | runtime cast re-derives a known type | dispatch returns `Response` directly |
| handler body hand-parses `req-body` / hand-builds JSON | every JSON endpoint re-writes decode+encode glue | `Handler` body uses json `Decode`/`Encode` |
| `http` client: `response-json` returns an untyped doc handle | caller walks the DOM by string keys | `(decode-body resp T) : (Result T cstr)` via json `Decode` |

`Request` and `Response` are already `defopaque` (`httpd/types.tur:19,27`),
so the U1 handle-safety groundwork is done. This plan is the U2 layer on
top: a `Handler` typeclass so the *handler contract* is typed, plus
first-class json codec helpers so a typed endpoint is a few lines instead
of inline-C body parsing.

```turmeric
;; target shape — a typed JSON echo endpoint
(defstruct EchoReq  [msg : cstr])
(defstruct EchoResp [echo : cstr  len : int])
(derive-json EchoReq  (msg cstr))
(derive-json EchoResp (echo cstr) (len int))

(defstruct Echo [])               ;; zero-field handler witness
(definstance Handler [Echo]
  (handle [self req]
    (with-json-body req EchoReq
      (fn [r] (json-ok (make-struct EchoResp (.msg r) (str-len (.msg r))))))))

(defn main [] : int
  (let [srv (serve 8080 (make-struct Echo))] ... (server-stop srv) 0))
```

---

## Non-goals

- Changing the threading model, socket code, or pool (`httpd/server.tur`,
  `httpd/pool.tur`). The dispatch fan-out into worker threads is unchanged;
  only the handler's *type* and the body codec helpers change.
- Capturing closures as handlers. The C function-pointer rep has no
  environment slot (documented at `httpd/server.tur:42`), and the typeclass
  approach preserves that: an instance witness is zero-sized, so the
  generated trampoline stays captureless. Stateful handlers remain
  out of scope (they would need a registry; note as future work).
- A routing/middleware framework. `handle` dispatches on the whole
  `Request`; path routing stays the user's job (they branch on
  `req-path`). A `Router` typeclass could layer on later.
- Replacing the wire format. JSON helpers are additive; text/plain
  handlers keep working via `resp-ok`.

---

## Strategy

The one real bridge problem: `server-start` needs a `(c-fn [int] int)`,
but a typeclass method `handle` is `(self, Request) -> Response`. The plan
generates a **captureless per-instance trampoline** that adapts the method
to the C-ABI signature, and exposes it through a `serve` macro so users
never write the cast. Everything else (the json helpers) is ordinary
library code on top of the already-shipped json typeclasses.

Ship in three independent slices:
1. httpd `Handler` typeclass + `serve` bridge (P1–P2).
2. httpd json codec helpers for handler bodies (P3).
3. http *client* typed body decode/encode (P4) — independent of 1–2,
   can land in parallel.

---

## Phases

### P1 — `Handler` typeclass (`httpd/handler.tur`, net-new module)

**Tasks**
- New module `spices/httpd/src/httpd/handler.tur`. Import `Request` /
  `Response` from `httpd/types`.
- Define the class (instances will live in user code; the class is exported
  so user `definstance`s are not orphans — same pattern as ansi `Color`):

  ```turmeric
  (defclass Handler [a]
    (handle [self req : Request] : Response))
  ```
- Document the captureless-witness convention: the instance type is
  typically a zero-field `defstruct` used purely to select the instance;
  per-request state lives in the `Request`, not the witness.

**Acceptance**
- A zero-field `(defstruct Hello [])` with `(definstance Handler [Hello] …)`
  type-checks and `(handle (make-struct Hello) req)` returns a `Response`.

### P2 — `serve` bridge: instance → `(c-fn [int] int)`

The server entry points (`server-start`, `server-start-pool`,
`server-start-spawn`) all take `(c-fn [int] int)`. P2 adapts a `Handler`
instance to that signature without changing the server internals.

**Tasks**
- Add a `serve` macro (in `httpd/handler.tur`) that, given a port and a
  `Handler` instance value, emits a captureless top-level trampoline and
  passes it to `server-start`:

  ```turmeric
  ;; (serve 8080 (make-struct Hello)) expands to roughly:
  (defn __httpd-trampoline-Hello [req : int] : int
    (:: (handle (make-struct Hello) (:: req Request)) :int))
  (server-start 8080 __httpd-trampoline-Hello)
  ```
  The witness is reconstructed inside the trampoline (zero-field, so
  free), keeping the function pointer captureless and matching the existing
  `(c-fn [int] int)` contract exactly.
- Provide `serve-pool` / `serve-spawn` variants mirroring the three
  `server-start*` entry points.
- Once the typed path exists, drop the `(:: … :Response)` re-cast in
  `srv-worker-loop` is *not* required (the trampoline already returns the
  bare int the worker expects) — leave `srv-worker-loop` untouched so the
  socket code is unchanged.

**Acceptance**
- An end-to-end test starts a server with `(serve port (make-struct Hello))`,
  issues a request (via the `http` client or a raw socket in the test), and
  gets the handler's `Response` body back.
- The generated trampoline compiles as a `(c-fn [int] int)` (captureless
  check passes).

### P3 — JSON codec helpers for handler bodies (`httpd/handler.tur`) — LANDED

This is the "codecs use json's Encode/Decode" half. Handlers should decode
a typed request body and encode a typed response without inline-C.

**Landed:** `req-decode` / `with-json-body` (macros, type-directed decode),
`json-ok` / `json-resp` (generic over `Encode`). Test:
`tests/json_codec_test.tur` round-trips a typed JSON POST body to a
typed JSON response, asserts `Content-Type: application/json`, and rejects a
malformed body with `400`.

Two findings worth recording:

- **Transitive native deps are not propagated across workspace siblings.**
  httpd importing json (whose `Encode`/`Decode` instances emit
  `#include <yyjson.h>`) does not inherit json's `yyjson` cmake-dep. Each
  consumer that ultimately links a native lib re-declares it — httpd now
  carries `yyjson` in its own `build.tur` `:cmake-deps`, exactly as
  `ecs-raylib` re-declares `raylib`. This is the repo convention, not a
  compiler gap.
- **`http/response`'s `response-json` had a latent include-scope bug.** It
  did `#include <yyjson.h>` *inside* the function body under
  `__has_include`; this was dormant only because no httpd test had yyjson on
  its include path. Once httpd linked yyjson, every httpd test importing
  `http/response` failed to compile (`static inline` functions are illegal
  inside a function body). Fixed by hoisting the guarded include to a
  file-scope C block; the stub-when-absent behavior is unchanged.

**Tasks**
- `(req-decode req T) : (Result T cstr)` — parse `(req-body req)` with
  `json-parse-doc`, take the root, dispatch the json `Decode [T]` instance
  via the return-type ascription, free the doc, return the `Result`.
  (Mirror the `decode` ascription idiom in `json/tests/round-trip.tur`.)
- `(json-ok x) : Response` — `(encode x)` via json `Encode [T]`, then
  `(resp-ok "application/json" <fragment>)`. Frees the encoded buffer per
  json's caller-owns contract.
- `(json-resp status x) : Response` — like `json-ok` for non-200 codes,
  built on `response`.
- `(with-json-body req T f)` convenience: `req-decode` then either call
  `f` on the decoded value or return `bad-request` with the decode error
  message. Keeps the happy path one expression.
- These depend only on the already-exported json surface
  (`derive-json`, `Encode`, `Decode`, primitives incl. `Encode [float]`);
  add `json` as a workspace sibling dep of `httpd` if not already declared
  (`build.tur` `:spices`).

**Acceptance**
- A handler using `(with-json-body req EchoReq (fn [r] (json-ok …)))`
  round-trips a JSON POST body to a JSON response.
- A malformed body yields `bad-request` with the json decode error, not a
  crash.
- `Content-Type: application/json` is set on `json-ok` responses.

### P4 — Typed client body decode/encode (`http/response.tur`, `http/request.tur`) — LANDED

Independent of the server work: the `http` client already has an
untyped `response-json` (`http/response.tur:106`) that hands back a raw
yyjson doc. Add a typed layer.

**Landed:** `response-decode` (macro, in `http/response.tur`) and
`json-request` (generic over `Encode`, in `http/request.tur`, with the
`__hdr-cons` header-prepend helper). Test:
`tests/http/codec_test.tur` verifies `json-request` encodes the struct body +
sets `Content-Type: application/json`, and `response-decode` decodes a typed
struct from a synthesized response body and rejects a non-JSON body — all
without a network/TLS (only `http/request` + `http/response` are imported, so
the mbedtls client path is never linked).

Two notes worth recording:

- **`http` now declares `yyjson`.** Same transitive-native-dep convention as
  P3: `http` re-declares `yyjson` in its `build.tur` `:cmake-deps`. yyjson was
  previously *optional* for `http` (only `response-json` used it, via a
  `__has_include` stub); P4's `response-decode`/`json-request` import
  `json/decode`/`json/encode` unconditionally, so yyjson becomes a first-class
  `http` dependency. Every consumer of `http/response` (e.g. httpd) therefore
  links yyjson transitively — httpd already declares it (P3), so the in-tree
  suite is unaffected.
- **Method/file placement matches the plan.** The codecs live in
  `http/request.tur` / `http/response.tur` as specified, rather than a
  separate codec module, accepting the above coupling as the deliberate cost
  of first-class typed client codecs.

**Tasks**
- `(response-decode resp T) : (Result T cstr)` in `http/response.tur` —
  parse `(response-body resp)` and dispatch json `Decode [T]`. Supersedes
  hand-walking the doc returned by `response-json` (keep `response-json`
  for dynamic/unknown shapes).
- `(json-request method url x headers) : int` in `http/request.tur` —
  `(encode x)` via json `Encode [T]`, build the request with the fragment
  as body and `Content-Type: application/json`. Thin wrapper over the
  existing `request` constructor.
- Add `json` as a workspace sibling dep of `http` if needed.

**Acceptance**
- `(response-decode resp User)` parses a JSON response body into a `User`
  struct (`derive-json User …`) and returns `(ok user)`.
- `(json-request "POST" url (make-struct User …) (nil-value))` produces a
  request whose body is the encoded struct and whose `Content-Type` is
  `application/json`.

### P5 — Tests, negative fixtures, docs — LANDED

**Positive tests + docs:**
- `spices/httpd/tests/json_codec_test.tur` (serve + `with-json-body`
  round-trip, landed P3) and `spices/http/tests/codec_test.tur`
  (`json-request` + `response-decode`, landed P4).
- READMEs: both `spices/httpd/README.md` and `spices/http/README.md` show the
  typed codec path; the raw `(c-fn [int] int)` `server-start` stays documented
  as the lower-level API.

**Negative fixtures (all four fail to compile as intended).** Each carries a
header with its `tur check` command and expected diagnostic, and lives under
`tests/errors/` so the non-recursive `tur test tests` run skips it.

| Fixture | Guarantee | Diagnostic |
|---|---|---|
| `httpd/tests/errors/handler-non-response-return.tur` | a `Handler` `respond` body must return a `Response` | `TUR-E0001`: instance method 'respond' declares return type 'Response' but its body returns Other |
| `httpd/tests/errors/req-decode-missing-decode-instance.tur` | `req-decode` into a type with no `Decode` instance | `no instance 'Decode NoCodec'` |
| `http/tests/errors/response-decode-missing-decode-instance.tur` | `response-decode` into a type with no `Decode` instance | `no instance 'Decode NoCodec'` |
| `http/tests/errors/json-request-missing-encode-instance.tur` | `json-request` body with no `Encode` instance | `TUR-E0001`: no 'Encode' instance for 'NoCodec' in constrained call to 'json-request' |

The first and last of these were initially blocked by two turmeric
type-checking gaps that P5 surfaced and that have since been **fixed in
turmeric main** (the change that also added the `return-type-nominal-*`
compiler fixtures; reported under
`docs/reported/instance-method-return-not-unified.md` and
`docs/reported/...-defn-constraint-not-discharged-at-call-site.md`):

1. **Instance method body result type now unifies with the class signature.**
   `(definstance Handler [Bad] (respond [self req] (make-struct Other 1)))` is
   now a `TUR-E0001`. (A bare `int` return still type-checks -- `Response` is a
   `defopaque :int`, so the int<->carrier coercion is intentional; the fixture
   returns a struct to exercise a genuine nominal conflict.)
2. **A `^Class` `defn` constraint is now discharged at the call site.**
   `(json-request … (make-struct NoCodec 1) …)` with no `Encode [NoCodec]` is
   now a `TUR-E0001`, the same way the `decode`-via-ascription path already
   rejected a missing `Decode`.

**CI-harness note:** both spices now use a flat `tests/*.tur` layout, so CI
runs the non-recursive `tur test tests` (all suites at the top level) and
skips the `tests/errors/` subdir. httpd was flattened from `tests/httpd/` for
exactly this reason -- under the descend-and-run path a nested layout uses,
CI would try to *run* (and fail on) an unconditionally-failing fixture, unlike
the `-Xsubstructural`-gated U1 fixtures which pass under plain `tur test`.

**Acceptance**
- `tur test spices/httpd/tests` and `tur test spices/http/tests` green.
- READMEs show the typed handler path end to end.
- All four negative fixtures fail to compile with the diagnostics above.

### Dependency graph

```
json Encode/Decode (already shipped) ──┬─► P3 (httpd json helpers)
                                       └─► P4 (http client codecs)  [parallel]

P1 (Handler class) ─► P2 (serve bridge) ─► P3 ─► P5 (tests/docs)
                                            P4 ──┘
```

P1→P2→P3 is the server spine. P4 is parallel and depends only on json.
P5 closes both out.

---

## Risks and open questions

1. **Trampoline generation in a macro.** P2's `serve` macro must emit a
   uniquely-named top-level `defn` per handler type and reference the
   instance's `handle`. The json `derive-json-sum-decode` macro already
   emits a standalone helper `defn` with a `str-append`-derived name
   (`json/encode.tur:591`) and delegates to it — reuse that exact idiom for
   the trampoline name. If a single port is served by two handler types in
   one module, ensure name uniqueness keys on the type name.

2. **Witness reconstruction cost.** The trampoline does
   `(make-struct Hello)` per request. For a zero-field struct this is free
   (no allocation if the compiler treats it as a unit); confirm it does not
   heap-allocate per call. If it does, hoist a module-level witness `def`
   and reference it instead.

3. **`Result` ABI at the typeclass boundary.** P3/P4 dispatch `Decode`
   through `(:: (decode …) (Result T cstr))`. The json suite already
   exercises this for structs, sums, and opaques, so the carrier-box path
   is battle-tested — but re-confirm for a `Decode [T]` whose `T` is a
   user struct *imported from another spice* (cross-module instance
   resolution), which the json tests do not currently cover.

4. **Stateful handlers.** The captureless constraint means a handler can't
   close over, e.g., a database pool. Out of scope here, but the `Handler`
   class shape (method takes `self`) leaves room for a future
   registry-backed variant that threads state through the witness; note it
   so the class signature isn't designed in a way that precludes it.

5. **`Content-Length` / body framing.** `json-ok` sets the body; confirm
   `serialize-response` (`httpd/write.tur`) computes `Content-Length` from
   the body length so a JSON body with embedded NULs (shouldn't happen, but
   guard) or multibyte UTF-8 is framed correctly.

---

## Acceptance (whole plan)

- httpd handlers can be written as `(definstance Handler [T] (handle …))`
  and registered with `(serve port (make-struct T))`; the raw
  `(c-fn [int] int)` `server-start` remains as the lower-level API.
- A typed JSON endpoint is expressible without inline-C body parsing,
  using `req-decode` / `with-json-body` / `json-ok` built on json's
  `Encode`/`Decode`.
- The `http` client can encode a typed request body and decode a typed
  response body through `json-request` / `response-decode`.
- Negative fixtures show a non-`Response` handler return and a missing
  `Decode` instance are compile-time errors.
