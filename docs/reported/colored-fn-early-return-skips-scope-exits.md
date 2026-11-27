# In a function that performs an effect, an early `return` runs no scope exit

**Severity:** medium (an explicit `defer` is skipped; a `ref<T>` cell or a
Drop local leaks, one per call on that path). The compiled program and
`tur --interpret` disagree.

An ordinary function's `return` computes its value, then runs every scope
exit it is leaving: the registered `defer`s, the drops the enclosing `let`s
injected, and the drops a branch join left for it (`return_.owed_drops`). A
function that performs an effect is lowered through the CPS IR, where `return`
hands its value straight to the function's return continuation. Nothing on
that path runs.

**Older than #1486.** The first three functions below behave the same on
`main` at `d4ad1b0`, before it. #1486 added `e5`'s shape to the set: the
release is now recorded on the `return` node, and this lowering does not read
it.

Found 2026-11-27. `main` at `853f866` plus the fix for
`cps-pass-drops-return-drop-lists` (now in `docs/archive/`); without that fix
`e5` leaks for that reason first.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)
(defeffect Ask [] : int)

;; An explicit defer.
(defn e8 [c : bool] : int
  (let [a (perform (Ask))]
    (defer (println "  deferred"))
    (if c (return 0) nil)
    (+ a 1)))

;; A ref<T> local: the compiler frees the cell at scope exit.
(defn e9 [c : bool] : int
  (let [r (ref 41)
        a (perform (Ask))]
    (if c (return 0) nil)
    (+ a (deref r))))

;; A Drop local the return path still owns.
(defn e7 [c : bool] : int
  (let [h (mk)
        a (perform (Ask))]
    (if c (return 0) nil)
    (+ a (peek h))))

;; The same, with the other arm consuming it (the #1486 shape).
(defn e5 [c : bool] : int
  (let [h (mk)
        a (perform (Ask))]
    (if c (return 0) (rel h))
    a))

(defn run8 [c : bool] : int (handle (e8 c) (Ask [] k) (resume k 1)))
(defn run9 [c : bool] : int (handle (e9 c) (Ask [] k) (resume k 1)))
(defn run7 [c : bool] : int (handle (e7 c) (Ask [] k) (resume k 1)))
(defn run5 [c : bool] : int (handle (e5 c) (Ask [] k) (resume k 1)))

(defn main [] : int
  (println "e8 true") (run8 true)
  (println "e8 false") (run8 false)
  (println "e9 true") (run9 true)
  (println "e7 true") (run7 true)
  (println "e7 false") (run7 false)
  (println "e5 true") (run5 true)
  (println "e5 false") (run5 false)
  0)
```

```sh
tur run r/main.tur
```

```
e8 true
e8 false
  deferred
e9 true
e7 true
  mk
e7 false
  mk
  rel
e5 true
  mk
e5 false
  mk
  rel
```

On the `true` path `e8` never prints `deferred`, and `e7` and `e5` never
release their widget. `e9` prints nothing either way; under
`TUR_SANITIZE_PROGRAMS=1` LeakSanitizer reports its cell, 8 bytes in one
allocation, from the `true` call.

`tur --interpret r/main.tur` prints `deferred` under `e8 true` and `rel` under
`e5 true`. It prints no `rel` under `e7 true` either, but for a reason of its
own that has nothing to do with effects:
[turi-early-return-skips-injected-drops](turi-early-return-skips-injected-drops.md).

A `return` whose own value hands the local on, `(return (take h))`, is right
in the compiled program too: there is nothing left to release on that path.

## Root cause

`src/passes/cps_ir.c:6048` (tail position) and `:6607` (bind position):

```c
case EX_RETURN:
    return cps_tail(b, e->as.return_.value, b->retk);
```

The value goes to `b->retk`, the function's return continuation, and the
node's other fields are not read. The scope exits a normal fall-through runs
are threaded by `cps_do_thread_defers` (`src/passes/cps_ir.c`), which follows
the `do`'s own tail; a `return` in the middle leaves that thread.

The direct emitter is `src/compiler/emit_stmt.c:1496`: it fires the frame
chain, runs the pending injected drops (`emit_frame_run_pending_drops`), then
`owed_drops`. The interpreter runs `owed_drops` at `src/turi/eval.c:10069`.

## Fix directions

- In both `EX_RETURN` cases, bind the value, then thread what the direct
  emitter runs -- the `defer`s of every enclosing `do` still open, the pending
  injected drops, `owed_drops` -- and only then deliver the value to `retk`.
  The builder needs to know which scopes are open at that point; today it
  tracks that only for the `do` it is threading.
- Or lower it in the elaborator when the enclosing function is colored:
  `(return v)` becomes `(let [t v] <exits> (return t))`, with the exits spelled
  out as ordinary calls. That needs nothing from the CPS IR, at the cost of a
  second place that knows the exit order.
- A fixture: the program above. It has no inline C, so `tests/run-turi.sh`
  runs it too; `e7` will hold the two engines to one answer only once the
  interpreter report linked above is fixed as well.
