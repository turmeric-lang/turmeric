# Dynamic Var Adoption Plan

> **Status:** Proposed, nothing started. **Revised 2026-10-17** against
> turmeric v0.65.5 (`main` 3d9e02a2a). The 2026-06-24 revision had five
> targets and two enabling phases. All five targets are dropped here. Each
> one failed on the tree (see "What changed"), and so did the C-side dynvar
> API that four of them waited on. What is left is smaller and real:
> `stdlib/dynvar.tur` declares four standard dynvars and documents what they
> do, but **nothing in stdlib reads any of them**. This plan is now about
> making stdlib honor those vars, or retiring them.
>
> **Prerequisites:** none to start. `defdynamic`, `binding` and
> `spawn-conveying` are always on: `-Xdynamic-vars` has been an
> accept-and-warn no-op since v0.24.0 (`src/runtime/globals.c:194`).
> `(def ^thread-local ...)` (mutable-globals G4b, 2026-08-05) covers the
> per-thread form the old ADO-0b proposed as `defthreadlocal`. See
> [dynamic-vars-guide](../guides/dynamic-vars-guide.md) and
> [mutable-globals-guide](../guides/mutable-globals-guide.md).
>
> **Sibling plan:** [dynvar-adoption-spices-plan](spices/dynvar-adoption-spices-plan.md)
> (revised 2026-10-17). It no longer waits on anything here.
>
> **Reports that bear on this plan:**
> - [dynvar-binding-not-swapped-on-fiber-switch](../archive/dynvar-binding-not-swapped-on-fiber-switch.md) (resolved 2026-10-18):
>   a `binding` is per thread, not per fiber. ADO-1 makes `*log-level*`
>   bindings mean something, so it makes this bug observable in async code.
> - [defdynamic-generic-adt-root-emitted-before-its-type](../archive/defdynamic-generic-adt-root-emitted-before-its-type.md)
>   (resolved 2026-10-18): a `(Option T)` dynvar did not compile. It does now,
>   and a bare `(None)` root needs no ascription.
> - [defdynamic-refuses-payload-free-defdata](../archive/defdynamic-refuses-payload-free-defdata.md)
>   (resolved 2026-10-21): split out of the report above. A payload-free
>   `defdata` is admitted now, so `*log-level*` can be an enum ADT.
> - [time-capability-now-is-32-bit](../archive/time-capability-now-is-32-bit.md)
>   (resolved 2026-10-18): filed while checking the old mock-time target. The
>   Time slots take `self` now, so `Mock-Time` has no singleton.

---

## What changed since the 2026-06-24 revision

Every row was measured on 2026-10-17.

