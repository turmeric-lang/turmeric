# A `perform` or `handle` anywhere in a program undoes the per-path `return` drops in every function

**Severity:** high (a double free, and a leak, in programs the compiler
accepts). **A regression from #1486 (2026-11-26):** one of the two shapes below
was a compile error before it.

#1486 taught `return` to release a Drop-owning local on the paths that still
own it, and to leave it alone on the paths that handed it on. The elaborator
records both on the `return` node. The CPS marking pass rebuilds every `return`
node in the program and copies only its value, so both records are gone by the
time C is emitted. The pass runs over the whole program as soon as one function
contains a `perform`, `handle`, `shift` or `reset`; it does not have to be
called, and the functions that break do not use effects themselves.

Found 2026-11-27 while measuring what #1486 left for colored functions.
Verified on `main` at `853f866`.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)
(defn take [w : Widget] : int (rel w) 5)

;; The return arm still owns h: it has to release it.
(defn d5 [c : bool] : int
  (let [h (mk)]
    (if c (return 0) (rel h))
    1))

;; The return arm hands h on: nothing is left to release.
(defn d6 [c : bool] : int
  (let [h (mk)]
    (if c (return (take h)) nil)
    (peek h)))

;; Never called.  Its presence is the whole trigger.
(defeffect Ask [] : int)
(defn asks [] : int (+ 1 (perform (Ask))))
(defn answers [] : int (handle (asks) (Ask [] k) (resume k 1)))

(defn main [] : int
  (println "d5 true") (d5 true)
  (println "d6 true") (d6 true)
  0)
```

```sh
tur run r/main.tur
```

```
d5 true
  mk
d6 true
  mk
  rel
  rel
```

`d5` never releases its widget; `d6` releases it twice. Delete the last three
definitions before `main` and both print one `mk` and one `rel`. With a heap
handle in place of the trace, `d6` is an AddressSanitizer double free.

Before #1486, `d6` was rejected (TUR-E0150, "used more than once"), so this
shape went from a compile error to a double free in any program with an
effect in it. `d5` was a double free on its other path before, and is a leak
on this one now.

`tests/fixtures/affine-drop-branch-paths` and `-trace` pin the same functions
and pass, because neither fixture contains an effect.

## Root cause

`src/passes/cps.c:2313`, in `cps_mark_expr`:

```c
case EX_RETURN: {
    Expr *new_value = e->as.return_.value ?
        cps_mark_expr(a, e->as.return_.value) : NULL;
    Expr *out = expr_new(a, EX_RETURN, e->type, e->span);
    out->as.return_.value = new_value;
    return out;
}
```

`return_.moved` / `n_moved` and `return_.owed_drops` / `n_owed`
(`src/compiler/expr.h:1902`) are not carried across. `cps_transform`
calls it on the whole program (`src/passes/cps.c:2856`), and only when
`cps_expr_contains_shift` finds something, which is why a program without
effects is unaffected.

Measured with a print in `elab_return` and one in the `return` emitter
(`src/compiler/emit_stmt.c:1544`): the elaborator sets `n_owed=1` on `d5`'s
node and `n_moved=1` on `d6`'s, and the emitter sees a different node, at a
different address, with both at zero.

## Fix directions

- Copy the four fields in that case, running `owed_drops` through
  `cps_mark_expr` like the value. The bindings in `moved` are not remapped by
  the pass, so the array can be shared. Tried locally on 2026-11-27 (not
  landed): both functions above then print one `mk` and one `rel`.
- Add an effect to `affine-drop-branch-paths-trace`, or a twin fixture with
  one, so the suite covers the pass.
- Any other pass that rebuilds nodes field by field has the same exposure to
  a field added later. `cps_mark_expr` is the only one that builds an
  `EX_RETURN` outside `elab_return`.

What is left for functions that themselves perform is
[colored-fn-return-does-not-run-owed-drops](colored-fn-return-does-not-run-owed-drops.md).
