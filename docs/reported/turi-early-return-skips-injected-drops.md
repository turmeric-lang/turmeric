# Under `tur --interpret`, an early `return` skips the drop a `let` injected

**Severity:** low (a Drop instance does not run on that path, in the
interpreter only; the compiled program is right).

A `let` that binds a Drop-owning local gets a scope-exit drop appended after
its body. An early `return` leaves before reaching it. The C emitter runs the
pending drops inline at the `return`
(`docs/archive/auto-drop-defers-skipped-on-early-return.md`); the interpreter
raises its returning flag and unwinds without them. Memory is not the issue in
the interpreter, which keeps its values for the life of the process; a Drop
instance that closes a file or releases a lock is.

Found 2026-11-27 while checking a compiled-only report against the
interpreter. `main` at `853f866`. No effect is involved.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)

(defn d7 [c : bool] : int
  (let [h (mk)]
    (if c (return 0) nil)
    (peek h)))

(defn main [] : int
  (println "d7 true") (d7 true)
  (println "d7 false") (d7 false)
  0)
```

```sh
tur --interpret r/main.tur
```

```
d7 true
  mk
d7 false
  mk
  rel
```

`tur run r/main.tur` prints `rel` under `d7 true` as well.

The shapes #1486 added do work in the interpreter: when the other arm of the
`if` consumes `h`, the release is recorded on the `return` node and the
interpreter runs it (`tests/fixtures/affine-drop-branch-paths-trace`). The gap
is only the drop still pending at the end of an enclosing `let`.

A panic that leaves the `let` does the same (measured 2026-11-27). With a
`defer` in the body, `(let [h (mk)] (defer ...) (boom) (peek h))` under
`catch-unwind` prints `rel` compiled and not interpreted. Without the defer
neither engine releases the local; the compiled half of that is
[caught-panic-skips-scope-exit-drops](caught-panic-skips-scope-exit-drops.md).

`tests/fixtures/defer-reads-let-local-drop-exits` and
`effect-fn-early-return-drops-owned-local` are compiled-only because of this
report; drop their `requires.compiled` when it is fixed.

## Root cause

`src/turi/eval.c:10065`, the `EX_RETURN` case: it runs `return_.owed_drops`,
sets `env->returning` and returns. The interpreter has no counterpart of
`emit_frame_run_pending_drops` (`src/compiler/emit_core.c`), which walks the
frame chain for the trailing drop-glue defers not yet registered. Where the
enclosing `do` stops evaluating its items once `returning` is set was not
traced.

## Fix directions

- When a `do` sees `returning` set after an item, run the trailing run of
  `is_drop_glue` defers it has not reached, skipping any binding the returned
  value moved out, as the emitter does.
- Or have the elaborator put every pending drop on the `return` node, so both
  engines read one list and the emitter's frame walk can go.
- A fixture: the program above, in `tests/fixtures/`; it has no inline C, so
  both harnesses run it.
