# The effect forms: `discontinue` runs on neither engine, `compose-handlers` is not nesting, and `resume` is unchecked

**Severity: medium.** Found 2026-10-19 checking the examples for the effect forms'
API reference (`stdlib/forms/effects.tur`). Nothing under `tests/fixtures` calls
`discontinue` (one comment mentions it), and none composes handlers whose
clauses perform each other's effects.

## 1. `discontinue` cannot be used

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (handle (+ 1 (perform (Ask))) (Ask [] k) (do (discontinue k "boom") 0))
  0)
```

`tur run`: `this effect operation has no lowering here`; `TUR_TRACE_EVICT=1`
names the cause, `[EVICT] BODY-UNSUPPORTED eff=1 main unsupported form:
EX_DISCONTINUE` (`src/passes/cps_ir.c`, `emit_cps_ir.c:4784`). `tur --interpret`:
`*** stack smashing detected ***`, exit status 134 (`src/turi/eval.c`,
`case EX_DISCONTINUE`, which stores an error in `cont->body_result` and
`swapcontext`s back to the handler). The same in a helper function, with `f`
returning `nil`, and at top level.

`discontinue` is typed `nil`, so `(handle (f) (Ask [] k) (discontinue k "boom"))`
in an `int` position is TUR-E0252 -- there is no way to write a use that
type-checks and also means "raise at the perform".

## 2. `handle-shallow`, `try-with` and `with-handler` statements at top level are rejected when compiled

```turmeric
(defeffect Ask [] : int)
(println (try-with (+ 1 (perform (Ask))) (Ask [] k) (resume k 41)))
```

`tur run`: `this effect operation has no lowering here`. The same line with
`handle` prints 42, and all four forms print 42 inside a `defn main`.
`tur --interpret` prints 42 for all. Root cause: the top-level fold into a
synthesized `main` is abandoned when a statement holds a `perform` that no
`handle` guards, and `form_has_toplevel_unhandled_perform`
(`src/compiler/elab_toplevel.c:1888`, the guard list at line 1895) names only
`handle`, `reset`, `fn` and `lambda`. Fix: add `handle-shallow`, `try-with` and
`with-handler` to that list.

## 3. `resume` does not check its value against the effect's result type

```turmeric
(defeffect Ask [] : int)
(defn main [] : int
  (println (handle (+ 1 (perform (Ask))) (Ask [] k) (resume k "x")))
  0)
```

`tur check` is silent. `tur run` prints `94470584934401`, an address read as an
integer, and `tur --interpret` another. `perform` checks its arguments
(TUR-E0001) and a clause's result (TUR-E0252); the answer a clause hands back
is the one value nobody compares with `defeffect`'s declared type. Fix: compare
`value->type` with the effect's result type in `elab_make_resume`
(`src/compiler/elab_effects.c`), where the continuation's result type is already
looked up.

## 4. `compose-handlers` is not nesting when a clause performs the other handler's effect

```turmeric
(defeffect Ask [] : int)
(defeffect Log [msg : cstr] : nil)
(defn run [] : int
  (with-handler
    (compose-handlers (handler (Log [m] k) (do (println m) (resume k nil)))
                      (handler (Ask [] k) (do (perform (Log "asked")) (resume k 41))))
    (+ 1 (perform (Ask)))))
```

Compiled: `tur: unhandled effect (tag 2)`, exit status 134. Interpreted:
`unhandled effect: Log`, exit status 1. Swapping the two arguments does the same.
The nested form `(with-handler (handler (Log ...)) (with-handler (handler (Ask
...)) body))` prints `asked` and 42 on both engines. `docs/archive/history/
first-class-handlers-semantics.md` (FH0.1) states the two are equal. With clauses
that do not perform each other's effects, `compose-handlers` works.

## 5. `cont?` after a `resume` differs between engines, and cannot guard a second resume

```turmeric
(defeffect Ask [] : int)
(defn h [k : effect-cont<int> v : int] : int (let [r (resume k v)] (if (cont? k) r -1)))
(println (handle (+ 1 (perform (Ask))) (Ask [] k) (h k 41)))
```

Compiled: `-1` (a resumed continuation is no longer invocable, which is what the
fixture comments say `cont?` means). Interpreted: `42`. For a `^multishot` `k`
compiled `cont?` is still false after the first `resume`, so the natural guard
`(if (cont? k) (+ r (resume k 1)) -1)` never takes the second resume (`-1`);
interpreted, `cont?` is true there and the second `resume` then fails with
`this continuation has already been resumed and its body has finished.
Multi-shot resume needs the work-stack path ...`, and a clause that calls
`cont?` is evidently one the interpreter's multi-shot path does not take. The same
clause without the `cont?` call returns 44 on both engines. Needs a decision on
what `cont?` of a `^multishot` continuation means, then a fixture on both.

## 6. Smaller things

- A compiled unhandled effect prints `tur: unhandled effect (tag 2)`; the
  interpreter names it (`unhandled effect: Ask`). Print the effect name.
- `(handle 5)`, with no clauses, is accepted by the elaborator and is `5`
  interpreted; compiled it is ``this `handle` has no lowering here``, a message
  about the backend for what is a malformed form (an odd clause list already gets
  `handle expects pairs of (case-header body)`).
- The interpreter's `handle` runs the body's pending `defer`s at the wrong time
  when a clause does not resume:
  [turi-fiber-handle-defers-fire-at-the-wrong-time](turi-fiber-handle-defers-fire-at-the-wrong-time.md)
  (already filed; the `handle` page links it).

## Fix directions

1. Lower `discontinue` in the CPS backend (raise at the `perform` after running
   the body's defers) or retire the form; at minimum make the interpreter fail
   with a message rather than overrun the stack.
2. Extend the guard list in `form_has_toplevel_unhandled_perform`.
3. Check `resume`'s value against the effect's result type.
4. Make `compose-handlers` dispatch like the nested form, or document it as
   disjoint-only and reject a clause that performs the sibling's effect.
5. Pick one meaning for `cont?` on a resumed continuation and give both engines
   a fixture.
