# Dynamic Var Adoption Plan

> **Status:** Proposed (revised 2026-06-24)
>
> **Prerequisite:** DV0-DV4 complete (`-Xdynamic-vars`); see
> [docs/archive/history/dynamic-vars-plan.md](../archive/history/dynamic-vars-plan.md)
> and [docs/guides/dynamic-vars-guide.md](../guides/dynamic-vars-guide.md).
>
> **Scope:** Two things, in this order.
>
> 1. **C-level dynvar runtime API.** Today every `defdynamic` emits a
>    per-module static root plus a `pthread_key_t` that only Turmeric-side
>    `binding` / read sites can touch (see
>    `src/compiler/emit_module.c:8174` and `src/compiler/emit_expr.c:4779`).
>    Nothing in `src/` C code can read or rebind a Turmeric dynvar. That
>    blocks the highest-payoff target -- routing the compiler's own
>    `fprintf(stdout, ...)` / `fprintf(stderr, ...)` calls through
>    `*current-out*` / `*current-err*` -- because the readers live in C.
> 2. **Adoption sweep with a dynvar-vs-thread-local distinction.** Not every
>    "ambient singleton" wants binding-stack semantics; several want plain
>    per-thread state. Decide per target, then convert.

---

## Motivation

The dynvar machinery exists and is exercised by fixtures, but production
code still rolls the same pattern by hand: `static` C globals plus a
per-thread save/restore dance, or `FILE*` arguments threaded through dozens
of function signatures.

Two findings from the first research pass force a revision:

- **C cannot read Turmeric dynvars today.** Codegen lowers
  `(binding [*v* x] body)` to local pthread_setspecific calls keyed off
  `_dynvar_key_<mangled>`, but those keys are module-static C symbols with
  no public accessor. Until the runtime exposes a stable C API, only
  Turmeric code can participate, which excludes the compiler's own dump /
  diagnostic surface -- the biggest leverage point.
- **Some "globals" want thread-local, not dynvar.** A dynvar carries a
  binding stack with per-call-site `__attribute__((cleanup))` restore; that
  costs codegen, a frame allocation per `binding`, and pthread_setspecific
  on entry/exit. State that needs per-thread isolation but never needs to
  unwind to a prior value (mock-time singleton, fiber cancel flag, the
  symbol-table pointer in production mode) should be a plain
  `_Thread_local` -- cheaper, simpler, and accurately typed for what it
  is.

The goal here is to **pick the few sites where dynvars demonstrably remove
ceremony or enable test isolation that is currently impossible**, demote
the rest to thread-locals or leave them alone, and unlock the
output-sink win by adding the missing C-side runtime.

Non-goals: converting every CLI flag, every static global, or anything on a
hot inner loop (`get` is cheap but not free, and configuration that is set
once per process gains nothing from scope).

---

## Dynvar vs. thread-local -- decision rubric

Use this checklist before classifying any target. The default should be
"plain thread-local or no change at all"; reach for a dynvar only when the
binding-stack semantics actually buy something.

A target wants a **dynvar** when *all* of the following hold:

- Callers want to override the value for a **lexical / dynamic scope** and
  have it auto-restore on exit (success, exception, early return, fiber
  yield).
- Nested overrides should compose (LSP request inside a test inside batch
  mode; capture inside a `with-style` inside a `with-state`).
- The override is on a **cold-to-warm path** -- per request, per test, per
  GUI frame -- not in an inner loop where a TLS read is too much.
- The carried type is non-substructural (TUR-E0603 rejects substructurals
  from dynvars).

A target wants a **thread-local** when:

- Each thread needs its own value, but no caller ever wants to *temporarily
  override and restore* it within a thread.
- The "stack" of values is at most 1; tests reset it explicitly at setup /
  teardown rather than relying on scope unwind.
- A cheap `_Thread_local T x;` set/get is enough.

A target wants **neither** (leave the global as-is) when:

- Set once at startup from a CLI flag and read on hot paths.
- Already an explicit argument and the threading is load-bearing for
  correctness (arenas, borrow-tracked handles).

---

## Phase 0 (NEW) -- C-level dynvar runtime API

**Why first:** Target A below (`*current-out*` / `*current-err*`) needs C
code in `src/` to read a Turmeric dynvar. Phase 0 is the unblocker.

