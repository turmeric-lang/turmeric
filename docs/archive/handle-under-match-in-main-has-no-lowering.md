# A `handle` under a `match` in `main` leaves `main` off the CPS backend, and every function performing its effect has no lowering

**Resolved 2026-10-14: the report is archived.** The repro and its match-arm
twin both build and print. The fix has three parts. The report named the
first two; the third turned up while fixing them.

1. **Detection.** `fn_is_d2b_main` (`emit_cps_ir.c`) also asks
   `cps_expr_uses_control`, so a `handle` under a `match` makes `main` a
   candidate. The answer only widens, and a newly admitted `main` still
   faces the taint fixpoint.
2. **Admission, which was the general gap rather than a `main` one.** A
   `handle` continuation may now deliver a value to a join that *encloses*
   the handle (`handle_cont_ok`, through `joins_closed_rec_x`). Before,
   `term_core_ok` required the continuation to be self-contained, so a
   `handle` anywhere but the tail of a branch evicted its function. That
   happened in ordinary functions too: `(do (if c (println (handle ...))
   ...) 7)` failed the same way.

   Such a join is never a label. `join_escapes_lifted_rec` already counts a
   delivery from `handle.body` as lifted, so the join is an escaping join,
   and `emit_escaping_join` makes its frame `cur_k` while the branch emits.
   `emit_handle` splices the continuation frame onto that `cur_k`, so the
   delivery is `dk_run(__kont, v)` into the join frame. That is the path a
   `perform` continuation in a branch already took. Only a value delivery
   is admitted. A tail call or loop exit to an outer join, and an outer
   reference from a nested delim or handler case, stay refused.
3. **Translation.** In bind position, a literal-pattern `match` (the
   direct-dispatch path, `cps_ir.c`) now binds a non-atomic scrutinee
   *before* its join, as the ADT `match` arm and `if` already did. Before,
   the scrutinee was bound inside the join, so a control op there had its
   continuation reach back out.
   - For the repro, the result works through part 2 either way.
   - The hoist is required for a scrutinee that is a colored call:
     `(do (match (f) 42 ... _ ...) 7)` with `f` performing. That builds a
     heap join, and a heap join's body must be closed. It was the same hard
     error, with no `handle` anywhere.

Verified on the emitted C (`h6`'s `main_hk1` spliced onto `__ej0`). The
shapes below give the right values under gcc 13 and clang 18. On the
previous commit, every fixture program and probe here was the "no lowering"
error.

- an abortive handler
- captures on both sides of the join
- a float with a fractional part (7.1) crossing a bind-position join
- nested branches
- an ADT match arm
- a 20000-step self-recursion through the join

Pinned by two fixtures, both under `run.sh` and `run-turi.sh`:

- `tests/fixtures/handle-under-match-in-main`: the `main` shapes, with the
  let-bound control.
- `tests/fixtures/handle-in-nontail-branch`: the function shapes and the
  colored-scrutinee match.

Both carry `requires.leak-check` and run clean under LSan, so a lost
escaping-join or continuation frame would now fail that gate. No
`expected.c` moved; full `run.sh` was 3800 passed, 0 failed with the two
fixtures.

The original filing follows. Its "two halves" are parts 1 and 2 above. Its
suggested hoist, binding a `handle` in operand position to a `let`, turned
out unnecessary once part 2 landed. The scrutinee hoist in part 3 is a
narrower thing, and it is there for the colored-call case.

**Severity: medium.** A correct, ordinary program is a hard compile error:
"this effect operation has no lowering here". The error names the `perform`
in the callee, not the shape in `main` that causes it. Present on `main` at
`90416910` and on every build since.

**Discovered:** 2026-10-14, while resolving
[heap-sum-rebuild-in-effectful-generic-merge-temp-ice](../archive/heap-sum-rebuild-in-effectful-generic-merge-temp-ice.md).
Its first fixture draft matched directly on a `handle` result.

## Repro

```turmeric
(defeffect Tick [] int)
(defn f [] : int (+ 1 (perform (Tick))))
(defn main [] : int
  (match (handle (f) (Tick [] k) (resume k 41))
    42 (println "ok")
    _ (println "no"))
  0)
```

`tur run` (gcc or clang):

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower `perform`.
```

The same failure happens with the `handle` in a match **arm**
(`(match 1 1 (println (handle (f) ...)) _ ...)`).

These shapes work:

- The same `handle` let-bound first:
  `(let [r (handle (f) ...)] (match r ...))` prints `ok`.
- A `handle` as a call argument (`(println (handle ...))`).
- A `handle` under a field read (`(.x (handle ...))`).
- A `handle` as a `make-struct` field.

## Root cause

Two halves. Fixing only the first moves the failure but does not remove it.

1. **Detection.** `fn_is_d2b_main` (`src/compiler/emit_cps_ir.c`) decides
   whether `main` may enter the CPS backend. It asks
   `cps_expr_contains_shift`, `expr_has_handle` and `cps_expr_contains_callcc`.
   The first two have no arm for `EX_MATCH` (both end in
   `default: return false`), so a `handle` under a match is not seen. `main`
   is then `SIG-MAIN`, a permanent direct-path function. Because it handles
   `Tick`, the taint fixpoint permanently taints `Tick`, and `f` is evicted
   (`BODY-TAINT`). `TUR_TRACE_EVICT=1` shows both lines.

   `cps_expr_uses_control` (`src/passes/cps.c`) is the robust form of the
   same question: it has the `EX_MATCH` arm and falls back to
   `cps_visit_children`. Adding it to `fn_is_d2b_main` was tried. `main` then
   becomes a candidate, and the failure moves to half 2.
2. **Translation.** The candidate `main` fails `term_core_ok` as
   `BODY-STRUCT-CORE`. `TUR_TRACE_CORE=1` names the innermost refusal,
   `CT_HANDLE` (kind 9). Its continuation (`handle.body`) has to pass
   `reset_body_ok`, and it does not:
   - As a match scrutinee, the continuation is the `CT_MATCH` on the
     handled value.
   - In a match arm, it is a jump to the match's join (`CT_LETCONT`).

   Neither is in the subset `emit_handle` lowers.

## Fix directions

- Both halves are needed. Half 1 is the one-line `fn_is_d2b_main` widening
  above. It was tried only against these probes, not a full `run.sh`, and was
  backed out because by itself it fixes nothing.
- For half 2, the cheapest route is probably a translation-time hoist: bind
  a `handle` that sits in a non-tail operand position to a fresh `let`
  before the enclosing form. That is exactly the shape that already works.
  The alternative is to widen `reset_body_ok` to admit a `CT_MATCH` and a
  join delivery as a handle continuation. That is the general fix, and it
  is bigger.
- Pin it: the repro and its match-arm twin, both printing `ok`.

## Workaround

Let-bind the `handle` result before matching on it.
