# Error Handling -- Deferred Features Plan

> **Status:** Complete -- all phases shipped (R1, C2, R6a, R6b, R6d earlier; R2 + R6c landed 2026-06-02)
> **Last Updated:** 2026-06-02
> **Type:** Language / Compiler / stdlib / Docs

---

## R2 + R6c -- shipped (2026-06-02)

`catch-unwind` / `catch-panic-of` now work end-to-end on the **compiled path**,
and the R6c effect-handler/continuation panic semantics are validated and made
normative in the guide. What landed:

- **Catchable plain panic.** The emitted `tur_panic` (`emit_module.c`) gained a
  branch: when a catch boundary is active (`global_panic_jmpbuf_valid`), it boxes
  the message as a `:cstr` payload, fires the panicking frame's defer chain, and
  `longjmp`s to the boundary. With no boundary it aborts exactly as before, so
  every existing panic/defer fixture is unchanged.
- **Result-box helpers.** New `tur_catch_unwind_box(int64 thunk)` /
  `tur_catch_panic_of_box(int type, int64 thunk)` invoke the thunk via a new
  `TUR_APPLY0` (nullary fat-closure apply), save/restore the single global
  `jmp_buf` for nesting, and return a `tur_result_box_t` (`tur_ok`/`tur_err`).
  The caught payload becomes the err value (opaque `Panic` handle). The legacy
  `tur_catch_unwind(tur_thunk_fn,...)` used by `try/catch` is untouched.
- **Partial unwind for free.** The catch thunk is a separate C function, so
  `tur_frame_fire_chain(global_panic_frame)` fires only the thunk's own defers
  and stops at the call boundary -- the plan's Risk #7 dissolves without a
  dedicated boundary-frame stack.
- **Codegen** (`emit_expr.c`): `EX_CATCH_UNWIND` / `EX_CATCH_PANIC_OF` now emit
  `int64_t v = tur_catch_unwind_box(...)` and return the box.
- **Elaboration** (`elab_concurrent.c`): the value is typed `:int` (the result
  box carrier, so stdlib `ok?`/`err?` and `?` consume it directly); a bare
  non-capturing thunk is auto-shimmed to a fat closure via `EX_FN_TO_FAT`;
  `catch-panic-of`'s type arg now accepts both `cstr` and `:cstr` spellings.
