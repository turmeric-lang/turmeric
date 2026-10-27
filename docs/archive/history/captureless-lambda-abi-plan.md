---
title: Captureless-Lambda ABI Soundness Plan
category: Planning
description: Track fat-vs-bare closure ABI in the type system so a captureless `fn` cannot be passed where a fat closure is expected, eliminating the sentinel-capture workaround
---

# Captureless-Lambda ABI Soundness -- Plan

> **Status:** Implemented (cleanup complete) -- archived
> **Last Updated:** 2026-06-02
> **Type:** Compiler / Codegen / Type system
> **Related:**
> - [direct-anonymous-lambda-application-plan.md](direct-anonymous-lambda-application-plan.md) -- adjacent: callable expressions in head position
> - [docs/guides/parser-combinators-tutorial.md](../../guides/parser-combinators-tutorial.md) -- consumer that previously documented the workaround
> - `stdlib/parsec.tur` header comment -- the original "params must be bound to locals before inner lambda capture" warning

---

## Final Status (2026-06-02)

The type-system tracking described in **Phase CLA1** had already landed in the
compiler independently, under the internal label **"A#1"**:

- `Type.as.fn.arg_fat[]` and `Type.as.fn.result_fat` flags on `TY_FN`
  (`src/compiler/types.h`) carry the fat-vs-bare representation refinement
  this plan proposed as `is_fat`.
- A surface annotation `^fat` marks a fat-expecting sink in **both**
  parameter position (`[p ^fat f]`) and **return** position
  (`^fat :ptr<void>`). The parameter form auto-shims a bare `fn` argument at
  the call site (`EX_FN_TO_FAT`, `elab_call.c`); the return form boxes a
  captureless tail lambda into a one-cell fat closure (`emit_fns.c`).
- Runtime `__tur_fatshim<arity>` thunks (`emit_module.c`) implement the
  one-cell box, exactly the wrapper this plan sketched.

This differs from the plan's design in one respect: the fat-expecting sink is
named **explicitly** with `^fat` rather than inferred purely from flow. That is
a deliberate, honest annotation (not the "source-level lie" the sentinel trick
was), so the remaining work was the **cleanup** the plan called for:

- **CLA2** -- `stdlib/parsec.tur`: `pfail`/`item` rewritten to
  `^fat :ptr<void>` returns; the obsolete "params must be bound to locals"
  header rule replaced (params *are* captured by inner lambdas now; verified).
- **CLA3** -- `tests/fixtures/parsec-tutorial/input.tur`: sentinel wrappers
  removed from `number`, `term-tail-pair`, `term`, `expr-tail-pair`, `expr`;
  `bind-parser`'s continuation marked `^fat`; `expected.c` regenerated (also
  repairing pre-existing `TUR_CONTRACTS_ENABLED` snapshot drift); still
  prints `15`. The tutorial guide's sections 4/5/7/8 updated to teach `^fat`
  instead of the sentinel idiom.
- **CLA0 / validation** -- `tests/fixtures/captureless-autobox/` added: an
  end-user captureless `fn` flowed into `^fat` sinks in both positions,
  asserting correct fat-ABI dispatch (`42` / `100`).

Not done, with rationale: the plan's **negative demotion fixture** (reject
`is_fat = true -> is_fat = false`) was dropped -- the implementation has no
"bare-only" sink to demote *into*, so no such diagnostic exists to test.

---

## Overview

Today the compiler emits **two incompatible ABIs** for a `fn` expression:

- **Fat closure**: `int64_t[]` whose first slot is the thunk pointer and
  whose remaining slots are captured free variables. Called via `apply-fat`
  / `TUR_APPLY1`, which load `fat[0]` and pass `fat` as the env.
- **Bare function pointer**: a plain `int64_t (*)(int64_t)`. Called
  directly. Emitted when the lambda has no free variables, as an
  allocation-elimination optimization.

The source syntax is identical: `(fn [x] body)`. The result type at the
call site is identical: `:ptr<void>`. A consumer that receives the value
has no way to tell which ABI to use.

When a bare pointer is fed to `apply-fat`, the call dereferences it as
`int64_t*`, reads the **first instruction byte** as the thunk address, and
jumps to garbage. The program segfaults (or worse, silently corrupts).

`stdlib/parsec.tur` (line 19 header) and every parser-combinator consumer
work around this by wrapping every "captureless" inner lambda in a
sentinel capture:

```turmeric
(let [sentinel 0]
  (fn [inp]
    (let [_ sentinel]      ; <- force the body to read a captured var
      (mzero))))
```

This is a source-level lie that exists solely to coerce codegen into
picking the fat-closure path. It infects:

- `stdlib/parsec.tur` (header rule + two literal `sentinel 0` sites,
  `pfail` and `item`)
