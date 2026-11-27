# In a function that performs an effect, a `return` arm that still owns a Drop local does not release it

**Severity:** low (a leak, one value per call, on one path).

A Drop-owning local consumed by one arm of an `if` is released by the other
arm. When the other arm is a `return`, the release is recorded on the `return`
node (`return_.owed_drops`) and the direct C emitter runs it. A function that
performs an effect is lowered differently, and its `return` does not.

This is separate from
[cps-pass-drops-return-drop-lists](cps-pass-drops-return-drop-lists.md), which
loses the record for every function. The output below is with that report's
fix applied locally; without it this function leaks for that reason first.

Found 2026-11-27. `main` at `853f866`, plus the one-case fix named above.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)
(defn take [w : Widget] : int (rel w) 5)
(defeffect Ask [] : int)

(defn e5 [c : bool] : int
  (let [h (mk)
        a (perform (Ask))]
    (if c (return 0) (rel h))
    a))

(defn e6 [c : bool] : int
  (let [h (mk)
        a (perform (Ask))]
    (if c (return (take h)) nil)
    (+ a (peek h))))

(defn run5 [c : bool] : int (handle (e5 c) (Ask [] k) (resume k 1)))
(defn run6 [c : bool] : int (handle (e6 c) (Ask [] k) (resume k 1)))

(defn main [] : int
  (println "e5 true") (run5 true)
  (println "e5 false") (run5 false)
  (println "e6 true") (run6 true)
  (println "e6 false") (run6 false)
  0)
```

```
e5 true
  mk
e5 false
  mk
  rel
e6 true
  mk
  rel
e6 false
  mk
  rel
```

`e5 true` makes a widget and never releases it. `e6` is right on both paths:
a `return` whose own value hands the local on is handled, so the gap is only
the drop a branch join left for the `return` to run.

## Root cause

Not traced to a line. The direct emitter runs the list at
`src/compiler/emit_stmt.c:1574`, and `emit_tail` routes a tail-position
`return` there when `n_owed > 0` (`src/compiler/emit_fns.c:2042`). The
lowering used for a function that performs does not go through either. The
interpreter does run the list (`src/turi/eval.c:10069`), so `tur --interpret`
and the compiled program can disagree here.

## Fix directions

- Find the `return` emission for a colored function and run `owed_drops`
  there, after the frame's defers, as `emit_stmt` does.
- Or lower the owed drops in the elaborator when the enclosing function is
  colored: `(return v)` with owed drops becomes
  `(let [t v] (drop h) (return t))`, which needs nothing from the emitter.
- A fixture: the program above, with `mk` and `rel` counted.
