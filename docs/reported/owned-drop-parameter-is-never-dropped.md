# A Drop-owning value passed by value to a function is never dropped unless the function consumes it by hand

**Severity:** medium (a silent leak in an ordinary shape; no diagnostic).

A `let`-bound move-only Drop opaque -- the `^affine` + `Drop` shape
`stdlib/numvec.tur` uses -- is released at scope exit. A *parameter* of the
same type is not: passing the value moves it out of the caller, so the caller
does not drop it, and the callee has no scope-exit drop for its parameters.
A function that ignores such a parameter, or consumes it on one path only,
leaks it.

This is why `spices/manifold` keeps its handles `^linear`: there the same
function is TUR-E0100. It predates #1486 and is listed there as not changed.

Verified 2026-11-27 on `main` at `853f866`.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn take [w : Widget] : int (rel w) 5)

(defn ignore [w : Widget] : int 7)
(defn maybe [w : Widget c : bool] : int (if c (take w) 0))

(defn main [] : int
  (println "ignore, a temporary") (ignore (mk))
  (println "ignore, a local") (let [h (mk)] (ignore h))
  (println "maybe true") (maybe (mk) true)
  (println "maybe false") (maybe (mk) false)
  0)
```

```
ignore, a temporary
  mk
ignore, a local
  mk
maybe true
  mk
  rel
maybe false
  mk
```

Three of the four widgets are never released. `tur check` is silent. With a
heap handle, `TUR_SANITIZE_PROGRAMS=1` reports each as a LeakSanitizer leak.

## Root cause

The scope-exit drop is injected by `elab_let` only. A binding gets
`scope_drop_inst` at `src/compiler/elab_forms.c:2481`, and the drop defers are
appended after the `let` body at `:2739`. Function parameters are bound
elsewhere and never get either, so nothing in `ignore` owns the release of
`w`. The caller treats the call as a move (that is what stops a double free
when the callee does consume), so it does not drop either.

## Fix directions

- Give an owned Drop-opaque parameter the same scope-exit drop a `let`
  binding has, skipped on the paths that consumed it. #1486's per-path
  machinery (`elab_branch_with_scope_drop`, `ArmMoves`, `return_.owed_drops`)
  keys on `scope_drop_inst`, so setting it for parameters covers `if`,
  `match` and early `return` at once. The function body needs the trailing
  defers a `let` body gets.
- A `^borrow` parameter must stay out: the caller still owns it.
- Inline-C bodies cannot be analysed. Today they are trusted to consume an
  owned parameter; that has to stay the rule, or every inline-C destructor
  double-frees.
- Check stdlib and turmeric-spices for functions that rely on the leak
  (a by-value parameter the caller keeps using through an alias).
