# `tur --interpret`: resuming a defer-bearing body from inside a scope in the clause crashes

**RESOLVED 2026-10-22** (archived). Each side of a fiber `handle` now has its
own defer chain. `fiber_enter_body` (`src/turi/eval.c`) installs the body's
chain (`TuriEffectCont.body_defers`) for as long as the body runs, saves it
back when the body yields, and gives the handler side its own chain again.
When the body finishes, what it left above its base (a `defer` at the top of
the body, which belongs to the scope around the `handle`) is spliced onto the
handler side's chain, as the shared chain used to leave it (`unsafe-defer`).
The abortive case's firing in `eval_handle` runs on the body's saved chain.

This also fixed the stored-`k` half of
[turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md).
A body a case kept but never resumed no longer fires its defer at the next
enclosing scope exit; its items stay parked on the cont. A body resumed after
its `handle` exited runs them at the resume. Both now match the compiled
runtime.

Pinned by `tests/fixtures/turi-fiber-defer-resume-from-clause-scope`, which
`run.sh` and `run-turi.sh` both run: resume from a `let` and from a local
function in the clause, a kept `k` never resumed, and a kept `k` resumed after
its handle exited. The `catch-unwind`-in-the-clause shapes of the table below
also run interpreted now. They are not in the fixture yet, because compiled
they hit
[cps-catch-unwind-cannot-cross-an-effect-capture](../reported/cps-catch-unwind-cannot-cross-an-effect-capture.md).

Suite: `run-turi.sh` 2993 passed, 0 failed.

**The root cause below is corrected.** The walk that crashes is the
clause's, not the body's. The clause's `let` pushes a scope marker on top of
the body's defer item and records the item as its fire-to mark. The resumed
body's return from `body` then pops both the marker and the item. When the
`let` exits, it walks from the chain's head for an item that is gone and runs
off the NULL end.

**Severity: high** (a segfault on an ordinary shape; interpreter only, the
compiled program is correct). It **blocks
[discontinue-plan](../upcoming/discontinue-plan.md)** on the interpreter: the
plan's fixture 1 gives the body a `defer`, which sends the `handle` to the
fiber runtime, and its fixture 3 discontinues from inside a `catch-unwind` in
the clause, a scope like the `let` below. Found 2026-10-22 checking what blocks
that plan; reproduces on `main` (`a0a718f3`) and on #1326's head.

## Repro

```turmeric
(defeffect Ask [] : int)
(defn body [] : int
  (defer (println "body: defer ran"))
  (+ 1 (perform (Ask))))
(defn main [] : int
  (println (handle (body) (Ask [] k) (let [x 1] (resume k x))))
  0)
```

`tur run` prints `body: defer ran`, then `2`. `tur --interpret`:

```
src/turi/eval.c:3234:32: runtime error: member access within null pointer of type 'struct DeferItem'
AddressSanitizer: SEGV ... in fire_defers_to_mark   (eval.c:3234)
    #1 eval_drive_ex (eval.c:11672, DK_CALL_RET)
```

| clause | `tur --interpret` |
| --- | --- |
| `(resume k 1)`, `(+ 0 (resume k 1))` | `body: defer ran`, `2` |
| `(let [x 1] (resume k x))` | SEGV |
| `(let [f (fn [] : int (resume k 1))] (f))` | SEGV |
| `(let [r (catch-unwind (fn [] : int 7))] (resume k 0))` | SEGV |
| `(catch-unwind (fn [] : int (resume k 0)))` (and the body panics) | SEGV in `fire_defers_to_mark_by_scope` (eval.c:3285) |

Without the `defer` in the body the `handle` stays on the work-stack path and
all of these run.

## Root cause (as filed; corrected above)

A `defer` in the body makes the `handle` take the fiber runtime
(`ws_capturable` refuses `EX_DEFER`). The body and the clause share one
`env->defer_stack`. The body runs on its fiber and pushes its defer item, then
swaps out at the `perform`. The clause then opens a scope (the `let`, a call)
that records and later restores its own mark on the same chain. The `resume`
swaps back into the body from inside that scope. When the body's call to
`body` returns, `DK_CALL_RET` walks the chain from its current head down to the
mark it took at `body`'s entry (`top->aux`). The clause's scope has moved the
head, so that mark is not on the chain, and the walk runs off the NULL end.
This is the same shared chain
[turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md)
narrows on (`body_defer_mark` in `eval_handle`, eval.c:4104), here as a memory
fault rather than a timing difference.

## Fix directions

1. **Give the body its own defer chain.** Save and restore `env->defer_stack`
   on every swap: the body's head is kept on the `TuriEffectCont` when it swaps
   out at a `perform`, and reinstalled when a resume swaps in, while the
   clause's head is saved across the resume and restored when the body yields
   or finishes. Each context then only ever walks its own items. This is also
   most of fix direction 2 of the related report ("park them on the cont").
2. **The root fix the related report names:** make `defer` (and
   `catch-unwind`) work-stack capturable, so these shapes never reach the fiber
   runtime.
