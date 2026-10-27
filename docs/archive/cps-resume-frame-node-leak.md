# Track-A perform continuation leaks its `dk_frame_resume` node (multi-shot / nested-control resume)

**RESOLVED.** The Track-A branch of `emit_perform` now registers the
`dk_frame_resume` node with `__dk_reap_node(...)` (single-node free at the
outermost entry boundary), matching the reset/handle structural-node reaping --
so the node is reclaimed once the computation settles, multi-shot-safe (it
outlives every re-entrant `dk_perform`) and without walking into `cur_k`.
Verified under ASan/LSan: the minimal repro below and
`cps-backend-two-perform` / `cps-backend-owning-struct-capture-multishot` are
leak-clean. The **separate** snapshot-not-freed leak on
`shift-crossfn-resume-works` (see "Scope / relationship" below) is untouched and
remains tracked in `docs/upcoming/cps-runtime-finish-plan.md` (Phase 3); that
fixture keeps its `requires.no-leak-check` until the snapshot leak is fixed.

**Severity:** low (bounded, per-perform-execution heap leak of one DK node; not a
correctness bug). Sibling of the fixed DK-node leaks
(`docs/archive/cps-delimited-dk-node-leak.md`,
`docs/archive/cps-effect-perform-carrier-leak.md`).

## Summary

When a `perform`'s continuation contains a NESTED control op (Track A of
`cps-backend-multishot-continuations-owning-capture-plan.md` -- the "resumed
twice" shape), the CPS backend lifts the continuation as a RESUME-FRAME and
splices a `dk_frame_resume(...)` node as the perform continuation, spliced onto
`cur_k` (its `->next`). Unlike the sibling straight-line perform continuation --
which reclaims its `dk_frame(...)` node with `dk_free_node` after `dk_perform`
settles -- the Track-A branch emits `return dk_perform(..., dk_frame_resume(...))`
inline and never frees the node. One DK node (88 bytes) leaks per execution of
such a perform.

## Minimal repro

```turmeric
(defeffect E [] : int)
(defn g [] : int
  (let [a (perform (E))]
    (let [b (perform (E))]     ; nested control op in the continuation of the first perform
      (+ a b))))
(defn f [base : int] : int
  (handle (g) (E [] k) (resume k (+ base 1))))
(defn main [] : int (println (f 40)) 0)
```

Build with an ASan/LSan `cc` and run: prints the correct value, then LSan reports
`88 byte(s) leaked` from `dk_frame_resume` (via `dk_new`) in `g__cps`.

Also reproduces in `tests/fixtures/cps-backend-two-perform`,
`cps-backend-owning-struct-capture-multishot`, and (multi-shot, ~14 nodes)
`tests/fixtures/shift-crossfn-resume-works`.

## Root cause

`src/compiler/emit_cps_ir.c:4146` (the Track-A branch of `emit_perform`):

```c
ce_line(ce, "return dk_perform(%d, %s, dk_frame_resume(%s, %s, %s));",
        tag, sa, pname, envexpr, ce->cur_k);
```

The `dk_frame_resume(pname, env, cur_k)` node's `->next` is `cur_k`, so it is a
single spliced node (dk_free would walk into the enclosing continuation). It is
never reclaimed. Contrast the straight-line sibling one branch up (`:4120-4124`):

```c
DK *__pfd = dk_frame(pname, 0, cur_k);
int64_t __pfr = dk_perform(tag, sa, __pfd);
dk_free_node(__pfd);
return __pfr;
```

The Track-A branch was added later (F3/Track-A nested-control resume) and did not
pick up the single-node free / reap discipline.

## Fix directions

1. Mirror the sibling: hoist the node, capture the `dk_perform` result,
   `dk_free_node` the node, then return -- i.e.
   `DK *__rfd = dk_frame_resume(...); int64_t r = dk_perform(tag, sa, __rfd); dk_free_node(__rfd); return r;`.
   Verify multi-shot safety first: `dk_perform` copies the captured range and the
   handler/resume operates on those copies, so the original node should be dead
   once `dk_perform` returns (same argument as the sibling) -- confirm under ASan
   on `shift-crossfn-resume-works` (multi-shot) that no use-after-free results.
2. Or register the node with `__dk_reap_node(...)` (single-node reap at the
   outermost entry boundary), matching how the reset/handle/join structural nodes
   are handled (`docs/archive/cps-delimited-dk-node-leak.md`). This is the
   conservative choice if the node can outlive `dk_perform` under some resume
   shape.

## Scope / relationship to other work

- This is a LEAK, not an eviction and not a miscompile. It does NOT block N6.5
  (fallback deletion); it only blocks dropping `requires.no-leak-check` on the
  multi-perform / multi-shot fixtures above.
- Independent of the native handle-in-reset eviction
  (`docs/archive/cps-native-handle-in-reset-plan.md`): making that shape native
  does not touch this node, and this leak persists on the native multi-shot resume
  path regardless.
- Related sibling on the now-native cross-function shift path
  (`docs/archive/cps-native-handle-in-reset-plan.md`, Reduction B, landed): the
  __Shift receiver's continuation is bridge-wrapped as a `tur_cloneable_cont` and
  resumed via `tur_continuation_snapshot` (`tur_cloneable_cont_clone` ->
  `dk_copy_range`), which clones the DK chain per resume without ever freeing the
  snapshot -- the same snapshot-not-freed leak the fiber path already had (far
  smaller now: ~3.3 KB vs ~4 MB on `shift-crossfn-resume-works`). Memory-safe (no
  UAF/double-free/UB). Fixing it needs a resume-and-drop discipline in the SHARED
  direct-emitter receiver codegen (the `tur_cloneable_cont_resume(snapshot(k), v)`
  it emits for `(k v)`), so it affects the fiber path too and is out of scope of the
  Reduction B admission/emit change. `tests/fixtures/shift-crossfn-resume-works/`
  carries `requires.no-leak-check` until this is resolved.
