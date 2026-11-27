# A Drop-owning value passed by value to a function is never dropped unless the function consumes it by hand

> **Status: fixed 2026-11-27** for an `^affine` Drop opaque passed to a
> Turmeric-bodied `defn`. See [Resolution](#resolution) for what that covers
> and what it leaves.

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

## Resolution

Fixed 2026-11-27, by the first direction, in `src/compiler/elab_fns.c`
(`elab_owned_params_mark`, `elab_owned_params_append_drops`).

A by-value parameter of an `^affine` move-only Drop opaque, in a `defn` with
a Turmeric body, now gets what a `let` local of that type has: its
`scope_drop_inst` is set while the body is elaborated, so an `if` / `match`
join, a loop and a `return` do their per-path bookkeeping for it, and a
trailing scope-exit drop is appended for the paths that still own it. All
four widgets in the repro are released, compiled and interpreted.

**What counts as still owning it.** A parameter is not an `^affine` binding
(a second use of one is not TUR-E0150), so its uses were not counted. They
are now, quietly (`Binding.owned_drop_param`): any use outside a `^borrow`
argument slot counts as handing the value on and suppresses the drop on that
path. A by-value call and a returned value are real hand-offs; a cast, a
field store or a capture is counted the same way, which can only trade this
fix for the old leak, never free twice.

**The type's own release.** A Drop method that hands its value to a helper
-- `(drop [x] (rel x))` -- is the release. A `rel` that never mentions its
parameter would otherwise drop it, calling the method, calling `rel`,
without end. A function the Drop method passes its own parameter to gets no
drop for that position, whichever of the two is defined first.

Kept out, deliberately:

- **A plain Drop opaque** (no `^affine`). Measured while fixing this: passing
  a local of one by value is not a hand-off; the caller keeps it and drops it
  at scope exit. A callee-side drop would free it twice.
- **A `^borrow` parameter**, whose owner is the caller, and a `^linear` one,
  which must be consumed (TUR-E0100).
- **A body with inline C anywhere in it.** C is trusted to consume an owned
  handle and cannot be read.

Fixtures: `owned-drop-param` (ignored, lent on, returned, passed on, handed
on along one path of an `if` and a `match`, an early return that hands it
on, a loop, a `^borrow` parameter, an effect-performing function; compiled
and interpreted agree), `owned-drop-param-release-helper` (the recursion
case), and `owned-drop-param-early-return` (an early return that still owns
the parameter; compiled only, for the interpreter gap in
[turi-early-return-skips-injected-drops](../reported/turi-early-return-skips-injected-drops.md)).

`bash tests/run.sh`: 4354 passed, 0 failed. No function in `stdlib/` or in
turmeric-spices picks up a drop: theirs take `^borrow`, consume, or are
inline C. `TUR_TRACE_PARAM_DROP=1 tur check FILE` lists the functions that do.

Not changed, each measured on the fixed tree:

- **A function defined before the type's `definstance Drop`.** When it is
  elaborated the type has no Drop instance yet, so its parameter is not
  owned by this rule and still leaks. Put the instance ahead of the
  functions that take the type by value.
- **A lambda** (`(fn [w : Widget] ...)`) and **a typeclass instance method**
  taking the value: neither goes through `elab_defn`. Both still leak.
- **A temporary of a plain Drop opaque** passed by value (`(ignore (mk))`):
  no local owns it, and the callee does not drop a non-affine parameter.
- **An early `return` taken before a later hand-off.**
  `(if c (return 9) nil) (take w)` leaks `w` when `c` is true. A `let` local
  has the same gap, older than this report:
  [early-return-before-later-consume-leaks](early-return-before-later-consume-leaks.md).
