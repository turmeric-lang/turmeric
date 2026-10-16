# Dynamic Var Adoption -- Spices Plan

> **Status:** Proposed (revised 2026-06-24)
>
> **Prerequisite:** DV0-DV4 complete (`-Xdynamic-vars`); see
> [docs/archive/history/dynamic-vars-plan.md](../../archive/history/dynamic-vars-plan.md)
> and [docs/guides/dynamic-vars-guide.md](../../guides/dynamic-vars-guide.md).
>
> **Sibling plan:** [dynvar-adoption-plan.md](../dynvar-adoption-plan.md)
> covers compiler-tree adoption and lands the C-level dynvar runtime API
> and the `defthreadlocal` form. **Do not start the spices work until
> ADO-0 and ADO-0b in the sibling plan are in.** Several spice targets
> here re-classify from "dynvar" to "thread-local" and need the new form
> to land cleanly.
>
> **Scope:** Replace ambient handles, threaded ctx/conn params, and
> hand-rolled "current X" globals in selected spices with either
> `defdynamic` (scope unwind matters) or `defthreadlocal` (per-thread
> isolation only). Singleton-busting and test-replacement only; no API
> redesign beyond what these forms naturally remove.

---

## Motivation

Several spices independently re-invent the binding-stack pattern: tourist
threads `Ctx` through every handler, postgres threads `Conn`, raygui
hand-rolls a state save/restore around C statics. The dynvar machinery
exists -- the work here is conversion, not design.

The first research pass uncovered two cross-cutting decisions that drive
this revision:

- **Some spice targets want thread-local, not dynvar.** Notably ansi's
  color-mode flag (set once by CLI parsing per process, never overridden
  in scope) and opengl's `*gl-program*` / `*gl-vao*` / `*gl-vbo*` (the GL
  driver already owns the real state; a TLS mirror is enough). The
  `defthreadlocal` form from the sibling plan covers them without paying
  binding-stack overhead.