**Deliverables:**

1. `src/runtime/dynvar.h` / `src/runtime/dynvar.c` exposing:

   ```c
   typedef struct TurDynFrame { struct TurDynFrame *prev; void *value; } TurDynFrame;

   /* Stable handle for a dynvar -- one per defdynamic, owned by the
      emitted module init. */
   typedef struct TurDynVar {
       pthread_key_t key;     /* same key codegen already mints */
       void         *root;    /* pointer to the static root slot */
       size_t        size;    /* sizeof(T) so the API is type-erased but
                                 still memcpy-correct */
   } TurDynVar;

   void  tur_dyn_get  (TurDynVar *v, void *out);
   void  tur_dyn_push (TurDynVar *v, TurDynFrame *frame, const void *value);
   void  tur_dyn_pop  (TurDynVar *v, TurDynFrame *frame);

   /* Convenience for the common :ptr<T> case. */
   void *tur_dyn_get_ptr (TurDynVar *v);
   ```

2. **Codegen change** in `src/compiler/emit_module.c` and
   `emit_expr.c:4779-4839`: in addition to the existing per-module
   `_dynvar_key_<mname>` / `_dynvar_root_<mname>` / `_dynvar_pop_<mname>`
   symbols, emit a `TurDynVar TUR_DYNVAR_<mname>` aggregate with external
   linkage so other translation units (including the compiler's own C) can
   reference the var by stable symbol. The current static codegen stays
   the fast path inside the same module.

3. **`extern-dynvar` declaration form** on the Turmeric side so a C-defined
   dynvar (the two below) can appear in stdlib without a `defdynamic`
   body. The C runtime defines and initializes the storage; Turmeric just
   `extern`s it.

4. The two stdout/stderr dynvars themselves live in C as the canonical
   bootstrap example:

   ```c
   /* src/runtime/dynvar_io.c */
   TurDynVar TUR_DYNVAR_current_out = { /* key */, &g_current_out_root, sizeof(FILE*) };
   TurDynVar TUR_DYNVAR_current_err = { /* key */, &g_current_err_root, sizeof(FILE*) };
   ```

   With matching `(extern-dynvar *current-out* :ptr<FILE>)` in
   `stdlib/io.tur`.

5. Tests: a fixture that calls a C helper (via inline-C) which reads
   `*current-out*` via `tur_dyn_get_ptr`, observes both the root and a
   `binding` override, and verifies the value matches what Turmeric-side
   readers see.

**Estimated size:** medium. Mostly codegen + a thin runtime; no new
language semantics.

---

## Phase 0b (NEW) -- `defthreadlocal` form

**Why:** several adoption targets want per-thread isolation without the
binding stack. Today the only Turmeric-visible knob is `defdynamic`,
which forces every consumer to pay frame allocation and a
pthread_setspecific on every override. A first-class `defthreadlocal`
lowers to a plain `_Thread_local T x = root;` with `get`/`set` ops -- no
binding form, no stack.

**Deliverables:**

- `(defthreadlocal *name* :T root-expr)` parses, elaborates, and
  codegens to `_Thread_local T <mangled> = <root>;` plus accessor
  functions.
- Compile-time error if `binding` is used on a `defthreadlocal` (point
  the user at `set!` and a manual try/finally pattern, or suggest
  promoting to `defdynamic`).
- `extern-threadlocal` mirror for C-defined slots, same shape as
  `extern-dynvar`.
- Diagnostic when a target accepts a substructural type -- thread-locals
  carry the same TUR-E0603 prohibition because a value escaping its
  thread-local slot would duplicate it.

**Estimated size:** small-medium; one new declaration form, one codegen
template, no new IR.

---

## Targets (revised, classified)

### A. Compiler output / dump sink -- **dynvar**, HIGH payoff

**Sites:** `src/main.c:400-423` and ~575 `fprintf(stdout|stderr, ...)` /
~2000 `printf` calls across `src/compiler/`; explicit `FILE*` parameters
on `cps_ir_dump_program`, `effect_check_dump_effects`,
`cps_dump_coloring`, etc.

**Vars (defined in C, declared in `stdlib/io.tur`):**

```turmeric
(extern-dynvar *current-out* :ptr<FILE>)
(extern-dynvar *current-err* :ptr<FILE>)
```

**Why dynvar (not thread-local):** the override is scope-shaped -- a test
or LSP request wants captured output *for this call only*, then auto-
restore on exit. A plain TLS slot would force every caller to set+restore
manually, which is the pattern we're trying to delete.

**Conversion:**

1. Wait for Phase 0; without the C-side API this target is unbuildable.
2. Replace the explicit `FILE*` parameter on dump APIs that are always
   `stdout` today with a `tur_dyn_get_ptr(&TUR_DYNVAR_current_out)` read.
3. Migrate the hot subset of `fprintf(stdout, ...)` callers; **do not**
   rewrite every `printf` in the codebase.
4. Test capture wraps the test harness entry point in a single
   `binding` form -- no per-call plumbing.

**Open question:** `:ptr<FILE>` carrier vs. a `defopaque OutSink :ptr<void>`
wrapper. Per the no-lazy-`:int` rule we want the wrapper; the wrapper goes
in `stdlib/io.tur` alongside the extern-dynvar declaration.

**Estimated size:** ~30-50 call sites touched, no signature explosion.

### B. Diagnostic output mode (JSON vs. human vs. LSP) -- **dynvar**, HIGH payoff

**Sites:** `src/compiler/diag.h:366-387`, `src/main.c:3428-3440`. Today
`diag_lsp_begin/flush/end` plus `diag_set_json_output` toggle a global.

**Var:**

```turmeric
(defdynamic *diag-mode* :int 0)   ; 0 = human, 1 = json, 2 = lsp-buffer
```

**Why dynvar (not thread-local):** nested scope is the point. An LSP
request inside a batch run inside a test should each see their own mode
and restore on exit. A plain TLS slot loses that.

**C-side reader:** uses the Phase 0 API. If we want to keep `diag.c`
self-contained, define `*diag-mode*` in C (extern-dynvar on the Turmeric
side) so `diag_emit_*` can `tur_dyn_get` it directly.

**Estimated size:** small (1 var, ~5-10 read sites, 2-3 set sites).

### C. HAMT key-eq / retain / release hooks -- **dynvar**, MEDIUM payoff

**Sites:** `src/runtime/hamt.c:249-295, 343-354`. Already has a manual
save/restore pattern (`hamt_eq_hook_save` struct) -- a hand-rolled
binding stack.

**Vars (C-defined via Phase 0):**

```turmeric
(extern-dynvar *hamt-key-eq*       :ptr<void>)
(extern-dynvar *hamt-key-eq-ctx*   :ptr<void>)
(extern-dynvar *hamt-key-retain*   :ptr<void>)
(extern-dynvar *hamt-key-release*  :ptr<void>)
```

**Why dynvar:** the existing save/restore IS a binding stack. Replacing
with dynvars deletes the boilerplate and makes re-entrancy automatic.
The four vars travel together; consider one dynvar carrying a struct
instead.

**Estimated size:** medium; mechanical.

### D. Global symbol table -- **thread-local** (revised), MEDIUM payoff

**Sites:** `src/runtime/symbols.c:16-91` (`g_sym_tab`, `g_sym_cap`,
`g_sym_len`, `g_sym_mu`).

**Revised classification:** thread-local, not dynvar. The use case is
test isolation -- each test wants its own intern table -- and tests set
the table at fixture setup, tear it down at fixture teardown. No call
site wants to "temporarily switch tables for this expression and restore."
A `_Thread_local TurSymTable *current_sym_table` is the right shape.

**Form:**

```turmeric
(extern-threadlocal *current-sym-table* :ptr<TurSymTable>)
```

**Conversion:** thread the table pointer through `tur_sym_intern` to read
the TLS slot instead of `g_sym_tab`. The existing global becomes the
default value the slot is initialized to.

**Caveat:** symbol IDs are pointer-keyed by the table they came from;
crossing thread boundaries with different tables changes identity.
Document this as a fundamental thread-locality, not a dynvar accident.

### E. Mock-time singleton -- **thread-local** (revised), LOW payoff

**Sites:** `stdlib/time.tur:64-71` (`static __tur_mock_time_singleton`).

**Revised classification:** thread-local, not dynvar. Concurrent tests
want isolation, not scope unwind -- a test sets mock time, runs, asserts,
tears it down. There is no nesting use case ("mock time for this
expression but restore the outer mock"). A `defthreadlocal` is exactly
the shape; we were reaching for the heavier tool.

**Form:**

```turmeric
(defthreadlocal *mock-time* :int 0)
```

**Estimated size:** tiny (1 var, 3-4 read/write sites in
`stdlib/time.tur`); this becomes the canonical first user of Phase 0b.

### Explicitly skipped

- **Compiler dump flags** (`g_dump_*`, `g_emit_abi_trace`, etc.): set
  once from CLI, read on hot paths, no scope benefit. Not even thread-
  local-worthy.
- **`SnippetOpts`**: already explicit-passed; not a global.
- **Arena allocator (`e->arena`)**: large rewrite for marginal gain; the
  explicit threading is load-bearing for nested elaborations.

---

## Phases (revised)

| Phase | Targets | Gate |
|---|---|---|
| ADO-0 | C-level dynvar runtime API + `extern-dynvar` | unblocks A, B, C-as-extern |
| ADO-0b | `defthreadlocal` / `extern-threadlocal` form | unblocks D and E without forcing dynvar semantics |
| ADO-1 | E (mock-time) as `defthreadlocal` | smallest; first real consumer of Phase 0b |
| ADO-2 | B (diag mode) | unlocks cleaner LSP/test paths |
| ADO-3 | C (HAMT hooks) | deletes existing save/restore code |
| ADO-4 | A (output sink) | biggest payoff; uses Phase 0 directly |
| ADO-5 | D (symbol table) as `defthreadlocal` | most invasive; defer until A is bedded in |

Each phase is independently shippable. Stop at any point if the payoff
falls below the rewrite cost for the next phase -- the goal is leverage,
not coverage.

---

## Sweep -- "is this thing a dynvar, a thread-local, or fine?"

Before A lands, do one read-only sweep across `src/runtime/` and `src/compiler/`
for `static` globals and `g_*` symbols, classifying each into:

- **dynvar candidate** (scope unwind matters)
- **thread-local candidate** (per-thread, no scope)
- **leave alone** (set-once-CLI, hot path, or already-threaded explicit)

Land the sweep as a follow-up doc under `docs/reported/` if anything
surprising shows up; otherwise just expand this plan's target list. The
sibling spices plan ([dynvar-adoption-spices-plan.md](spices/dynvar-adoption-spices-plan.md))
does the same sweep across `../turmeric-spices/`.

---

## Test plan per phase

- Add one fixture under `tests/fixtures/dynvar-adopt-<target>/` (or
  `tls-adopt-<target>/` for thread-local targets) that exercises both
  the root/default value and an override.
- For A, add a fixture that uses `spawn-conveying` to verify a child
  thread sees the parent's capture (this also acts as a real-world
  smoke test for DV3 outside the existing convey-isolation fixture).
- For D and E, add a fixture that spawns two threads, sets a different
  TLS value in each, and verifies isolation -- this is the property a
  thread-local exists to provide.
- Verify the full suite stays at zero `FAIL` after each phase
  (`bash tests/run.sh 2>&1 | grep "^FAIL"`).

---

## Open questions

- **Root-init known bug.** The DV2 "root-value initializer not emitted
  when explicit `defn main` is present" limitation still applies. For
  targets where the root is `stdout`/`stderr`/`default-sym-table`, the
  initial value matters; Phase 0's C-defined storage sidesteps this by
  initializing in the runtime constructor, but a Turmeric-defined dynvar
  (B, E if it stayed a dynvar) still has to fix or work around it.
- **`extern-dynvar` and `extern-threadlocal` linker story.** Cross-
  module dynvar identity must survive separate compilation. Confirm the
  emitted `TUR_DYNVAR_<name>` symbol has external linkage and that the
  ABI cache is invalidated when the runtime header changes.
- **`defthreadlocal` and `spawn-conveying`.** A dynvar is conveyed to a
  child thread on spawn (DV3). A thread-local is *not* -- the child sees
  the slot's default. Document this as a load-bearing difference; it is
  another reason mock-time wants thread-local (parallel tests must not
  inherit each other's mock clock) and `*current-ctx*` (in the spices
  plan) wants dynvar (worker threads should inherit the request).