- **stdlib/panic.tur**: `(defopaque Panic :ptr<void>)` plus `result-panic`,
  `panic-message`, `panic-file`, `panic-line`, `panic-type` (OQ#2 opaque wrapper).
- **Fixtures**: `panic-catch-unwind-basic`, `-caught`, `-defer`, `-nested`,
  `-double`, `panic-catch-panic-of`, `panic-in-handler`, `panic-reset-clears`
  (all behavioral / stdout-based; the catching ones carry `requires.no-leak-check`
  since the recovered `Panic` handle is owned by the result in v1).
- **Guide**: new `### Catching a panic at a boundary: catch-unwind` section; the
  effect-handler/continuation rules promoted out of *Deferred* into the body; the
  Deferred table emptied.
- All codegen snapshots regenerated for the preamble change.

**R6c note:** the planned `tur_reset_enter`/`tur_reset_exit` save/restore of
`tur_panic_in_progress` proved **unnecessary** -- the catch boundary itself
clears the flag on recovery, so a panic propagating through `reset`/`shift` and
caught by an outer `catch-unwind` leaves clean state (verified by
`panic-reset-clears` and manual reset/shift tests).

**Interpreter parity (follow-up fix).** The tree-walking interpreter
(`tur --interpret` / `tur eval --file`) now composes `catch-unwind` with
`ok?`/`err?` too:

- The `--interpret`/eval startup path never preloaded `result.tur`, so the
  `ok?`/`err?`/`some?`/`none?` natives had no declared type and the elaborator
  defaulted their result to `:int` -- which then failed the strict
  "if condition must be bool" check even for a plain `(ok 5)`. Fixed by
  injecting `:bool` typed stubs alongside the other native signature stubs in
  `src/main.c` (the natives still provide the runtime behaviour).
- The interpreter's `EX_CATCH_UNWIND` returned a `make_struct_val("ok"/"panic")`
  struct that the box-reading `ok?`/`err?` natives could not inspect. It now
  returns the native 3-int Result-box layout `{ is_ok, ok_val, err_val }` (ok =
  thunk value, err = the caught message), so it composes like any other Result.
- Regression guard: `eval-catch-unwind` in `tests/run-flags.sh` (the genuine
  interpreter harness) exercises ok + caught-panic through `tur eval --file`.

**`*-must` reroute (OQ#2 bonus -- shipped).** The interpreter natives
`native_result_must` / `native_result_must_msg` / `native_option_must` /
`native_option_expect` in `src/main.c` previously called `_exit(1)` directly,
bypassing both `catch-unwind` and the defer chain. They now raise a *catchable*
panic via a shared `turi_runtime_panic(env, msg)` helper (extracted in
`src/turi/eval.c` from the `EX_PANIC` case and exposed in `eval.h`), so:

- a `*-must` failure inside `catch-unwind` is recovered as an `(err ...)` result;
- an uncaught `*-must` fires defers and exits nonzero with the standard
  `panic at` / `panic: <msg>` message + double-panic guard -- making the guide's
  "standard panic message format and double-panic guard" claim true.

(`result-must`/`option-must` remain interpreter-only -- they are not defined for
the compiled path -- so this surface is interpreter-side, as the plan noted.)
Regression guards: `eval-must-catchable` / `eval-must-uncaught` in
`tests/run-flags.sh`.

---

---

## Implementation status (2026-06-02)

Five of seven items have shipped on branch `claude/practical-volta-3dl2D`
(suite green at 1257 fixtures). The plan's original "Current state" column was
significantly stale -- several flags were already partly wired, and
`catch-unwind` is *not* merely "partial" (see the R2 findings below).

| # | Feature | Status | Notes |
|---|---|---|---|
| 1 | `?` query operator | **Shipped (R1)** | `?`-specific TUR-E0001 for literal non-Results; fixtures for typed err-carry, sweet-exp `?(...)`, bad-type, top-level; guide section added. |
| 2 | `catch-unwind` | **Remaining (R2)** | Larger than "partial" -- two disconnected panic impls must be unified. See "R2 findings" below. |
| 3 | `--no-contracts` | **Shipped (C2)** | Strips contract macros *and* refinement contracts at elaboration; folds `contract-enabled?`; `TUR_CONTRACTS_ENABLED` preamble define; `release-stripped` recipe. |
| 4 | `--warn-unused-result` | **Shipped (R6a)** | Flag was already implemented; added 5 fixtures (incl. OQ#3 `?` carve-out) + guide. |
| 5 | `--lint-panic` | **Shipped (R6b)** | Rewrote the ad-hoc lint into TUR-W0038 with `;; #lint-panic-allow` file/per-call allow-list and `*-unwrap` soft-deprecation (OQ#1); 6 fixtures + guide. |
| 6 | Panic in effect handlers / continuations | **Remaining (R6c)** | Depends on R2. |
| 7 | Panic in async tasks | **Shipped (R6d)** | Promoted to a body section with Today/v2 callouts + cps-transform cross-link. |

**Deferred-table & HTML cleanup:** the guide's *Deferred* section now holds
only the `catch-unwind` row (R2) and the effect-handler/continuation subsection
(R6c). The `See Also` section was added. The HTML companion
(`error-handling-guide.html`) regen was intentionally deferred to a single
final sweep once R2/R6c land (the `.md` is the source of truth meanwhile).

### R2 findings (why catch-unwind is more than "partial")

Auditing the compiled path turned up that **panic has two disconnected
implementations**:

- `src/runtime/runtime.c` has `tur_catch_unwind` + a single global
  `setjmp` boundary (`global_panic_jmpbuf`) and a `tur_panic_with` that
  `longjmp`s to it. Nested boundaries are explicitly unsupported.
- **Compiled programs do not use that path.** `emit_module.c:2400` emits a
  *separate* `tur_panic` into each program's C preamble that fires the defer
  chain (`tur_frame_fire_chain`) and then **always `abort()`s** -- it never
  consults the runtime's boundary jmpbuf. The plain `(panic "msg")` form
  lowers to this emitted `tur_panic`, so it is uncatchable in compiled code.

That is why the `panic-catch-unwind` fixture is just `(defn main [] :int 0)`:
the codegen for `EX_CATCH_UNWIND` (`emit_expr.c:1203`) is effectively dead --
it declares a `tur_result`, calls the runtime `tur_catch_unwind` with a
mismatched thunk ABI (`(fn [] :int)` fat closures are *not*
`void(*)(void*, tur_result*)`), and "extracts" the value only via C comments.

### Concrete R2 design for a dedicated session

1. **Unify the panic path in the emitted preamble** (`emit_module.c`):
   - Emit a boundary *stack*: `static jmp_buf tur_catch_stack[N]; static int
     tur_catch_top; static tur_panic_payload *tur_caught_payload;` plus the
     `tur_panic_payload` struct + accessors (or link them from `runtime.c`,
     but keep one source of truth).
   - Reroute the emitted `tur_panic` **and** `tur_panic_with`: when
     `tur_catch_top > 0`, fire defers up to the boundary's frame, box the
     message/payload, set `tur_caught_payload`, and `longjmp` to
     `tur_catch_stack[tur_catch_top-1]`. When `tur_catch_top == 0`, behave
     exactly as today (fire chain + abort) so every existing panic/defer test
     is unaffected.
   - Plain string panic boxes `{type_tag = TY_CSTR, value = strdup(msg)}` so
     `catch-unwind` can recover the message.

2. **Inline `setjmp` codegen** for `EX_CATCH_UNWIND` (`emit_expr.c`) -- the
   `setjmp` MUST live in the generated frame, not a runtime helper:
   ```c
   int64_t __cu_ok; tur_panic_payload *__cu_p = NULL; int __cu_caught = 0;
   jmp_buf *__jb = &tur_catch_stack[tur_catch_top++];
   if (setjmp(*__jb) == 0) { __cu_ok = TUR_APPLY0(<thunk-closure>); tur_catch_top--; }
   else { __cu_p = tur_caught_payload; tur_caught_payload = NULL; __cu_caught = 1; }
   ```
   Then build a **result-shaped `ptr<void>`** `{is_ok, ok_val, err_val}` (the
   same layout `__tur-q-is-err?` reads) so the result composes with
   `err?`/`ok-val`/`?`: ok => `{1, __cu_ok, 0}`; err =>
   `{0, 0, (int64_t)__cu_p}` where the err slot is the opaque `Panic` handle
   (OQ#2). Confirm `TUR_APPLY0` exists for nullary fat closures; add it if not.

3. **`catch-panic-of`**: same boundary, but after catching, if
   `tur_panic_payload_type(p) != expected`, re-raise via `tur_panic_with`
   (now routing to the *next* outer boundary because this one was popped).

4. **stdlib `stdlib/panic.tur`**: `(defopaque Panic :ptr<void>)` and the
   `panic-payload-message/-file/-line` accessors (`:Panic -> ...`) wrapping the
   runtime accessors. `catch-unwind`/`catch-panic-of` stay special forms.

5. **`*-must` reroute (OQ#2 bonus)**: `native_result_must` /
   `native_option_must` / `*-expect` in `src/main.c:4989+` currently
   `_exit(1)`. Route them through `tur_panic_with` so they are catchable and
   the guide's "standard panic message format + double-panic guard" claim
   becomes true. (Interpreter-side; the compiled `*-must` surface is separate.)

6. **Regenerate all codegen snapshots** (preamble change) per the CLAUDE.md
   Fixture Snapshots rule, and add fixtures: `panic-catch-unwind-basic`,
   `-caught`, `-nested`, `-defer`, `-double`, `panic-catch-panic-of`, plus the
   interpreter equivalents under `tests/run-turi.sh` (the interpreter already
   has `env->catch_jmp` in `src/turi/env.h:154`).

7. **Risk:** the partial-defer-unwind-to-a-boundary semantics (plan rules 3-4
   of R6c) are not in the current frame-chain model, which only supports
   full-chain unwind to abort. Validate `panic-defer`, `panic-double-panic`,
   `panic-trace`, `continuation-*`, `defer-*`, `effect-abort-panic`, and
   `taskgroup-panic-propagate` after every step.

### R6c note

R6c (panic in effect handlers / continuations) is unblocked only once R2 lands.
Its runtime change -- `tur_reset_enter`/`tur_reset_exit` save/restore of
`tur_panic_in_progress` (rule 2) -- should be implemented against the unified
boundary stack from R2 step 1, not the dead `runtime.c` path.

---

## Overview

The Error Handling Guide ([docs/guides/error-handling-guide.md](../guides/error-handling-guide.md))
ends with a "Deferred" table listing five named features and two behavioural
sections (effect handlers, async tasks) that are flagged as planned-but-not-yet
implemented. This plan finishes those features, sequences them by dependency,
and folds the guide updates into the same phase that ships each piece -- so
the guide never drifts ahead of, or behind, the runtime.

The deferred surface is (original assessment -- see
"Implementation status (2026-06-02)" below for the corrected, current state;
several "Not started" / "Partial" entries here proved stale):

| # | Feature | Guide phase tag | Current state |
|---|---|---|---|
| 1 | `?` query operator | R1 | **Mostly implemented** in compiler (`elab_question` in `src/compiler/elab_forms.c:2292`) + stdlib helpers (`__tur-q-is-err?`, `__tur-q-ok-val` in `stdlib/result.tur:194`). Passing fixtures: `tests/fixtures/result-question-op{,-chain}`. **Undocumented** in the guide. |
| 2 | `catch-unwind` | R2 | **Partial.** Runtime has `tur_panic_payload`, `tur_panic_with`, `EX_CATCH_UNWIND` AST node, interpreter `catch_jmp` setjmp boundary in `src/turi/env.h:154`. Stub fixture `tests/fixtures/panic-catch-unwind` only verifies it compiles. Missing stdlib surface, end-to-end test, and docs. |
| 3 | `--no-contracts` flag | C2 | Not started. `stdlib/contract.tur` already calls `contract-enabled?` which is hard-coded to `true`; the flag flips a compiler constant and a stdlib function. |
| 4 | `--warn-unused-result` | R6 | Not started. Requires `ignore!`-aware lint pass over `result`-typed call expressions in statement position. |
| 5 | `--lint-panic` | R6 | Not started. Requires a call-site walker plus a per-source-file allow-list. |
| 6 | Panic in effect handlers / continuations | R6 | Behavioural; depends on (2). Runtime already unwinds defer thunks; needs spec + tests for `reset` boundary clearing the panic state and for continuation-resumption semantics. |
| 7 | Panic in async tasks | R6 (v2) | Behavioural; depends on (2) and on fiber-based async (out of scope of this plan; tracked in `docs/upcoming/cps-transform-plan.md`). This plan only specifies the boundary behaviour and writes synchronous-mode docs. |

---

## Goals / Non-Goals

### Goals

- Land `?` and `catch-unwind` as user-visible, documented features with full
  test coverage and stdlib surface.
- Ship `--no-contracts` as a real release-build optimisation: the contract
  macros expand to nothing when the flag is on.
- Ship `--warn-unused-result` and `--lint-panic` as opt-in `diag_warn` passes
  with single-source-file allow-list comments and CI-friendly diagnostics.
- Specify and test panic behaviour inside effect handlers, continuations, and
  defer chains.
- Rewrite the **Deferred** section of `error-handling-guide.md` after each
  phase so the guide always reflects what actually ships.

### Non-Goals

- Typed exceptions / `try` / `catch` syntax (covered in
  `docs/design/error-handling-rationale.md` §"Typed Exceptions (v2)" -- separate
  v2 roadmap item).
- Fiber-based async runtime (tracked in
  `docs/upcoming/cps-transform-plan.md`). This plan only writes the eventual
  panic-at-task-boundary semantics into the guide as a forward-looking note.
- Replacing `panic`/`abort` with stack traces (out of scope; existing
  `tur_panic` message format is preserved).

---

## Phase R1 -- Document the `?` operator (already implemented)

### Audit before writing

- Confirm `(? expr)` lowers via `__tur-q-is-err?` / `__tur-q-ok-val` at
  `src/compiler/elab_forms.c:2292`.
- Confirm `e->fn_body_depth == 0` guard rejects `?` at the top level.
- Confirm `result-question-op` and `result-question-op-chain` fixtures pass
  under `bash tests/run.sh`.

### Stdlib

- No new functions. The existing helpers `__tur-q-is-err?` and `__tur-q-ok-val`
  are already correctly marked `#{}` (safe).

### Compiler

- Add a sweet-exp form for `?` so it composes with neoteric
  (`?(foo(x))` and `? foo(x)` already parse via the existing reader_macros
  path; add a fixture pinning both spellings).
- Improve the diagnostic when `?` is used on a non-result type. Today it
  silently lowers; we want `TUR-E0xxx: ? operator requires a Result value,
  got <type>` from elaboration after the helper resolves.

### Tests

- `tests/fixtures/result-question-op-typed/` -- typed `Result<int, cstr>`
  through `?`, asserting the err branch carries through.
- `tests/fixtures/result-question-op-bad-type/` -- a `?` on an `:int` value
  to lock in the new diagnostic.
- `tests/fixtures/result-question-op-toplevel/` -- a `?` at the REPL / top
  level to lock in the `fn_body_depth` rejection.

### Guide updates

Add a new `## Query operator: ?` section between `## Result` and `## Option`:

- Syntax: `(? expr)` in both s-exp and sweet-exp; show neoteric `?(expr)`.
- Lowering: `(let [q expr] (if (err? q) (return q) (ok-val q)))`.
- Scope rule: only valid inside a function body whose return type is `result`.
- Worked example: a `parse-config` function that threads three `?` calls.
- Cross-link to `result-or-else` for the "transform-then-propagate" use case.

Move row 1 ("`?` operator") of the **Deferred** table into the new section.

---

## Phase R2 -- `catch-unwind`

### Audit before writing

- Existing scaffolding in `src/runtime/runtime.h:238` (`tur_panic_payload`),
  `src/turi/env.h:154` (`catch_jmp`), `src/compiler/expr.h:192`
  (`EX_CATCH_UNWIND`).
- The compiled path (codegen) currently emits a `Phase R2:
  catch-unwind/catch-panic-of` comment but no real lowering -- confirm in
  any `actual.c` (e.g. `tests/fixtures/panic-catch-unwind/actual.c`).
- Stub fixture `tests/fixtures/panic-catch-unwind/input.tur` only contains
  `(defn main [] :int 0)`; replace with real coverage.

### Runtime

- Finish `tur_panic_with` so it routes through the innermost active
  `catch_jmp` (interpreter) or unwind-table boundary (codegen).
- Implement `tur_panic_payload_downcast(p, target_type)` -- already declared
  in `runtime.h:298`; needs body + a type-tag check against
  `TypeKindInt`.
- Document the double-panic-during-catch case: if a `defer` thunk fires
  inside `catch-unwind` and itself panics, the **inner** panic replaces the
  caught one (matches Rust `catch_unwind` + `Drop` panic semantics).
- Ensure `catch_jmp` is saved/restored across nested `catch-unwind` calls
  (currently a single field; needs a linked-stack push/pop guarded by a
  local `jmp_buf`).

### Compiler

- Codegen for `EX_CATCH_UNWIND`: emit `setjmp` around the thunk call, push
  `catch_jmp` on entry, pop on exit, lower the panic-propagation path to
  populate a `tur_panic_result` struct.
- Reject `catch-unwind` outside a function body (same fn_body_depth guard
  as `?`).
- Refuse to compile `catch-unwind` on the WASM target without
  `-fexceptions` until a Wasm story exists; emit
  `TUR-E0xxx: catch-unwind requires -fexceptions on wasm32`.

### Stdlib

Add `stdlib/panic.tur`. Per resolved OQ#2, the payload is exposed through an
opaque `Panic` wrapper rather than a raw `:ptr<void>`, so callers cannot
accidentally dereference it; the accessors take `:Panic`:

```turmeric
;; Opaque wrapper around the runtime tur_panic_payload pointer (OQ#2).
(defopaque Panic :ptr<void>)
```

Per the resolved bonus finding, this phase also reroutes the existing
`*-must`/`*-expect` natives (`native_result_must` et al. at `src/main.c:4989`)
through `tur_panic_with` instead of their current bare `_exit(1)`, so they are
catchable by `catch-unwind` and the guide's "standard panic message format and
double-panic guard" claim becomes true. Add a fixture
`tests/fixtures/panic-catch-unwind-must/` asserting `(catch-unwind (fn []
(result-must (err-val ...))))` returns `err`, not a process exit.

```turmeric
;;; catch-unwind -- run thunk; return ok(thunk-result) or err(panic-payload).
;;;
;;; Parameters:
;;;   thunk -- (fn [] :int) -- the body to protect
;;;
;;; Returns:
;;;   Result; ok = thunk's return value, err = tur_panic_payload pointer
;;;
;;; Example:
;;;   (let [r (catch-unwind (fn [] (panic "boom")))]
;;;     (if (err? r) (println "caught") (println "ok")))
;;;
;;; Since: Phase R2
(defn catch-unwind [thunk : int] : Panic ...)  ;; err slot carries :Panic, not raw ptr

;;; catch-panic-of -- like catch-unwind, but only catches panics whose
;;; payload downcasts to the given type tag; re-raises other panic types.
;;;
;;; Parameters:
;;;   type-tag -- TypeKindInt of the expected payload
;;;   thunk    -- (fn [] :int)
;;;
;;; Since: Phase R2
(defn catch-panic-of [type-tag : int thunk : int] : Panic ...)

;;; panic-payload-message -- read the message string out of a caught payload.
;;;
;;; Since: Phase R2
(defn panic-payload-message [p : Panic] : cstr ...)

;;; panic-payload-file -- source file of the panic call site.
(defn panic-payload-file [p : Panic] : cstr ...)

;;; panic-payload-line -- source line of the panic call site.
(defn panic-payload-line [p : Panic] : int ...)
```

### Tests

- `tests/fixtures/panic-catch-unwind-basic/` -- `(catch-unwind (fn [] 42))`
  returns `ok(42)`.
- `tests/fixtures/panic-catch-unwind-caught/` -- catches an explicit
  `(panic "boom")` and asserts message round-trip.
- `tests/fixtures/panic-catch-unwind-defer/` -- defer thunks fire before
  the boundary returns.
- `tests/fixtures/panic-catch-unwind-nested/` -- inner boundary catches,
  outer boundary observes no panic.
- `tests/fixtures/panic-catch-unwind-double/` -- defer panics inside the
  boundary; outer caller sees the inner panic, not the original.
- `tests/fixtures/panic-catch-panic-of/` -- typed payload downcast paths.
- Interpreter equivalent under `tests/run-turi.sh` (the `catch_jmp` field
  in `src/turi/env.h` is interpreter-side).

### Guide updates

Add a new `## Catching a panic at a boundary: catch-unwind` section after
`## panic`. Cover:

- Use cases: FFI boundaries, test harness, supervisor loops -- **not** as a
  substitute for `result`.
- The double-panic-during-catch rule above.
- The interaction with `defer`: defer thunks run before `catch-unwind`
  returns its `err` result.
- Cross-reference `docs/design/error-handling-rationale.md` for the
  exception-vs-panic boundary.

Remove row 2 of the **Deferred** table.

---

## Phase C2 -- `--no-contracts` release flag

### Compiler

- Add `--no-contracts` to the global flags parser (sibling of `--no-data-literals`,
  `--no-auto-spice`).
- Define a single compile-time constant `TUR_CONTRACTS_ENABLED` written into
  the codegen preamble. When the flag is set, the constant is `0`.
- `tur-contract-check` and `tur-contract-check-inv` (`stdlib/contract.tur:45-49`)
  become no-ops when the constant is `0`. The cleanest implementation is to
  expand the macros (`assert!`, `require!`, `ensure!`, `invariant!`) to
  `(do)` rather than to the helper call, so the predicate expression itself
  is never evaluated.
- `contract-enabled?` returns `false` under the flag (today: hard-coded `true`).

### Tests

- `tests/fixtures/contracts-stripped/` -- compile a fixture with `--no-contracts`
  that contains `(assert! (= 1 2))` and verify it runs to completion.
- `tests/fixtures/contracts-stripped-side-effect/` -- the predicate's side
  effects are also stripped (pins the "expand to `(do)`" behaviour).
- Negative: without the flag the same fixture panics with `Assertion failed`.

### Guide updates

Update the "In v1, contracts are always enabled..." paragraph in
`## Contract macros` to describe `--no-contracts`. Remove row 3 of the
**Deferred** table.

### Justfile / build

- Add `release-stripped` recipe: `tur build --no-contracts -O2 ...`.
- Reference it from `docs/guides/compiler-flags-guide.md`.

---

## Phase R6a -- `--warn-unused-result`

### Compiler

- Add a new diagnostic class `TUR-W0xxx: result discarded without ignore!`.
- Walker hooks into the statement-position pass in `src/compiler/elab_forms.c`
  (the same pass that drops `do` block intermediate values).
- A call expression whose static return type is `result<_, _>` and whose
  enclosing parent is a `do`/`progn`/function body (not bound, not the tail
  expression) triggers the warning.
- `ignore!` (already in `stdlib/macros.tur`) silences the warning because it
  expands to `(do expr nil)` -- the wrapping `do` shifts the discarded value
  to a non-result type.
- Opt-in via `--warn-unused-result`; not on by default.

### Tests

- `tests/fixtures/lint-unused-result/` -- expects warning.
- `tests/fixtures/lint-unused-result-ignored/` -- silenced by `ignore!`.
- `tests/fixtures/lint-unused-result-bound/` -- a `let [_ ...]` binding
  does **not** warn (explicit-discard convention).
- `tests/fixtures/lint-unused-result-off/` -- flag not passed; no warning.
- `tests/fixtures/lint-unused-result-question/` -- a `?`-wrapped result call
  in statement position does **not** warn (resolved OQ#3). The `?` lowering
  binds its operand via `(let [__q_N expr] ...)` at `elab_forms.c:2307`, so the
  call is never in statement position; this fixture pins that no warning fires.
  No elaborator carve-out is needed -- the let-binding shields it.

### Guide updates

Add a `### Linting unused results` subsection under `## ignore!`. Remove
row 4 of the **Deferred** table.

---

## Phase R6b -- `--lint-panic`

### Compiler

- Add a new diagnostic class `TUR-W0xxx: panic call site outside allow-list`.
- A call to `panic`, `tur_panic`, `assert!`, `require!`, `ensure!`,
  `invariant!`, `result-unwrap`, or `option-unwrap` is a panic site.
- Allow-list source comment: a top-of-file `;; #lint-panic-allow` directive
  silences the warning for the whole file.
- Per-call escape: an immediately-preceding `;; #lint-panic-allow` comment
  silences a single call.
- Off by default; `--lint-panic` opts in.
- **`*-unwrap` soft-deprecation (resolved OQ#1):** `result-unwrap` and
  `option-unwrap` are flagged *specifically* as panic sites here -- they are
  already in the panic-site set above, but the diagnostic text calls out the
  `*-must` alternative (`TUR-W0xxx: panic call site outside allow-list; prefer
  result-must / option-must`). This is the soft-deprecation lever: no removal,
  no breaking change, just a lint nudge consistent with the guide's existing
  steer toward `*-must`.

### Tests

- `tests/fixtures/lint-panic-warn/` -- expects warning on `(panic ...)`.
- `tests/fixtures/lint-panic-file-allow/` -- file-level allow.
- `tests/fixtures/lint-panic-call-allow/` -- per-call allow.
- `tests/fixtures/lint-panic-asserts/` -- contract macros warn.
- `tests/fixtures/lint-panic-off/` -- flag absent, so the warning stays silent.
- `tests/fixtures/lint-panic-unwrap/` -- `(result-unwrap ...)` / `(option-unwrap
  ...)` warn with the `prefer *-must` text (resolved OQ#1 soft-deprecation).

### Guide updates

Add a `### Auditing panic call sites` subsection at the bottom of
`## panic`. Remove row 5 of the **Deferred** table.

---

## Phase R6c -- Panic in effect handlers / continuations

### Specification

Depends on R2. The semantics already described in the guide
(effect handlers, continuations, defer) become **normative** -- the spec
text moves out of the *Deferred* section into the body of the guide.

The four rules to lock in:

1. **Effect handlers**: a panic inside `(perform ...)` propagates through
   the handler chain and is only intercepted by a `catch-unwind`.
2. **Continuations**: a panic between `shift` and `reset` unwinds to the
   `reset` boundary, which **clears** the panic state. Resuming a captured
   continuation after a panic does not re-panic.
3. **Defer**: defer thunks fire in reverse registration order during
   unwinding. A defer thunk that itself panics triggers the double-panic
   guard (`abort()`).
4. **Cleanup ordering**: defer thunks fire *before* `catch-unwind` populates
   its `err` payload, so a defer thunk can observe panic state via
   `tur_panic_in_progress`.

### Compiler / runtime

- Runtime change: `tur_reset_enter` and `tur_reset_exit`
  (existing entry points in `src/runtime/effects.c`, walk the codebase to
  confirm exact names) must save/restore `tur_panic_in_progress` to
  implement rule 2.
- No new user surface beyond what R2 ships.

### Tests

- `tests/fixtures/panic-in-handler/` -- panic propagates past the handler.
- `tests/fixtures/panic-in-handler-caught/` -- `catch-unwind` wraps a
  handler and catches.
- `tests/fixtures/panic-reset-clears/` -- continuation after `reset`
  proceeds normally after a caught panic.
- `tests/fixtures/panic-defer-double/` -- double-panic guard fires.

### Guide updates

Promote the "Panic inside effect handlers and continuations (Phase R6)"
section out of *Deferred* into the body, immediately after `## panic`'s
existing `### defer during panic` subsection.

---

## Phase R6d -- Async panic semantics (forward-looking note)

Fiber-based async is out of scope (`docs/upcoming/cps-transform-plan.md`),
but the guide should document what *currently* happens and what the v2
target is, in one place.

### Spec text only -- no code

Rewrite the "Panic inside async tasks (Phase R6)" section as:

- A `## Panic inside async tasks` body section.
- A clear "**Today (synchronous async):**" callout that says: "`async`
  inlines the function call; a panic propagates through the caller. There
  is no task boundary."
- A "**v2 (fiber async):**" callout that quotes the four-point list from
  the guide today (panic → rejected future, cancellation precedence,
  uncaught panic in async main, WASM `unreachable`).
- A `> See [docs/upcoming/cps-transform-plan.md](cps-transform-plan.md)`
  cross-link.

Remove the Deferred entry; it lives in the spec now.

---

## Documentation phase -- guide rewrite checklist

The guide updates above land **in each phase**, but a final sweep is needed
to clean up the table and cross-links:

1. The **Deferred** section shrinks to zero rows. Replace the table with a
   short paragraph: "All previously-deferred features have shipped; see the
   table of contents above for the current surface."
2. The guide's opening "It describes what is implemented today" line stays
   accurate.
3. Add a `## See Also` section linking to:
   - `docs/design/error-handling-rationale.md`
   - `docs/guides/effects-system-guide.md`
   - `docs/guides/compiler-flags-guide.md` (for the three new flags)
   - `docs/upcoming/cps-transform-plan.md` (for the v2 async semantics)
4. Regenerate HTML: `python3 tools/genguides.py` (or whichever recipe
   `just docs` invokes for the guides tree -- confirm before running).
5. Refresh `docs/guides/error-handling-guide.html` and check it into the
   same commit as the `.md` changes (the existing guides all carry both
   variants).

---

## Sequencing & dependencies

```
R1 (?)  ──┐
          ├──► R6a (warn-unused-result; benefits from ? being documented)
          │
R2 (catch-unwind) ──► R6c (effect/continuation panic rules)
                  │
                  └─► R6d (async semantics note; spec only)

C2 (--no-contracts)  ── independent
R6b (--lint-panic)   ── independent (but ergonomic after C2 so release builds
                       are not flagged for `assert!` calls)
```

Recommended landing order: **R1 → C2 → R2 → R6c → R6a → R6b → R6d**.

R1 is mostly docs + small compiler tightening, so it ships fastest and
gives `?` to users immediately. C2 is the highest user-visible value for
release builds. R2 is the largest compiler change and gates the two R6
phases that depend on it.

---

## Risks

1. **`catch-unwind` in codegen vs interpreter divergence.** The interpreter
   uses `setjmp`/`longjmp` through `env->catch_jmp`; the C codegen needs a
   compatible scheme. Pinning both with the same fixture set under
   `tests/run.sh` and `tests/run-turi.sh` (per CLAUDE.md harness notes) is
   essential.
2. **WASM target.** `setjmp`/`longjmp` on WASM requires `-fexceptions` or
   asyncify; the guide currently does not call this out for the WASM
   build. The plan errs on the side of refusing `catch-unwind` on WASM
   without `-fexceptions` to keep the failure mode loud.
3. **Contract stripping subtlety.** Side effects inside contract predicates
   silently disappear under `--no-contracts`. This is the documented Rust /
   C `assert` convention but surprises Python/Clojure users. The new tests
   pin the behaviour and the guide calls it out explicitly.
4. **`--lint-panic` allow-list bikeshed.** The chosen `;; #lint-panic-allow`
   comment syntax is the simplest path; if the project grows other
   per-call lint silencers we may want a unified `;; #lint-allow: panic`
   form. Leave a TODO in the implementation; do not block the phase on it.

---

## Open Questions (resolved 2026-06-02)

1. **Should `result-unwrap` / `option-unwrap` be soft-deprecated in favor of
   `*-must`?** **Resolved: lint-only soft-deprecation.** `--lint-panic` (Phase
   R6b) flags `*-unwrap` call sites *specifically* -- on top of the general
   panic-site warning -- so migration is nudged without any breaking change or
   removal. The guide already steers callers toward `*-must`. See the R6b
   addendum below.
2. **`catch-unwind` payload representation -- raw pointer or opaque wrapper?**
   **Resolved: opaque `Panic` wrapper.** The stdlib surface introduces
   `(defopaque Panic :ptr<void>)`; `catch-unwind` returns `Result<int, Panic>`
   and the `panic-payload-*` accessors take a `:Panic` (not a raw
   `:ptr<void>`), preventing accidental dereferencing. This matches the Phase
   R2 stdlib sketch -- the accessor signatures below are updated from
   `:ptr<void>` to `:Panic` accordingly.
3. **`--warn-unused-result` interaction with the `?` lowering.** **Resolved:
   confirmed safe; pin with a fixture.** The lowering already binds the
   operand via `(let [__q_N expr] ...)` (`elab_forms.c:2307`, comment: "avoid
   multiple evaluation"), so a `?`-wrapped call is never in statement position
   and the warning cannot fire. No elaborator change is needed; R6a adds
   `tests/fixtures/lint-unused-result-question/` to lock this in.

### Bonus finding (resolved 2026-06-02): `*-must` bypass `tur_panic`

Audit turned up that `result-must` / `result-must-msg` / `option-must` /
`option-expect` are natives in `src/main.c` (e.g. `native_result_must` at
`src/main.c:4989`) that call `_exit(1)` **directly**, not via `tur_panic`.
This contradicts the guide's claim (error-handling-guide.md:86-87, 219-220)
that the `*-must` family yields "the standard panic message format and
double-panic guard" -- and, more importantly, it makes them **uncatchable by
`catch-unwind`**. **Resolved: fold the fix into Phase R2.** Reroute the four
`*-must`/`*-expect` natives through `tur_panic_with` so they are catchable and
the guide text becomes accurate. See the R2 addendum below.

---

## See Also

- [error-handling-guide.md](../guides/error-handling-guide.md) -- the guide
  this plan updates
- [error-handling-rationale.md](../design/error-handling-rationale.md) --
  exceptions vs. panic design rationale
- [compiler-flags-guide.md](../guides/compiler-flags-guide.md) -- where the
  three new flags will be documented
- [cps-transform-plan.md](cps-transform-plan.md) -- fiber-based async, on
  which the v2 panic-at-task-boundary semantics depend
- [effects-system-guide.md](../guides/effects-system-guide.md) -- effect
  handler semantics referenced by R6c