- **The "carry context to spawned worker" question is the deciding test.**
  Dynvars are conveyed to child threads on `spawn-conveying`; thread-
  locals are not. tourist's `*current-ctx*` MUST be conveyed (a worker
  thread handling the request needs the parent's ctx); mock-time MUST
  NOT (parallel tests must not inherit each other's clock). Use this as
  the primary classifier when borderline.

Non-goals: rewriting spice APIs, removing typed handles, or "everything
should be a dynvar." Each target below has a concrete win that justifies
the change.

---

## Dynvar vs. thread-local -- decision rubric (cross-reference)

See the sibling plan for the full rubric. Quick form:

- **Dynvar** when callers want lexical / dynamic scope with auto-restore,
  nested overrides should compose, and (for spawn) the child should inherit
  the parent's binding.
- **Thread-local** when per-thread isolation is enough, no caller wants a
  binding stack, and (for spawn) the child should start from the default.
- **Leave alone** when the global is set once from a CLI flag or the
  explicit threading is load-bearing.

---

## Targets (revised, classified)

### A. `tourist` + `httpd` -- request context -- **dynvar**, HIGH payoff

**Sites:** `../turmeric-spices/spices/tourist/src/tourist/app.tur:79-88`
and ~29 handler/middleware/matcher functions that take `[ctx : Ctx]` as
their first parameter. `httpd` is the producer (sets per-request state in
its worker thread).

**Var (in a new `tourist/src/tourist/dynvars.tur`):**

```turmeric
(defdynamic *current-ctx* :ptr<Ctx> nil-ctx)
```

**Why dynvar (not thread-local):**

- Scope unwind matters: middleware wraps a downstream handler and wants
  the ctx restored on return. A binding stack is the natural fit.
- `spawn-conveying` semantics matter: if a handler spawns a background
  worker mid-request, the child thread should inherit the request's ctx.
  This is exactly the property thread-locals don't give us.

**Conversion:** unchanged from the prior revision:

1. Declare `*current-ctx*` in tourist.
2. `httpd`'s request dispatch wraps the user handler in
   `(binding [*current-ctx* req-ctx] (user-handler))`.
3. Tourist's middleware/matcher API gains an arg-less reader
   (`(current-ctx)`) and old explicit-ctx entry points become thin
   wrappers that pass `*current-ctx*`.
4. Migrate handlers opportunistically -- both call styles work during
   transition.

**Caveat:** `Ctx` must be non-substructural to be carried by a dynvar
(TUR-E0603 rejects substructurals). Today it is; preserve that.

**Estimated size:** medium-large; touches both spices, but the change
per call site is mechanical.

### B. `raygui` -- GUI state and style stack -- **dynvar**, HIGH payoff, low risk

**Sites:** `../turmeric-spices/spices/raygui/src/raygui/core.tur:102-206`.
`gui-set-state` / `gui-set-style` / `gui-load-style` already manipulate C
statics with hand-rolled save/restore.

**Vars:**

```turmeric
(defdynamic *gui-state* :ptr<GuiState> default-gui-state)
(defdynamic *gui-style* :ptr<GuiStyle> default-gui-style)
```

**Why dynvar (not thread-local):** the existing pattern IS a binding
stack -- `(with-style s ...)` / `(with-state s ...)` should pop back on
exit, including on early return. Thread-locality alone wouldn't deliver
that.

**Conversion:** convert the existing setter/getter pairs to read/write
the dynvar; offer `with-state` / `with-style` macros that expand to
`binding`. Keep the underlying C `Raygui*` calls -- only the Turmeric
wrapper changes.

**Estimated size:** medium; isolated to one spice.

### C. `ansi` -- terminal sink (dynvar) + color mode (thread-local) -- MEDIUM payoff

**Sites:** `../turmeric-spices/spices/ansi/src/ansi/color.tur:27-95`.
Every color/style helper writes to stdout via `printf`.

**Revised: split into two vars with different shapes.**

```turmeric
(defdynamic       *ansi-sink*       :ptr<FILE> stdout-handle)
(defthreadlocal   *ansi-color-mode* :int       0)   ; 0 = auto, 1 = always, 2 = never
```

**Why split:**

- `*ansi-sink*` is scope-shaped: tests want to capture output for a
  block, then restore. Spawn semantics matter too -- a worker writing
  ansi-colored progress should inherit the parent's capture sink.
  Dynvar.
- `*ansi-color-mode*` is set once at startup from `--no-color` and read
  on every emit. No call site wants `(binding [*ansi-color-mode* 2]
  ...)` for a single println. A `_Thread_local int` is cheaper, and
  conveying it to spawned workers gains nothing because they read the
  same default. Thread-local.

**Why the sink dynvar pairs with the compiler plan:** the sibling plan's
ADO-4 (`*current-out*`) lands the C-side dynvar API and the
extern-dynvar declaration form. Ansi's sink reuses that machinery if it
wants the same FILE* under the hood, or stands on its own as a
Turmeric-side dynvar if the spice prefers its own carrier.

**Estimated size:** small; ~10-15 emit sites.

### D. `valkey` -- Redis client handle -- **dynvar**, MEDIUM payoff

**Sites:** `../turmeric-spices/spices/valkey/src/valkey/` (cmd, reply,
client, pubsub modules). Pub/sub and queue helpers thread the client.

**Var:**

```turmeric
(defdynamic *valkey-client* :ptr<ValkeyClient> nil-client)
```

**Why dynvar:** request-scoped client selection (different namespaces
per tenant) and `(with-client c ...)` are the use cases; both want scope
unwind. Workers spawned to run a pipelined batch should inherit the
caller's client -- another reason dynvar over thread-local.

**Conversion:** same shape as tourist; add arg-less readers, keep the
explicit-client API as wrappers.

**Estimated size:** medium.

### E. `postgres` -- connection handle -- **dynvar**, MEDIUM payoff, design question

**Sites:** `../turmeric-spices/spices/postgres/src/postgres/db.tur:197-322`.
25+ functions take `[^borrow conn : Conn]`.

**Var:**

```turmeric
(defopaque ConnHandle :ptr<void>)        ; non-substructural carrier
(defdynamic *current-conn* :ConnHandle nil-conn)
```

**Why dynvar (not thread-local):** `(with-transaction ...)` IS a binding
stack -- nested transactions, savepoints, and per-test fixture
connections all want scope unwind. A pooled worker that spawns helpers
mid-transaction wants the connection conveyed.

**Design caveat:** `Conn` is `^borrow`-typed today and `defdynamic`
rejects substructural types (TUR-E0603). The dynvar must carry a
non-substructural handle (a `defopaque` wrapper). Linearity then shifts
from the type system to discipline at the dynvar boundary -- production
code continues to use the typed API; the dynvar is the test/scoping
seam, not the primary handle.

**Decision required before starting:** are we OK with the dynvar
carrying an un-borrowed handle, or do we want to lift the substructural
restriction on `defdynamic` for this case? Recommend the former -- the
restriction exists for sound reasons.

**Estimated size:** medium, plus the design call.

### F. `opengl` -- current program / VAO / VBO -- **thread-local** (revised), LOW payoff

**Sites:** `../turmeric-spices/spices/opengl/src/opengl/shaders.tur:147-150`
and bind helpers.

**Revised classification:** thread-local, not dynvar. The driver owns
the real GL state machine and is itself thread-local-by-context.
Mirroring it into a Turmeric dynvar gains nothing -- nested
`(with-program p ...)` does not save/restore the *driver's* state on
its own, and conveying a program handle to a spawned worker is
incorrect (workers need their own GL context). What's actually wanted
is per-thread cached values to avoid redundant glUseProgram calls; a
thread-local gives exactly that.

**Vars:**

```turmeric
(defthreadlocal *gl-program* :int 0)
(defthreadlocal *gl-vao*     :int 0)
(defthreadlocal *gl-vbo*     :int 0)
```

**With-program style helpers:** still possible via a hand-written
try/finally (`set!` + cleanup), but no longer a `binding` form. Document
the GL-context caveat alongside.

**Estimated size:** small per var; defer unless someone is actively
hitting the redundant-bind issue.

### Explicitly skipped

- `math`, `linalg`, `regex`, `json`, `frame`: pure/data, no ambient state.
- `c-dsl`, `glsl`: compile-time codegen, no runtime singleton.
- `ecs`/`ecs-raylib`: the world pointer is a candidate, but ECS APIs
  already lean on an explicit world; the win is smaller than tourist's
  ctx threading. Revisit after A.

---

## Phases (revised)

| Phase | Targets | Gate |
|---|---|---|
| -- | (sibling plan ADO-0 + ADO-0b must be in) | C-side dynvar API + `defthreadlocal` form available |
| SPA-1 | C (ansi sink dynvar + color-mode thread-local) | smallest spice; validates both forms in one spice |
| SPA-2 | B (raygui state stack) | self-contained; deletes existing boilerplate |
| SPA-3 | A (tourist + httpd ctx) | biggest payoff; coordinate across both spices |
| SPA-4 | D (valkey client) | mirrors A on smaller scope |
| SPA-5 | E (postgres) | gated on the substructural-vs-dynvar design call |
| SPA-6 | F (opengl) as thread-local | only if a real consumer asks for it |

Each phase is independently shippable in `../turmeric-spices/`. Stop at
any phase where the next target's payoff falls below its conversion
cost -- the goal is leverage, not coverage.

---

## Spice-tree sweep -- classify before adopting

Before SPA-3 (the largest target) lands, do one read-only sweep across
`../turmeric-spices/spices/*/src/` for `static` C globals, hand-rolled
"current X" wrappers, and any function with `[ctx : T]` / `[conn : T]`
threading through more than ~5 sites. Classify each into:

- **dynvar candidate** (scope unwind + spawn-convey both matter)
- **thread-local candidate** (per-thread, no scope, no convey)
- **leave alone** (set-once-CLI, hot path, or already-threaded explicit)

Land the classification table as an appendix to this plan rather than a
separate doc -- the goal is to keep the conversion decisions
co-located with the conversion work.

---

## Sequencing relative to the compiler-tree plan

The compiler-tree plan ([dynvar-adoption-plan.md](../dynvar-adoption-plan.md))
must land ADO-0 (C-level runtime API) and ADO-0b (`defthreadlocal`) before
any spice phase starts -- those are hard prerequisites for SPA-1 (ansi's
thread-local color mode and sink dynvar) and SPA-6 (opengl thread-locals).
The mock-time and diag-mode phases (ADO-1, ADO-2) on the compiler side are
safe to run in parallel with SPA-1 and SPA-2; SPA-3 (tourist) still wants
the compiler-side output-sink work (ADO-4) bedded in first because
tourist tests will reuse that capture machinery.

---

## Test plan per phase

- Add a fixture under
  `../turmeric-spices/spices/<spice>/tests/dynvar-adopt/` (or
  `tls-adopt/` for thread-local targets) that exercises both the root /
  default value and an override.
- For A and D, add a `spawn-conveying` fixture: parent binds a fake,
  child worker sees it. Doubles as a real-world DV3 smoke test outside
  the existing convey-isolation fixture in this repo.
- For F (opengl thread-locals) and the ansi color-mode thread-local,
  add a two-thread fixture asserting isolation -- two threads with
  distinct values, neither leaks into the other. This is the property
  a thread-local exists to provide.
- Each spice's existing suite must stay green
  (`bash tests/run.sh 2>&1 | grep "^FAIL"` empty) after each phase.

---

## Open questions

- **Cross-spice dynvar declarations.** `*current-ctx*` belongs to
  tourist but is set by httpd. Either tourist exports the var and
  httpd imports it, or both depend on a small shared
  `tourist-dynvars` spice. Recommend the export-from-tourist route --
  fewer moving parts, and httpd already depends on tourist in
  practice.
- **`Ctx` shape compatibility.** Confirm `Ctx` is non-substructural
  and that a `:ptr<Ctx>` (or the existing handle representation) fits
  the dynvar type rules without lifting TUR-E0603.
- **`extern-dynvar` and `extern-threadlocal` across spice boundaries.**
  The sibling plan's Phase 0 makes the runtime symbol externally
  linkable; confirm the spice loader and ABI cache cope when a spice
  declares an `extern-dynvar` whose definition lives in another spice
  (rather than in the runtime). If this turns out to require an extra
  loader hop, prefer keeping each spice's dynvars self-contained.
- **Root-init known bug.** The DV2 "root-value initializer not emitted
  when explicit `defn main` is present" limitation still applies. For
  targets whose root must be a real handle (default sink, default
  state, default client), either fix the root-init bug first or
  initialize from spice glue before user code runs.
