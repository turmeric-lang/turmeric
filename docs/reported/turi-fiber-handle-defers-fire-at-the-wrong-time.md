# `tur --interpret`: a fiber-path `handle` fires the body's pending `defer`s at the wrong time

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