- `docs/guides/parser-combinators-tutorial.md` (sections 4, 6, 7 each
  show or call out the sentinel idiom, with explanatory paragraphs)
- The forthcoming `tests/fixtures/parsec-tutorial/input.tur` carries the
  same pattern through `number`, `term-tail-pair`, `expr-tail-pair`,
  `term`, and `expr`.

The fix is to make the ABI choice **visible in the type system**, so the
compiler refuses to silently coerce one representation into the other.
Once that's done, the sentinel workaround is no longer needed and every
guide that documents it can be simplified.

---

## Goals / Non-Goals

### Goals

- A captureless `fn` and a captureful `fn` are statically distinguishable
  by type. The compiler never silently passes one where the other is
  expected.
- Existing code that uses `apply-fat`, `TUR_APPLY1`, or any other fat-ABI
  caller continues to work without source changes -- the compiler arranges
  for a captureless lambda flowing into a fat-expecting slot to be boxed
  into a one-cell fat closure automatically.
- `stdlib/parsec.tur`'s `sentinel 0` workarounds and the header comment
  about "params must be bound to locals" become obsolete and are removed.
- `docs/guides/parser-combinators-tutorial.md` loses sections 4 / 6 / 7's
  sentinel commentary, section 8's discussion of the workaround, and any
  fixture mirroring it.
- Existing `parsec-*` fixtures continue to pass without regenerating
  `expected.c` -- behavioural equivalence is the bar, not byte-equality of
  generated C. Snapshot drift driven by the new boxing wrapper is expected
  and will be re-snapshotted in one commit.

### Non-Goals

- No change to the runtime ABI of fat closures themselves. The fat layout
  (`int64_t[0]` = thunk, `int64_t[1..]` = env) is stable; this plan only
  changes how the compiler decides to *produce* a fat vs bare value.
- No deprecation of the bare-function-pointer optimization. It stays, but
  becomes a representation choice the compiler proves safe rather than one
  the user has to second-guess.
- No new surface syntax. Users keep writing `(fn ...)` and `:ptr<void>`;
  the type system distinguishes the two internally.
- No retroactive rewrite of `docs/archive/history/` material. The archived
  `direct-anonymous-lambda-application-plan.md` covers a different gap
  (head-position calls) and stays as-is.

---

## Background

### How the two ABIs are emitted today

`src/compiler/emit_expr.c` handles `fn` lowering. The decision point is
`collect_free_vars` on the lambda body: a non-empty result triggers
fat-closure emission (heap-allocate `int64_t[]`, fill slots 1..N with
captured locals, return the buffer pointer as `:ptr<void>`). An empty
result triggers the bare-pointer path: the lambda becomes a top-level C
function and the `fn` expression evaluates to its address.

Both paths return values whose static type is `TY_PTR_VOID` (or `TY_FN`
when the binding is a `let`-aliased function). `apply-fat` and the
`TUR_APPLY1` macro both assume fat-closure layout unconditionally.

### Why the sentinel trick works

`(let [sentinel 0] (fn [inp] (let [_ sentinel] body)))` rewrites the
body to reference `sentinel`, so `collect_free_vars` reports `{sentinel}`
and the fat path fires. The captured value is dead (`_` is unused) but
codegen doesn't know that, so the allocation happens and the ABI matches
what the caller expects. This is exactly the "compiler does the wrong
thing silently" smell -- the user is hand-encoding the call convention.

### Why option 3 (type-track) over options 1 and 2

The companion question raised two cheaper alternatives:

1. **Always emit fat ABI** when the result flows into `:ptr<void>`.
   Correct, but gives up the optimization in the very contexts (returning
   a closure from a constructor) where it would otherwise be most useful.
2. **ABI-polymorphic `apply-fat`** with a tag bit.
   Requires a runtime check on every closure call. Cheap but pollutes the
   hot path of the parser-combinator and effect-handler stacks.
3. **Track "is-fat" in the type system**.
   Compile-time, zero runtime overhead, lets the optimization fire
   wherever it's provably safe and forces boxing wherever it isn't.

Option 3 is the principled fix. Options 1 and 2 are escape hatches we can
fall back on if 3 proves too disruptive in a phase below.

---

## Design

### Type-level distinction

Introduce a representation refinement on `TY_FN` / `TY_PTR_VOID` closure
types. Two viable encodings:

- **A new TypeKind**, e.g. `TY_FN_BARE` for top-level / captureless
  function values, with `TY_FN` reserved for fat closures.
- **A flag on `TY_FN`**, e.g. `ty.as.fn.is_fat : bool` plus a parallel
  flag on `TY_PTR_VOID` when the pointer carries a closure.

