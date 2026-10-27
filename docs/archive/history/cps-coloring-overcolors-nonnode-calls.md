# CPS coloring over-colors any function that calls a non-node callee (constructors, stdlib)

**Status: RESOLVED for the impactful case** (constructor calls). Kept for the
paper trail; see "Resolution" below. The remaining residuals (extern-c calls
still conservatively colored; the general non-tail cps->cps heap-join
limitation) are separate and lesser.

**Severity:** medium -- a **coverage** limitation for the CPS-IR-to-C backend,
not a miscompile. The over-coloring itself is safe (coloring more functions never
produces wrong code); the cost is that needlessly-colored functions cannot be
delegated as cps->direct calls, so a whole class of colored code falls back.
Gated behind `--enable=cps-backend`.

## Summary

The may-capture coloring in `src/passes/cps.c` marks a function colored if it
makes an **unresolved** call -- `cps_collect_calls` sets `has_indirect = true`
(cps.c:340) whenever an `EX_CALL`'s `fn_binding` is not one of the top-level
function nodes. That set does **not** include auto-generated constructors,
stdlib functions, or externs. So *any* function whose body calls one of those is
conservatively colored -- including a pure `make-struct` helper, whose body is a
call to the (non-node) constructor.

With the CPS backend this over-coloring is what blocks Option/Result/record
construction via helper functions across an effect boundary: a helper like `some`
or a user `mkwid` is colored, so a **non-tail** call to it (`(let [o (some v)]
...)`) needs a heap-reified join -- the pre-existing C1/C3 `needs_heap_join`
limitation -- which evicts the caller and forces the whole function back to the
fiber path. An *inline* `make-struct` works (it lowers to a constructor call the
CPS backend delegates directly), but the far more common helper-call form does
not.

## Minimal repro

```turmeric
;; pure make-struct helper -- no control op anywhere
(defstruct Wid [A] (v A))
(defn mkwid [x : int] : (Wid int) (make-struct Wid :v x))
```

`tur emit-c ... --enable=cps-backend --dump-cps` shows `cps-fn mkwid` (i.e.
`mkwid` is colored). Used non-tail across an effect it evicts the caller:

```turmeric
(defeffect E [] : int)
(defn f [] : int
  (let [v (perform (E))]
    (let [w (mkwid v)]      ; non-tail cps->cps to a (needlessly) colored fn
      (.v w))))
(defn run [] : int (handle (f) (E [] k) (resume k 42)))
```

`f` falls back (no `f__cps`), even though `mkwid` reaches no control op. The
same happens for stdlib `some` / `none` / `ok` / `err` and map/list builders,
because they too are `make-struct` helpers.

## Root cause

Two composing causes:

1. **Coloring over-approximation** (`src/passes/cps.c:333-341`,
   `cps_collect_calls`). An `EX_CALL` to a binding not found by `cps_find_node`
   (constructors, stdlib, externs) sets `has_indirect = true` -> the function is
   colored. This is correct-but-coarse: an unresolved call *might* reach a
   control op, but a constructor / extern-c / known-pure callee never does.
2. **Non-tail cps->cps has no heap join** (`needs_heap_join`,
   `src/compiler/emit_cps_ir.c`). Even once a function is (correctly or not)
   colored, calling it in non-tail position requires reifying the join
   continuation onto the heap chain, which the backend does not yet do -- so the
   caller is evicted. This is the documented C1/C3 subset limitation.

Either fix alone unblocks the common case; both are worth doing.

## Fix directions

1. **Coloring precision.** Do not set `has_indirect` for a call whose callee is
   resolved-but-non-node and known not to reach a control op -- constructors and
   `extern-c` functions in particular (they have no Turmeric body / no effects).
   A cheap version: recognize constructor call targets (and externs) and treat
   them as non-coloring edges rather than indirect. This un-colors the large
   population of pure `make-struct` helpers (Option/Result/map builders).
2. **Non-tail cps->cps heap join.** Implement the heap-reified join for a
   non-tail call to a colored, CPS-emitted callee (lift the continuation after
   the call onto the DK chain and resume it), removing the `needs_heap_join`
   eviction. This is the more general fix and also helps non-struct colored code.

Landing (1) is the smaller step and directly unblocks Option/Result/record
helpers; (2) is the broader capability.

## Resolution

Implemented fix direction (1) for **constructor calls** -- the impactful case
(`src/passes/cps.c`, `cps_collect_calls`): an `EX_CALL` whose `call_.ctor` is
non-NULL no longer sets `has_indirect`. A constructor stores its
(independently-checked) argument values into a fresh aggregate and invokes
nothing, so it cannot reach a control op; any control op in an argument is still
caught by the seed scan and the argument recursion. This un-colors every pure
`make-struct` builder (stdlib `some`/`none`/`ok`/`err`, map/list constructors,
and user helpers like `mkwid`), so a caller can delegate the call cps->direct.

A companion emit-side extension was needed for the destructuring side: the
lifted-body subset predicates (`shift_body_ok` / `perform_body_ok` /
`handle_case_ok` in `emit_cps_ir.c`) now allow a `CT_IF`, so an `Option`/`Result`
`match`-on-`.is-some`/`.is-ok` inside a perform/shift/handle continuation stays
on the CPS path (the emitter already lowered `CT_IF`).

Verified end to end: a colored function that performs an effect, wraps the
resumed value in an `Option` via stdlib `some`, then branches on `.is-some` and
reads `.value`, now CPS-emits and is direct-vs-CPS equal + LeakSanitizer-clean.
Same for `Result` (`ok` + `.is-ok`/`.ok-val`, which also exercises the delegated
`default-of`). Fixtures: `tests/fixtures/cps-backend-option-effect` (and the
earlier `cps-backend-struct-effect`). Full suite: 2003 passed, 0 failed.

**Residuals (separate, not blocking the struct-builder case):**

- **extern-c calls are still conservatively colored.** Skipping them was left out
  deliberately: an extern may invoke a Turmeric callback passed as an argument,
  which *could* reach a control op, so treating an extern call as non-coloring is
  not obviously sound the way a constructor call is. Revisit with callback-aware
  analysis if it proves to matter.
- **Non-tail cps->cps heap join** (fix direction 2) is not implemented. It no
  longer blocks struct builders (they are uncolored now, so their calls are
  cps->direct), but it remains the general limitation for non-tail calls to
  genuinely-colored callees. Tracked as the pre-existing `needs_heap_join`
  subset limitation (parent plan C1/C3).

## Related

- N3 of the non-scalar plan
  (`docs/upcoming/v1/cps-backend-non-scalar-values-plan.md`): the struct/ADT
  *local* slice landed there works for inline `make-struct`; this report is the
  "still open" helper-call case noted in that section.
- `needs_heap_join` in `src/compiler/emit_cps_ir.c` (the C1/C3 non-tail
  cps->cps limitation).
- `docs/reported/cps-coloring-ascription-hides-control-op.md` -- a different
  coloring-vs-backend precision gap (that one *under*-colors; this one
  *over*-colors).
