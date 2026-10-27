# CPS coloring does not see a control op nested inside a type ascription

**RESOLVED (2026-07-12):** Added an `EX_ASCRIBE` case that descends into
`e->as.ascribe_.inner` to the three coloring traversals in
`src/passes/cps.c` -- `cps_expr_contains_shift`, `cps_directly_uses_control`
(the seed scan), and `cps_collect_calls` (call-graph edges). A control op
reachable only through `(:: <control-op> T)` is now seen: the seed scan colors
the enclosing function, and a call inside an ascription now contributes a
call-graph edge. Verified with the report's repro: before, `use-ask`
(whose only `perform` sits inside `(:: (perform (Ask)) :int)`) dumped as
`use-ask uncolored`; after, `use-ask COLORED`. The CPS-IR-to-C backend has
since graduated to always-on (the `--enable=cps-backend` flag was removed in
#658), so the fixture guards the fix with `--dump-cps-coloring` (asserting
`use-ask: colored`) rather than the retired flag; the program prints `42` on
both the CPS and fiber paths. Regression fixture:
`tests/fixtures/cps-backend-ascribe-only-control/`.

Note: this fix covers the CPS *coloring* traversals only. The sibling
handler-reachability analysis (TUR-W0033) has the same ascription-descent gap
-- it still warns "handler clause for 'Ask' is unreachable" on this shape, a
false positive filed separately.

**Severity:** low (coverage gap for the CPS-IR-to-C backend, not a miscompile:
the direct-style / fiber path still handles such functions correctly).

## Summary

The whole-program may-capture coloring in `src/passes/cps.c`
(`cps_expr_contains_shift`, `cps_collect_calls`, and the call-graph transform)
does not have an `EX_ASCRIBE` case, and their `default` does not descend. So a
control operator (`perform` / `shift` / `reset` / ...) that appears **only**
inside a type ascription `(:: <control-op> T)` is invisible to coloring: the
enclosing function is not seeded/colored, and therefore the CPS-IR-to-C backend
(`--enable=cps-backend`) never even considers it (it stays on the fiber path).

Type ascription is erased at codegen (`emit_expr.c` unwraps `ascribe_.inner`),
and Phase C5 made `cps_ir.c` peel ascriptions, so a colored function *with an
ascription elsewhere* now lowers. But when the ascription is the **only** thing
wrapping the control op, coloring misses it upstream of the IR.

## Minimal repro

```turmeric
(defeffect Ask [] : int)
(defn use-ask [] : int
  (+ 1 (:: (perform (Ask)) :int)))   ; perform only reachable through the ascription
(defn run [] : int
  (handle (use-ask) (Ask [] k) (resume k 41)))
(defn main [] : int (println (run)) 0)
```

`tur check --dump-cps` shows `run` calling `use-ask` as `cps->direct` (i.e.
`use-ask` is treated as uncolored); `use-ask` is not listed as a `cps-fn`.
Direct-style still prints the correct value (`42`), so it is a coverage gap, not
a miscompile.

## Root cause

`src/passes/cps.c` -- `cps_expr_contains_shift` (~line 25) and the sibling
traversals have explicit per-kind cases and a non-descending `default`; there is
no `case EX_ASCRIBE: return cps_expr_contains_shift(e->as.ascribe_.inner);`.

## Fix directions

Add an `EX_ASCRIBE` case that recurses into `e->as.ascribe_.inner` in every
cps.c traversal that walks expression subtrees (`cps_expr_contains_shift`,
`cps_collect_calls`, and the coloring transform switch). This was intentionally
**not** done under C5: coloring is always-on and feeds decisions beyond this
backend (fiber-lift / cps-path), so widening it is a broader-impact change best
made deliberately with its own regression pass, not folded into a backend
coverage fix. The C5 `cps_ir.c` peel already covers the common case (an
ascription on a non-control sub-expression of an already-colored function).