Prefer the flag form -- fewer downstream switch arms to update in
`emit_expr.c`, and the inference rules described below interact with
`TY_FN`'s existing arity/argtype tracking.

### Inference rules

- A `fn` expression starts with `is_fat = ?` (unknown) and is resolved
  during `collect_free_vars`:
  - non-empty free-var set -> `is_fat = true`
  - empty free-var set -> `is_fat = false` (bare-eligible)
- A function reference (`defn` head) is `is_fat = false`.
- A `let` / `def` binding inherits the initializer's `is_fat`.
- A call-site expecting `TY_PTR_VOID` with no flow constraint accepts
  either; the **caller's annotation** at the eventual fat-expecting sink
  (e.g. `apply-fat`'s declared parameter type, parser-combinator
  parameters) is what forces the decision.

### Coercion (auto-boxing)

When a value with `is_fat = false` flows into a slot with `is_fat = true`,
the compiler emits a one-cell fat box:

```c
int64_t *box = malloc(sizeof(int64_t));
box[0] = (int64_t)(intptr_t)bare_fn;
/* pass box where fat is expected */
```

The wrapper thunk slot is the bare function pointer itself; `apply-fat`
reads it and calls with `(box, arg)`. The bare function ignores the
`void*` env. This is exactly equivalent to what the user is doing manually
with `let [sentinel 0]` today, but at the right layer (after typing) and
without lying about the body.

The reverse coercion (`is_fat = true` into a bare-only slot) is a type
error -- there is no safe way to demote a fat closure.

### Sink annotations

