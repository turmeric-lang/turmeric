# Dynamic Var Adoption -- Spices Plan

> **Status:** Proposed, nothing started. **Revised 2026-10-17** against
> turmeric v0.65.5 and turmeric-spices `main` 7465a08. The 2026-06-24 revision
> had six targets; four are dropped here, because they rested on facts that
> were wrong when written or have changed since (see "What changed"). What is
> left is tourist:
> - **SPA-1** (the per-thread Conn slot) needs nothing from the compiler and
>   can start now.
> - **SPA-2** (the request context) can start now on the pool and spawn
>   servers tourist runs on, with two decisions open.
>
> **Prerequisites:** none. `defdynamic`, `binding` and `spawn-conveying` are
> always on: `-Xdynamic-vars` has been an accept-and-warn no-op since v0.24.0
> (`src/runtime/globals.c:194`). `(def ^thread-local ...)` (mutable-globals G4b,
> 2026-08-05) is the per-thread form the old text waited on as `defthreadlocal`.
> See [dynamic-vars-guide](../../guides/dynamic-vars-guide.md) and
> [mutable-globals-guide](../../guides/mutable-globals-guide.md).
>
> **Sibling plan:** [dynvar-adoption-plan](../dynvar-adoption-plan.md) covers
> the compiler tree. Nothing here waits on it any more. Its ADO-0b
> (`defthreadlocal`) is covered by `^thread-local`, and ADO-0 (the C-side
> dynvar API) mattered only to the ansi target dropped below.
>
> **Found while revising, filed as reports:**
> - [dynvar-binding-not-swapped-on-fiber-switch](../../archive/dynvar-binding-not-swapped-on-fiber-switch.md) (resolved 2026-10-18):
>   a `binding` is per thread, not per fiber.
> - [defdynamic-generic-adt-root-emitted-before-its-type](../../archive/defdynamic-generic-adt-root-emitted-before-its-type.md)
>   (resolved 2026-10-18): a `(Option T)` dynvar did not compile. It does now,
>   so tourist's "no request in flight" root can be an `(Option Ctx)`.
> - [unknown-type-annotation-becomes-implicit-tyvar](../../archive/unknown-type-annotation-becomes-implicit-tyvar.md):
>   a 2026-10-17 note; `option<T>` is not a type in an annotation, so write
>   `(Option T)`.
>
> **Scope:** replace hand-rolled "current X" state and threaded context
> parameters in spices with `defdynamic` (when scope restore matters) or
> `^thread-local` (when per-thread isolation is the whole point). No API
> redesign beyond what these forms naturally remove.

---

## What changed since the 2026-06-24 revision

Every row was measured on 2026-10-17. The probes ran on the v0.65.5 release
binary.