| The old text said | The tree says |
| --- | --- |
| Prerequisite: DV0-DV4 behind `-Xdynamic-vars`. | Always on. The flag is a no-op. |
| Open question: with an explicit `defn main`, the root initializer is not emitted. | Fixed. The initializer goes through `def_init_body` (`src/compiler/emit_module.c:20755-20768`), and a `defdynamic` with an explicit `main` reads its root correctly. |
| ADO-0b: add `(defthreadlocal *x* :T root)`, lowered to `_Thread_local`. | `(def ^mut ^thread-local x : T root)` exists. It lowers to a `pthread_key_t`, deliberately not `_Thread_local`, because c2mir treats `_Thread_local` as an ordinary global under `tur jit` (`src/compiler/expr.h:274-288`). The proposed lowering would have been wrong. |
| ADO-0: a C API (`TurDynVar`, `tur_dyn_get`, `extern-dynvar`) so `src/` C can read Turmeric dynvars. | Still not built (`_dynvar_key_<name>` is module-static, `emit_module.c:20330`). Nothing below needs it, so it is parked. |
| **A.** Route the compiler's `fprintf(stdout/stderr)` through `*current-out*` / `*current-err*`. | A category error. The compiler is C, and a `defdynamic` exists only in programs `tur` emits, so `tur`'s own output has no Turmeric dynvar to read. Capturing the compiler's output is a C refactor (a sink passed down, or a C `_Thread_local`), not dynvar adoption. |
| **B.** `(defdynamic *diag-mode* :int 0)` for human / JSON / LSP diagnostics. | The same category error. Diagnostic mode is `diag.c` state inside the compiler (`diag_set_json_output`, `diag_lsp_begin/flush/end`, `src/compiler/diag.h:877-914`). The `:int` 0/1/2 encoding was also the stand-in CLAUDE.md forbids. |
| **C.** Replace the HAMT comparator hooks' hand-rolled save/restore with dynvars. | The hooks are already C `_Thread_local`s (`src/runtime/hamt.c:284-296`), saved and restored inside each C entry point (`:321-330`, uses from `:1244`). No Turmeric code binds them, and they are read on every key comparison. A dynvar would add a `pthread_getspecific` to that hot path and buy nothing. |
| **D.** A thread-local symbol table for test isolation. | A correctness regression. `Sym` identity is process-wide by contract: one mutex-guarded table whose pointers live forever (`src/runtime/symbols.c:1-7`), and `sym=?` is pointer equality (`stdlib/sym.tur:57-60`). Per-thread tables would make the same symbol compare unequal across threads. |
| **E.** A thread-local mock-time singleton. | It treats a symptom. `Mock-Time` is already a capability value, but its `now`/`sleep` slots take no context, so they read a process-wide singleton (`stdlib/time.tur:66-75`). A thread-local copy would hide a `Mock-Time` from any worker thread the capability is handed to. The fix is a `self` argument on the slots, which [time-capability-now-is-32-bit](../archive/time-capability-now-is-32-bit.md) lists alongside the 32-bit `now` it found. |
| (not known) | `stdlib/dynvar.tur`'s four vars have no readers, and its docs describe functions that do not exist (below). This is the new scope. |

---

## The new scope: the standard dynvars

`stdlib/dynvar.tur` declares four vars and documents what each does. A
`git grep` over `stdlib/` finds no reader for any of them:

| Var | Documented behavior | What exists |
| --- | --- | --- |
| `*log-level*` `:int 1` | `(binding [*log-level* 0] (log-debug ...))` shows debug output "only in verbose mode". | `stdlib/log.tur` has no `log-debug`. Its `log-*-direct` functions and the `Real-` / `Simple-` / `Null-Logger` capabilities print every level and never read the var. |
| `*current-module*` `:cstr ""` | Prefixes log lines (`"auth: token validated"`). | No logger reads it. |
| `*random-seed*` `:int 0` | "Seed for the per-thread RNG": `(binding [*random-seed* 42] (random-int 100))` is reproducible. | There is no `random-int`; the real functions are `rand-int` and friends. `stdlib/random.tur` seeds libc `rand()`, which is process-global and not per thread, with `time(NULL)` behind six `static int seeded` flags (`random.tur:61-280`). `Seeded-Random [seed]` already gives explicit reproducibility. |
| `*locale*` `:cstr "en-US"` | `(binding [*locale* "fr-FR"] (format-currency 1234))` | No `format-currency`, and no locale-aware formatting at all. |

The file also still says `Requires: -Xdynamic-vars flag.`

### ADO-1. `stdlib/log.tur` honors `*log-level*` and `*current-module*` -- small, do first

This is the case a dynvar exists for. A scoped verbosity override that
restores on exit, composes when nested, and follows a `spawn-conveying` child
is exactly what callers want from a log level.

- `log-*-direct` drops a message whose level is below `*log-level*`, using the
  var's documented convention (0 debug, 1 info, 2 warn, 3 error). It prefixes
  `*current-module*: ` when that is non-empty.
- **Decision:** should the Logger *capabilities* filter too? A capability is
  explicit configuration, so a case can be made either way. Recommend: yes for
  `Real-Logger` and `Simple-Logger`, so one `binding` quiets a whole subsystem;
  `Null-Logger` is unaffected. Write the answer into the module docstring.