Every consumer that currently assumes fat ABI (`apply-fat`, `apply-parser`,
`bind-parser`'s continuation slot, etc.) gets its parameter type narrowed
to `is_fat = true`. The compiler then forces auto-boxing at every site
that previously segfaulted.

### What disappears

After this lands:

- `stdlib/parsec.tur` header rule about "params must be bound to locals
  before inner lambda capture" -- delete.
- `stdlib/parsec.tur` `(let [sentinel 0] ...)` wrappers in `pfail` and
  `item` -- replace with the direct `(fn [inp] body)` form.
- `docs/guides/parser-combinators-tutorial.md` sections 4 / 6 / 7
  paragraphs that explain the workaround; section 8's discussion of the
  sentinel idiom.
- `tests/fixtures/parsec-tutorial/input.tur` sentinel wrappers in
  `number`, `term-tail-pair`, `expr-tail-pair`, `term`, `expr`.

---

## Phases

### Phase CLA0 -- Reproducer + failing fixture

- Add `tests/fixtures/captureless-fat-mismatch/` containing the minimal
  segfault reproducer: a captureless `(fn ...)` passed to `apply-fat`
  without the sentinel trick. Mark it `requires.no-leak-check`.
- The fixture is expected to **fail** before CLA1 and **pass** after.
  Until CLA1 lands, ship it with a `requires.skip-until-cla1` marker (or
  the existing skip mechanism) so the suite stays green.
- Document the segfault behaviour in the input fixture's leading comment
  so a reader hitting it understands what the test exercises.

### Phase CLA1 -- Type-system tracking

- Add the `is_fat` flag to `TY_FN` (and the `TY_PTR_VOID` closure carrier
  if separate) in `src/compiler/types.{c,h}`.
- Set the flag at `fn` elaboration in `src/compiler/emit_expr.c` based on
  `collect_free_vars`.
- Propagate through `let` / `def` / `defn` initializer flows.
- Narrow `apply-fat`, `apply-parser`, and the parser-combinator
  continuation slots to `is_fat = true`.
- Implement auto-boxing at `is_fat = false` -> `is_fat = true` coercion
  points.
- Reject `is_fat = true` -> `is_fat = false` flows with a clear diagnostic
  (`error: fat closure cannot be demoted to bare function pointer`).
- Drop the `requires.skip-until-cla1` marker from CLA0's fixture; it now
  must pass.

### Phase CLA2 -- stdlib cleanup

- Remove the header comment paragraph in `stdlib/parsec.tur` documenting
  the param-locals rule (lines around the header banner).
- Rewrite `pfail` and `item` to drop the sentinel wrapper:
  ```turmeric
  (defn pfail [] : ptr<void> (fn [inp] (mzero)))
  (defn item [] : ptr<void> (fn [inp] (item-impl inp)))
  ```
- Regenerate `tests/fixtures/parsec-*/expected.c` snapshots per the
  fixture-snapshot rule in `CLAUDE.md`. Verify `bash tests/run.sh`
  reports zero `FAIL` lines.

### Phase CLA3 -- Guide and tutorial cleanup

- `docs/guides/parser-combinators-tutorial.md`:
  - Section 4: remove the "free variable forces fat closure" paragraph.
  - Section 6: drop the sentinel-capture commentary on `bind-parser`'s
    continuation argument.
  - Section 7: simplify the AST-building snippets to not use
    `(let [sentinel 0] ...)` wrappers.
  - Section 8: replace the "captureless lambda pitfall" subsection with a
    short note that the compiler now boxes captureless lambdas
    automatically at fat-expecting sinks. Cross-link this plan's archived
    home (after CLA4) for historical context.
  - Sweet-exp variants must be re-paired -- run
    `tools/check-guide-pairs.py` (or `tur parse-check` once available) so
    AST equivalence still holds.
- `tests/fixtures/parsec-tutorial/input.tur`: remove sentinel wrappers
  from `number`, `term-tail-pair`, `expr-tail-pair`, `term`, `expr`.
  Regenerate `expected.c` and verify `expected.stdout` is still `15`.
- Run the full suite. Net diff should be smaller `.tur` snippets, smaller
  guide, and smaller fixture.

### Phase CLA4 -- Archive

- Move this plan to `docs/archive/history/captureless-lambda-abi-plan.md`
  with a final-status note (date, "implemented in CLA1, cleanup CLA2-3").
- Update any `[[captureless-lambda-abi-plan]]` references in memory or
  cross-links to point at the archived path.

---

## Test / Validation Strategy

- The CLA0 reproducer fixture is the contract: it must segfault before
  CLA1, pass after.
- All existing `parsec-*` fixtures (`parsec-basic`, `parsec-full`,
  `parsec-json-subset`, `parsec-many`, `parsec-or`, `parsec-sequence`,
  `parsec-tutorial`) must continue to pass after CLA2 and CLA3 without
  source changes beyond removing the sentinel wrappers.
- `bash tests/run.sh` must report zero `FAIL` lines at every phase
  boundary.
- ASan/LSan: the auto-boxing wrapper allocates one extra cell per
  captureless-into-fat coercion. Spot-check that this does not push the
  compiler/codegen path itself (which is leak-checked) into a regression,
  and that interpreter fixtures (`run-turi.sh`) continue to opt out of
  leak detection as before.
- Add at least one positive-case fixture under
  `tests/fixtures/captureless-autobox/` that exercises the boxing wrapper
  from end-user code (a captureless `fn` flowed directly into
  `apply-fat`), with an `expected.stdout` that proves correct dispatch.
- Add at least one negative-case fixture proving the compiler rejects
  `is_fat = true` -> `is_fat = false` demotion with the new diagnostic
  string.

---

## Open Questions

1. **Encoding: new TypeKind or flag?** The plan recommends a flag on
   `TY_FN` / `TY_PTR_VOID`, but a new `TY_FN_BARE` kind may surface bugs
   earlier by forcing every switch arm to handle it. Decide before CLA1.
2. **Sweet-exp snippet drift.** After CLA3 the tutorial's paired snippets
   shrink. Confirm `tur parse-check` (per `parse-check-subcommand-plan`)
   is available and wired into CI before that cleanup, or accept that
   pairings will be re-verified by hand for this round.
3. **Other consumers.** This plan names `stdlib/parsec.tur` explicitly.
   Audit during CLA1 whether any other stdlib module (`free.tur`, effect
   handlers, STM continuation slots) has its own captureless-vs-fat
   workaround in inline-C or comments. If so, fold their cleanup into
   CLA2 rather than spawning a separate plan.
4. **Diagnostic wording.** The `is_fat = true` -> `is_fat = false`
   demotion error needs a message a human can act on without reading this
   plan. Draft 2-3 candidates during CLA1 and pick whichever surfaces
   best in the existing fixture-failure output format.

---

## See Also

- [docs/guides/parser-combinators-tutorial.md](../../guides/parser-combinators-tutorial.md) -- consumer guide (sections 4, 5, 7, 8)
- [docs/guides/c-integration-guide.md](../../guides/c-integration-guide.md) -- inline-C ABI and fat-closure conventions
- [docs/guides/backtracking-guide.md](../../guides/backtracking-guide.md) -- list-monad primer, peer consumer of `apply-fat`-style callers
- [stdlib/parsec.tur](../../../stdlib/parsec.tur) -- `pfail`/`item` (now `^fat` returns)
- [tests/fixtures/parsec-tutorial/input.tur](../../../tests/fixtures/parsec-tutorial/input.tur) -- tutorial fixture (sentinel wrappers removed)
- [tests/fixtures/captureless-autobox/input.tur](../../../tests/fixtures/captureless-autobox/input.tur) -- positive end-to-end auto-box fixture
- [direct-anonymous-lambda-application-plan.md](direct-anonymous-lambda-application-plan.md) -- adjacent gap: callable expressions in head position
