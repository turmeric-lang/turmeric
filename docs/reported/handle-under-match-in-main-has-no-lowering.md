# A `handle` under a `match` in `main` leaves `main` off the CPS backend, and every function performing its effect has no lowering

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