| The old text said | The tree says |
| --- | --- |
| Prerequisite: DV0-DV4 behind `-Xdynamic-vars`. | Always on. The flag is a no-op. |
| Wait for the sibling plan's `defthreadlocal` (ADO-0b). | `(def ^mut ^thread-local s : (Option Ctx) (None))` works today, one copy per thread. It lowers to a `pthread_key_t`, because plain `__thread` is shared by every thread under `tur jit` (`src/compiler/expr.h:274-286`). |
| Open question: with an explicit `defn main`, the root initializer is not emitted. | Fixed. The initializer goes through `def_init_body` (`src/compiler/emit_module.c:20714-20727`). Every probe below uses an explicit `main` and reads the root correctly. |
| A: httpd is the producer; tourist exports `*current-ctx*` for httpd to bind. | tourist builds and frees the `Ctx` itself, in `__tourist-handler` (`spices/tourist/src/tourist/app.tur:395-400`), so httpd is not involved. spices/httpd has also become a typed layer over stdlib httpd (turmeric-spices #98). The cross-spice declaration question goes away. |
| A: about 29 functions take `[ctx : Ctx]`. | 80 `ctx : Ctx` annotations across tourist, tourist-session and tourist-ws (the sweep below). |
| A: `(defdynamic *current-ctx* :ptr<Ctx> nil-ctx)`. | `Ctx` is `(defopaque Ctx :int)`, and a `:Ctx` dynvar works, including `spawn-conveying`. A `(Option Ctx)` dynvar did not compile until 2026-10-18 (report above); it does now, and conveys. |
| B: raygui "already manipulates C statics with hand-rolled save/restore". | `gui-set-state` / `gui-set-style` / `gui-load-style` are plain setters over raygui's own C globals (`spices/raygui/src/raygui/core.tur:110-228`). Nothing saves or restores, and a dynvar cannot change what raygui's C reads. |
| C: ansi has a stdout sink and a color-mode flag to convert. | ansi `printf`s to stdout (`spices/ansi/src/ansi/color.tur:27-95`), and there is no color-mode state: `term-no-color?` reads `$NO_COLOR` on every call (`spices/ansi/src/ansi/term.tur:43`). |
| C: color mode should be thread-local, because "workers read the same default". | That is backwards. A worker thread's copy starts from the default, so a `--no-color` set on the main thread would be ignored on every other thread. A process-wide setting is a plain `^mut` global. |
| D: `*valkey-client* :ptr<ValkeyClient>`. | `Client` is `(defopaque Client :int :linear)` (`spices/valkey/src/valkey/client.tur:21`), and `defdynamic` refuses linear types (`TUR-E0603`). |
| E: postgres `Conn` is `^borrow`-typed. | It is `(defopaque Conn :int :linear)` (`spices/postgres/src/postgres/db.tur:29`). Same refusal. |
| F: `(defthreadlocal *gl-program* :int 0)`. | `Program`, `Vao` and `Vbo` are `:linear` defopaques (`spices/opengl/src/opengl/types.tur:37-51`), and an `:int` cache of one is the stand-in CLAUDE.md forbids. |
| (not known) | tourist keeps the current request's Conn in a hand-rolled `static __thread int64_t` (`app.tur:533-545`). That is the pattern `^thread-local` replaces: it is shared under `tur jit`, and it types the Conn as `:int`. This is SPA-1. |
| (not known) | A `binding` does not move with its fiber. That is safe on the pool and spawn servers, where one worker thread owns a request end to end. It is wrong on a fiber-per-request server. This is decision 2 under SPA-2. |

---

## Dynvar vs. thread-local -- the rubric

- **Dynvar** when callers want scoped override with automatic restore, nested
  overrides should compose, and a child started with `spawn-conveying` should
  see the parent's binding.
- **Thread-local** when per-thread isolation is the point, no caller wants a
  nested override, and a new thread should start from the initializer.
- **Plain global** (`^mut`, `^atomic` if read across threads) when the value is
  process-wide, such as a setting read once from the CLI.
- **Leave alone** when the explicit parameter is load-bearing. That covers a
  `:linear` handle, whose linearity is the safety property, and an explicit
  target such as a canvas or a writer, where passing it is the API.

Two type rules from the probes:
- Spell an optional type `(Option T)`, never `option<T>` (that becomes an
  implicit type variable).
- A dynvar cannot hold an `(Option T)` until its report is resolved.

---

## Targets

### SPA-1. tourist's current-request Conn -> `^thread-local` -- small, do first

**Sites:** `spices/tourist/src/tourist/app.tur:533-559`, which are
`__tourist-conn-cell`, `tourist-request-conn` and `__tourist-handler-conn`.

**Today:**
- A `static __thread int64_t cur` lives behind a mode-switch `:int` function.
- `tourist-request-conn` casts that slot to a `Conn`, so outside a request it
  returns a null handle.
- The slot is never cleared, so it holds the previous request's Conn until the
  next request overwrites it.

**Change:**

```turmeric
(def ^mut ^thread-local current-conn : (Option Conn) (None))
```

- `__tourist-handler-conn` sets `(Some conn)` before the dispatch and `(None)`
  after it.
- Add `tourist-current-conn : (Option Conn)`.
- Keep `tourist-request-conn : Conn` as a wrapper that panics outside a request
  (document it), and move tourist-ws's `ws-route!` to the new reader.

**Why thread-local and not a dynvar:**
- One worker thread owns the request end to end (`app.tur:519-524`).
- A helper thread must not inherit the socket: writing to the Conn from a
  second thread races the worker.
- No caller wants a nested override.

**Wins:**
- Correct under `tur jit`.
- A typed Conn instead of an `:int` carrier.
- No stale Conn between requests.

**Tests** (in `spices/tourist/tests/`):
- Two threads set distinct Conns and each reads back only its own.
- The slot reads `(None)` after a request completes.
- The first CI run must use the spices CI's sanitized Debug `tur`, so that
  reading the stored `(Option Conn)` more than once is shown not to free its
  box. The release-binary probe read it twice without trouble, but it ran
  unsanitized.

**Size:** small. It is one file plus a test.

### SPA-2. tourist's request context -> dynvar -- medium-large, two decisions open

**Sites:**
- `__tourist-handler` (`app.tur:395-400`) creates and frees the Ctx.
- The 80 `ctx : Ctx` annotations (sweep below):
  - tourist: `param` 21, `request` 17, `routing` 7, `app` 6, `middleware` 5,
    `static` 1;
  - tourist-session: `ctx` 13, `csrf` 7, `mw` 2;
  - tourist-ws: `route` 1.

**Var** (root per decision 1):

```turmeric
(defdynamic *current-ctx* :Ctx (null-ctx))
```

**Conversion:**
1. `__tourist-handler` wraps the dispatch in
   `(binding [*current-ctx* ctx] ...)`, between `tourist-ctx-new` and
   `tourist-ctx-free`. tourist owns both ends, and httpd does not change.
2. Add an argument-less reader, `(current-ctx) : (Option Ctx)`. Keep the
   explicit-ctx entry points as they are, and migrate handlers when convenient,
   since both call styles work side by side.
3. `use-around!` middleware binds over the downstream call. Nested `binding`
   forms compose and restore.

**Why a dynvar:** the old reasons still hold. A binding restores around
middleware, and a helper thread started with `spawn-conveying` sees the
request's ctx. The probe confirms both: a conveyed child sees the binding, and a
plain spawn sees the root.

**Decision 1 -- the "no request" root.**

- (a) `(Option Ctx)` is the shape CLAUDE.md prefers. A dynvar of it did not
  compile until 2026-10-18; it does now, with a bare `(None)` root, and
  `spawn-conveying` carries it (`tests/fixtures/defdynamic-option-convey`).
- (b) The alternative is a null `Ctx` as the root. `(current-ctx)` would then
  turn it into `(Option Ctx)`, so the sentinel never leaves that one function.
  tourist already uses a null handle for `Pattern` (`types.tur:78`).

This recommended (b) while (a) did not compile. Now that it does, (a) is
open: re-decide when the work starts.

**Decision 2 -- fiber servers.** Dynvar bindings do not move with fibers.
- Today nothing can hit this. spices/httpd exposes only pool, spawn and
  thread servers (`spices/httpd/src/httpd/server.tur:383-607`). stdlib's
  `httpd-new-async`, which runs a fiber per request
  (`stdlib/httpd.tur:3529-3543`), is not reachable from tourist.
- So: land SPA-2 with a comment at the `binding` site, and a line in tourist's
  README, saying tourist must not run on a fiber server until the fiber report
  is fixed.
- Alternatively, fix the compiler first. Recommend the comment and README note:
  there is no fiber path for it to break.

**A third point to settle in review: conveyed children vs. the Ctx's
lifetime.** `tourist-ctx-free` runs when the handler returns. A conveyed child
that outlives the request would read a freed Ctx. So the rule should be "join
what you convey before you return", stated in the reader's docstring.

**Caveat retained:** `Ctx` must stay non-substructural, a `defopaque` without
`:linear`, or `TUR-E0603` refuses the dynvar.

**Tests** (in `spices/tourist/tests/`):
- Nested middleware sees the binding, and it is restored afterwards.
- A `spawn-conveying` child that is joined before the handler returns sees the
  ctx.
- A plain spawn sees the root.
- `(current-ctx)` outside a request is `(None)`.

**Size:** medium-large. Each call-site change is mechanical.

---

## Dropped targets

- **B. raygui.** There is no binding stack to replace, and raygui's C reads
  its own globals. If someone wants scoped GUI state, the tool is a
  `with-gui-state` combinator that saves with `gui-get-state`, sets the new
  state, and restores it with `defer`. That is not a dynvar and not this
  plan. Separately, the setters take `state : int` with a 0-3 convention; an
  enum ADT is the CLAUDE.md fix.
- **C. ansi.** There is no sink or mode state to convert.
  - An output sink that tests can capture belongs to a stdlib output type
    (the sibling plan's ADO-4 `*current-out*`), which ansi would adopt rather
    than define.
  - Color mode needs no new state. If it ever gets some, make it a
    process-wide `^mut` global holding a `ColorMode` ADT, not a thread-local
    and not an `:int`.
- **D. valkey, E. postgres, F. opengl.** Their handles are `:linear` by
  design. A dynvar or thread-local could carry one only through an erasing
  wrapper. That is the `:int` stand-in CLAUDE.md forbids, and it would give up
  the linearity check that keeps the handles safe. Scoped helpers such as
  `with-transaction` keep taking the handle explicitly.
- **Unchanged from before:**
  - math, linalg, regex, json and frame are pure data.
  - c-dsl and glsl are compile-time only.
  - ecs leans on an explicit world; revisit it after SPA-2.

---

## Phases

| Phase | Target | Needs | Gate |
| --- | --- | --- | --- |
| SPA-1 | tourist Conn slot -> `^thread-local` | nothing | none |
| SPA-2 | tourist request context -> dynvar | decisions 1 and 2 | pool and spawn servers only, until the fiber report is fixed |

Each phase is one turmeric-spices PR, and each stands alone.

---

## Appendix: spice-tree sweep (2026-10-17, turmeric-spices `main` 7465a08)

The old text asked for this sweep before the tourist work. Here it is.

- **`defdynamic` / `binding`:** none, in any spice.
- **Hand-rolled thread-locals in Turmeric source:** one,
  `tourist/app.tur:540` (SPA-1). `html/c/html/turhtml.c:110-112` keeps the
  DOM entry points' error state per thread in a vendored C file. That is the
  right tool there, so leave it alone.
- **Parameters threaded more than five times,** by `name : Type`. Counts are
  annotations, in any parameter position:

| Count | Parameter | Spice | Class |
| --- | --- | --- | --- |
| 80 | `ctx : Ctx` | tourist, tourist-session, tourist-ws | dynvar candidate (SPA-2) |
| 44 | `c : Canvas` | plutovg | leave alone -- the drawing target is the API |
| 31 | `conn : Conn` | httpd (4 files), postgres (3), ws-server (1) | leave alone -- postgres's is `:linear`, and httpd's is the per-request socket (SPA-1 covers tourist's slot for it) |
| 23 | `w : SaxWriter` | dom-core | leave alone -- explicit writer |
| 17 | `c : Client` | valkey | leave alone -- `:linear` |
| 12 | `c : NngCtx` | nng | leave alone -- `:linear` |
| 7 | `db : Db` | sqlite | leave alone -- `:linear` |
| 9, 9, 6, 6, 6, 6, 5 | `w : Wav`, `conn : WsConn`, `w : Window`, `conn : TlsConn`, `c : PNCounter`, `c : DotContext`, `ctx : TlsCtx` | wav; tourist-ws, ws-client, ws-server; opengl; httpd, tls; crdt | leave alone -- explicit values or handles, no ambient state |

---

## Open questions

- **`tourist-request-conn`'s signature.** Keep the panicking `: Conn` reader
  beside the new `(Option Conn)` one, or change it in a minor bump? Recommend
  keeping both for one release.
- **Should a conveyed child be able to outlive the request?** The answer above
  is "no, join it first". The alternative is reference-counting the Ctx, which
  is a bigger change than this plan wants.