- **Type:** `*log-level*` is an `:int` with a 0-3 convention, which is the
  stand-in CLAUDE.md forbids. Make it the enum ADT,
  `(defdata LogLevel (Debug) (Info) (Warn) (Error))`, with a rank helper for
  the comparison. Until 2026-10-21 `TUR-E0603` refused it as move-only even
  with no payloads, and this bullet recommended a `(defopaque LogLevel :int)`
  instead; a payload-free `defdata` is admitted now
  ([defdynamic-refuses-payload-free-defdata](../archive/defdynamic-refuses-payload-free-defdata.md),
  pinned by the `defdynamic-payload-free-defdata` fixture, which is this
  shape).
- **Fibers:** once a binding changes behavior, the fiber report becomes
  observable. A `binding [*log-level* 0]` in one async handler leaks into the
  others on that thread. Say so in `log.tur`'s docstring until the report is
  fixed.
- **Tests:** a fixture covering the root level, a nested override, restore
  after the binding exits, the module prefix, and a `spawn-conveying` child
  that sees the parent's level.

### ADO-2. `*random-seed*` -- retire it, and fix the docs

Honoring a *scoped* seed means each `binding` needs its own RNG state, reset
whenever a new binding with the same seed begins, and kept separate from the
process-wide `rand()` that the six `seeded` flags share. That is a per-binding
generator in disguise. `Seeded-Random [seed]` already provides it as an
explicit capability.

Recommend: deprecate `*random-seed*` and point its docstring at
`Seeded-Random`. Correct "per-thread RNG" (libc `rand()` is process-global)
and the example's nonexistent `random-int`.

### ADO-3. `*locale*` -- retire it until something formats

Nothing in stdlib is locale-aware, so the var has nothing to configure.
Deprecate it, or keep it and replace the `format-currency` example with a
sentence saying no stdlib function reads it yet. Recommend deprecating: a var
that silently does nothing is worse than no var.

### ADO-4. `stdlib/dynvar.tur` docs

Drop `Requires: -Xdynamic-vars flag.`, make every example call a function
that exists, and run `tur run docs` so `stdlib/docstrings.tur` picks it up.
Ride this along with ADO-1, -2 and -3 rather than as its own change.

---

## Phases

| Phase | What | Size | Gate |
| --- | --- | --- | --- |
| ADO-1 | `log.tur` honors `*log-level*` / `*current-module*`; `LogLevel` type | small | none |
| ADO-2 | deprecate `*random-seed*` in favor of `Seeded-Random` | tiny | none |
| ADO-3 | deprecate `*locale*` | tiny | none |
| ADO-4 | `dynvar.tur` docs and docstring regen | tiny | rides with ADO-1..3 |
| parked | the old ADO-0 C-side API | medium | a consumer that needs C to read a Turmeric dynvar; none exists |

Each phase is one PR and stands alone.

---

## Appendix: stdlib sweep (2026-10-17, `main` 3d9e02a2a)

**Readers of the standard dynvars:** none, in any stdlib module other than
`dynvar.tur` itself.

**Hand-rolled thread-locals:** one site.
- `stdlib/r7rs/prelude.tur:2128-2135` holds the call/cc stack base per thread.
  It is already correct under `tur jit`: the `_Thread_local` is behind
  `__GNUC__`/`__clang__`, and otherwise the host keeps the value
  (`src/runtime/tur_tls.c`). Leave alone.

**Function-local C statics in stdlib inline-C:**

| Site | What | Class |
| --- | --- | --- |
| `random.tur:61-280` (six) | `static int seeded` around libc `srand` / `rand` | ADO-2 |
| ~~`time.tur:67`~~ | ~~`__tur_mock_time_singleton`~~ | Done 2026-10-18: the slots take a `self` (the time report) |
| `test/capability.tur:172` | `__tur_test_time_value` | the same shape as the mock clock; fix it alongside |
| `term.tur:125, 144` | `have_saved` (saved termios) | leave alone -- the terminal is process-wide state |
| `httpd.tur:1851` | `warned` (warn-once flag) | leave alone |
| `httpd.tur:5802` | request-ID mixing counter, used only without `/dev/urandom` | leave alone -- the ID needs uniqueness, not secrecy |
| `image_hooks.tur:40`, `r7rs/prelude.tur:3040` | registration count, one-time init | leave alone |

None of these wants a dynvar. One wants deprecation, and two want a capability
that carries its own state.
