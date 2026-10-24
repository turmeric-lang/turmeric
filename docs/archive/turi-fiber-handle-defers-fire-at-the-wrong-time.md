# `tur --interpret`: a fiber-path `handle` fires the body's pending `defer`s at the wrong time

**RESOLVED 2026-10-24 by fix direction 1: `defer` and `catch-unwind` are
work-stack capturable**, so a body that reaches its `perform` under either no
longer takes the single-shot fiber runtime, and a `^multishot` resume of it
runs (case 5). In `src/turi/eval.c`:

- A `DK_PROMPT` records the defer chain and the catch-boundary stack at its
  push (`dbase` / `cbase`). A capture takes what the body holds above them off
  the env (`ws_capture_body_state`): the body's pending defers go to the
  continuation (`TuriWsCont.dseg`, moved into the env's pool for a multi-shot
  `k`, `ws_capture_rehome_defers`), its catch boundaries ride the slice's
  `DK_CATCH_UNWIND` frames. The clause runs with neither -- its scope exits do
  not fire the body's defers, and its panics do not land in the body's catches.
- A resume (`ws_slice_reinstall`) puts them back above the chain where it
  resumes -- a copy per resume unless the continuation is one-shot --
  remapping every defer mark the slice holds (call, `let` and `do` scopes, a
  nested prompt's base, a catch boundary's saved chain), and links the
  boundaries back above `g_catch_stack`.
- A case that returns having neither resumed nor kept `k` fires the body's
  defers then (`ws_case_abandoned`), innermost scope first, and those of an
  inner handle's case abandoned with it (a re-opening clause), as the compiled
  runtime does -- also when the case leaves by an escape.
- `ws_capturable` admits a `defer` whose body performs nothing when it fires
  (`ws_defer_body_ok`, which answers a direct call from the callee's body) and
  a `catch-unwind` whose thunk is a lambda (its body is checked). `panic`,
  `panic-with` and `discontinue` are no longer black boxes to it.

Pinned by `defer-abortive-handler-no-inline-c` -- the inline-C-free twin of
`defer-cps-abortive-handler` the last paragraph asked for, all its cases but
16 and 17, on all three engines -- plus `cps-catch-unwind-multishot` and
`discontinue-multishot`, which lost their `requires.compiled`.

Still on the fiber: a body that reaches its `perform` under `catch-panic-of`
(the driver has no frame for a typed catch; `errors/turi-multishot-resume-
past-fiber-body` now uses that shape), or under a `defer` whose body itself
performs. Not this report: cases 16 and 17 of the compiled fixture -- a
deferred body that panics -- are swallowed by the interpreter at any scope
exit, handle or not:
[turi-defer-body-panic-is-swallowed](../reported/turi-defer-body-panic-is-swallowed.md).

**Narrowed again 2026-10-22: the stored-`k` case is fixed too; what is left
is the single-shot limit.** Each side of a fiber `handle` now has its own
defer chain (`fiber_enter_body`; fixed with
[turi-fiber-resume-from-clause-scope-crashes-on-body-defer](../archive/turi-fiber-resume-from-clause-scope-crashes-on-body-defer.md)),
so a kept `k`'s body keeps its defers on the cont. They run when a later
resume finishes the body, or never if nobody resumes it. Both rows of the
table below now read as the compiled column
(`tests/fixtures/turi-fiber-defer-resume-from-clause-scope`). The one open item
is case 5: the fiber runtime is single-shot, so a multi-shot resume over a
body that reaches a `perform` under a `defer` (or a `catch-unwind`; neither is
work-stack capturable) is refused. Fix direction 1 is what is left.

**Narrowed 2026-10-21: the abortive case is fixed; the stored-`k` case stays
open.** `eval_handle` marks the defer chain before the body runs; when a case
returns with the body suspended and no case can keep `k`
(`fiber_handle_k_may_escape` is false), the entries above the mark are fired
by scope right there, so the first row of the table below now reads
`w4: defer ran`, then `7` on both engines
(`tests/fixtures/turi-fiber-abortive-defer`, run by both harnesses). What
remains is the second row: a case that may keep `k` still leaves the body's
defers on the chain, where the next enclosing scope exit fires them, while
the compiled runtime holds them until the resume. Fix direction 2's "park
them on the cont" (or fix direction 1) is what is left.

**Severity:** low-medium (engine divergence: output order, and a refused
multi-shot resume). Interpreter only. The compiled program is correct. A
`handle` takes the interpreter's single-shot fiber runtime when its body
reaches a `defer` (`ws_capturable` refuses `EX_DEFER`). On that runtime the
body's pending defers run at the wrong time. Filed 2026-10-18 while fixing
[turi-defer-in-stored-continuation-use-after-free](../archive/turi-defer-in-stored-continuation-use-after-free.md),
which is the same shape's memory-safety half.

## Repro

```turmeric
(defeffect Ask [] : int)

(defn w4 [] : int
  (defer (println "w4: defer ran"))
  (+ 1 (perform (Ask))))

(defn main [] : int
  (println (handle (w4) (Ask [] k) 7))          ; abortive case
  (let [^mut saved 0]
    (println (handle (w4) (Ask [] k) (do (set! saved k) 0))))
  (println "never resumed")
  0)
```

| | compiled (`tur run`) | `tur --interpret` |
| --- | --- | --- |
| abortive case | `w4: defer ran`, then `7` | `7`, then `w4: defer ran` |
| `k` stored, never resumed | `0`, `never resumed` | `0`, `w4: defer ran`, `never resumed` |

`tests/fixtures/defer-cps-abortive-handler` pins the compiled semantics. Under
the interpreter it shows the abortive shape in its cases 1 and 2. Its case 5
(multi-shot over a defer-bearing body) is refused with "this continuation has
already been resumed", because the fiber runtime is single-shot. That fixture
is in `run-turi.sh`'s inline-C carve-out, so no suite runs it under the
interpreter.

## Root cause

The body's `defer` is pushed onto `env->defer_stack` while the body runs on its
fiber stack. When the case returns without resuming, `eval_handle`
(`src/turi/eval.c`) leaves that entry on the stack. It then fires at the next
enclosing scope exit that drains the defer stack (here `main`'s `let`), not at
the case's return. The compiled DK runtime fires the abandoned frames' defers
when the case returns (`cps-defer-skipped-on-abortive-effect`), and never fires
a stored continuation's defers until it is resumed.

## Fix directions

1. **Root fix:** make `defer` work-stack capturable, so this shape never reaches
   the fiber runtime. The captured slice would carry the body's defer entries
   and re-install them on each resume. That would also lift the single-shot
   limit (case 5).
2. **Narrower:** in `eval_handle`, mark the defer-stack depth before the body
   runs. When the case returns with the body suspended, take the entries above
   the mark off the stack. Fire them if the case could not have kept `k`
   (`fiber_handle_k_may_escape` is false). Otherwise park them on the cont, to
   run when a later resume finishes the body.

Then remove `defer-cps-abortive-handler` from the carve-out, or add an inline-C
free twin of its cases 1, 2 and 4.
