---
title: Backtrackable State Guide
category: Advanced Control Flow
description: Trailed mutable cells from `stdlib/trail.tur` -- take a mark, mutate freely, then undo or keep the writes; scopes, the `Bt` capability, and why a search loop wants this instead of a persistent structure
---

# Backtrackable State Guide

`stdlib/trail.tur` gives you mutable cells whose writes **undo on demand**. You
take a mark, mutate freely, and then either throw the mutations away or keep
them. This is the WAM / constraint-logic-programming shape, and it is what a
search loop wants instead of threading a persistent structure through every
step.

It is autoloaded, so there is nothing to import and no flag to pass.

```turmeric
(defn main [] : int
  (let [c (bt-cell-new 10)
        m (bt-mark)]
    (bt-set! c 20)
    (println (bt-get c))    ; 20
    (bt-undo-to! m)
    (println (bt-get c))    ; 10 -- the write is gone
    (bt-cell-free c)
    0))
```
```sweet-exp
defn main [] : int
  let [c bt-cell-new(10)
        m bt-mark()]
    bt-set!(c 20)
    println(bt-get(c))    ; 20
    bt-undo-to!(m)
    println(bt-get(c))    ; 10 -- the write is gone
    bt-cell-free(c)
    0
```

## Why a trail rather than a persistent structure

Because undoing recorded state is cheaper than rebuilding it, and because it can
be **asymmetric**.

Measured against `stdlib/logic.tur`'s real persistent `Subst` (an `SBind` chain
walked by `logic-walk`), an indexed trailed substitution wins at every size, with
no crossover:

| bindings | persistent ns/op | trailed ns/op | speedup |
|---:|---:|---:|---:|
| 1 | 209.5 | 18.7 | 11.2x |
| 8 | 194.1 | 11.5 | 16.8x |
| 64 | 369.9 | 20.7 | 17.9x |
| 512 | 782.2 | 22.7 | 34.4x |

The expectation going in was that a persistent list would win at small `n` -- it
is O(1) to extend and free to backtrack -- and lose only once its O(n) lookup
began to dominate. It never wins.

The asymmetry matters more than the constant. A search that backtracks usually
wants to discard *most* of what it did and keep a little: a solver drops the
trail above a backjump point but keeps the clause it learned. A persistent
snapshot restores everything or nothing. A trail lets you say which.

## The three opt-outs

Backtracking is asymmetric, so the design turns on being able to **opt a cell
out**, at three granularities. Pick the narrowest one that fits.

| granularity | mechanism | for |
|---|---|---|
| per cell, forever | `g-cell-new` | activity scores, statistics, a learned-clause store |
| per write | `with-untrailed` | one write inside an otherwise-trailed cell that must survive |
| per level | `bt-commit-to!` | promote this level's writes and drop the level |

`BtCell` and `GCell` are **distinct types**, so opting out is visible in the
signature of anything that touches the cell rather than being a flag someone
forgets to pass.

```turmeric
(let [g (g-cell-new 5)      ; never trailed
      c (bt-cell-new 1)     ; trailed
      m (bt-mark)]
  (g-set! g 6)
  (bt-set! c 2)
  (println (bt-depth))      ; 1 -- only the BtCell reached the trail
  (bt-undo-to! m)
  (println (g-get g))       ; 6 -- survived
  (println (bt-get c)))     ; 1 -- restored
```
```sweet-exp
let [g g-cell-new(5)      ; never trailed
      c bt-cell-new(1)     ; trailed
      m bt-mark()]
  g-set!(g 6)
  bt-set!(c 2)
  println(bt-depth())      ; 1 -- only the BtCell reached the trail
  bt-undo-to!(m)
  println(g-get(g))       ; 6 -- survived
  println(bt-get(c))      ; 1 -- restored
```

### `bt-commit-to!` escapes the whole scope, not one level

Committing means "promote to level 0". It discards the undo information rather
than merging it into the enclosing level, so a committed write survives **every**
enclosing `bt-scope` too. That is deliberate -- it is how a learned clause
outlives a backjump -- but it is the opposite of what "commit into the parent"
suggests to a Prolog ear.

## Scopes

`bt-scope` is the bracket form of `bt-mark` / `bt-undo-to!`. Prefer it: the
halves are easy to leave unpaired on an early return, and the bracket cannot be.

```turmeric
(bt-scope (fn [] (do (bt-set! c 99) (bt-get c))))   ; => 99, and c is restored
```
```sweet-exp
bt-scope
  (fn []
    (do
      bt-set!(c 99)
      bt-get(c)))   ; => 99, and c is restored
```

The **value** survives; the trailed **writes** do not. Returning something that
points into state the scope just undid is the caller's problem -- this is a
search primitive, not a memory-safe region.

Two caveats:

- **A panic inside the body is undone only if something catches it.** The
  restore is a `defer` (`bt-scope` is `(let [m (bt-mark)] (defer (bt-undo-to! m))
  (body))`), so a panic that an enclosing `catch-unwind` catches unwinds
  through it and the trail is back at the outer level when the catch returns;
  `with-untrailed` resumes trailing the same way. An uncaught panic aborts the
  process, where the undo is moot. This used to be a real caveat -- the undo
  was skipped on a caught panic, because a `defer` inside a generic function
  was dropped on the compiled path's unwind
  ([archived report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/defer-in-generic-hof-skipped-on-caught-panic.md));
  `tests/fixtures/bt-scope-panic-undo` pins the fixed behaviour on both back
  ends.
- **Free cells outside the scope that wrote them.** A trail entry still pointing
  at a freed cell dangles until the next undo, and the failure surfaces inside
  `bt-undo-to!` rather than at the free.

## Why a thousand writes cost one trail entry

Every cell carries the level at which it was last trailed. A write records only
when that stamp is below the current level, then bumps it -- so a cell written
repeatedly inside one level costs **one** entry, not one per write. This is what
makes the primitive affordable in a search loop, and the entry count is the only
place it is observable:

```turmeric
(let [c (bt-cell-new 0)
      m (bt-mark)]
  (spin c 500)              ; 500 writes
  (println (bt-depth))      ; 1, not 500
  (bt-undo-to! m))
```
```sweet-exp
let [c bt-cell-new(0)
      m bt-mark()]
  spin(c 500)              ; 500 writes
  println(bt-depth())      ; 1, not 500
  bt-undo-to!(m)
```

## Write-once cells

`bt-lvar-new` is the logic-variable shape: unbound until bound, bound once per
level, and undo returns it to unbound. `bt-set!` **refuses** a second binding
rather than overwriting -- binding twice is a caller bug, not a silent
overwrite -- and returns `false` when it does.

## Continuations and the trail

A continuation captures **control, not state**. Re-enter one and you resume a
computation whose trail levels may already be gone.

**The type checker prevents most of this outright.** `Mark`, `BtCell` and `GCell`
have no `Clone` instance, and `cloneable-shift` requires `Clone` on every
captured binding, so a multi-shot continuation **cannot close over a trail
handle**. That is a `TUR-E0014` at compile time.

Where a mark does get through -- laundered via a plain `:int`, or via inline-C --
a mark carries a *generation* as well as a level, so `bt-undo-to!` on a spent
mark returns `false` instead of unwinding whichever level now occupies that
index. Always check the result if the mark could be stale:

```turmeric
(let [m (bt-mark)]
  (bt-set! c 2)
  (println (if (bt-undo-to! m) 1 0))   ; 1 -- works
  (println (if (bt-undo-to! m) 1 0)))  ; 0 -- refused, and changed nothing
```
```sweet-exp
let [m bt-mark()]
  bt-set!(c 2)
  println(if(bt-undo-to!(m) 1 0))   ; 1 -- works
  println(if(bt-undo-to!(m) 1 0))  ; 0 -- refused, and changed nothing
```

Unwinding *past* a scope is fine: `escape`, an abortive `shift0`, or a panic
unwinds through and the undo runs like any scope-exit action.

**Serializing a continuation inside an open scope is refused.** The undo
information is process-local and does not travel with the blob, so the
serialization aborts naming the scope depth and the outstanding write count.
Serialize outside the enclosing scope, or `bt-commit-to!` the level first.

## The search driver

`stdlib/backtrack-dfs.tur` builds depth-first search on top of the trail. A
**goal** is `(fn [(fn [] bool)] bool)`: it receives a success continuation `k`
and calls it once per solution, with bindings live in the trailed cells for
exactly the duration of that call.

```
k returns:       true = keep searching     false = cut the whole search
a goal returns:  true = exhausted normally false = a stop is propagating out
```

Combinators: `dfs-succeed`, `dfs-fail`, `dfs-and`, `dfs-or`, `dfs-guard`,
`dfs-set`, `dfs-choose-int`, `dfs-solve`.

**Solutions escape by reification, not by reference.** When `k` runs, the answer
exists only as the current cell values; the moment the search backtracks past it,
they are undone. Whatever you want to keep, `k` must copy out -- into a vec, a
`GCell`, or by printing -- before returning. Returning a `BtCell` handle and
reading it later yields the restored pre-search value, by design.

### Which driver to reach for

Against `stdlib/backtrack.tur`'s list monad on N-queens, **the list monad is
4-11% ahead at N=4..7** -- the trail's 11-34x was against persistent-state
*lookup*, and enumeration is a different fight where the driver pays a fat
dispatch per combinator layer per node. What the driver buys is **scale and
reification**: the list path's packed-int board is structurally capped at N=7,
while the driver runs N=8/9/10 and can read any solution out of live cells.

So: list monad for small, fixed enumeration; the trailed driver when the state is
large, when you need to reify answers, or when the search is the inner loop of
something that also wants the asymmetric keep-what-you-learned behavior.

### Two spellings the checkers dictate

Worth knowing before extending the driver:

- The combinators use the `bt-mark` / `bt-undo-to!` **halves**, not the
  `bt-scope` bracket, because wrapping `(g1 k)` in a scope thunk *captures* `k`,
  and capturing a fat closure moves it (`TUR-E0005`). Calling is non-consuming;
  capture is not.
- Recursion is written pass-`k`-through (`dfs-choose-go`) rather than
  goal-returning. When the driver was written, a self-recursive call whose
  fn-typed result fed a `^fat` parameter hit a
  [codegen bug](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/self-recursive-fn-returning-call-into-fat-sink.md),
  since fixed (2026-08-27); the spelling stayed because it is also the one
  that does not capture `k`.

## The `Bt` capability

Every function in `stdlib/trail.tur` that mutates the trail carries `#fx{Bt}`:
the writes (`bt-set!`, `g-set!`), the marks (`bt-mark`, `bt-undo-to!`,
`bt-commit-to!`), the pause halves (`untrailed-begin`, `untrailed-end`,
`trail-reset!`), and both bracket forms (`bt-scope`, `with-untrailed`). In the
driver, `dfs-solve` and the internal `dfs-choose-go` carry it too; the goal
*constructors* (`dfs-set`, `dfs-choose-int`, ...) do not, because they only
build a closure and the trail is touched when `dfs-solve` runs it.

`Bt` is a `^capability` effect, like `#fx{FS}` or `#fx{Net}` (see the
[Effects System Guide](effects-system-guide.md#capability-effect-tags-capability)),
which means the row is **checked**, at two places:

- **Effect rows.** A capability propagates from a callee's declared row into
  the caller's inferred row, so a caller that declares its own row without
  `Bt` and reaches the trail fails with `TUR-E0009`. A caller with no row
  annotation is not checked at all -- the discipline is opt-in, exactly as for
  the other tags.

  ```turmeric
  (defn bind-var [c : BtCell v : int] #fx{Bt} : bool   ; OK
    (bt-set! c v))

  (defn bind-var-bad [c : BtCell v : int] #fx{} : bool  ; TUR-E0009
    (bt-set! c v))
  ```

- **Refinement purity.** The solver only congruence-collapses two calls of a
  function it has proven pure, and a declared non-empty row vetoes that proof
  whatever the body looks like. The veto is memoized with the verdict and is
  transitive: an unannotated function whose only work is calling a
  `#fx{Bt}` function is not proven pure either. The trail's own mutators were
  never at risk here -- they are inline-C, which the walk refuses on sight --
  so what the row buys is precision on the wrappers *around* them, and a
  declaration the walk would find already in place if it were ever widened.

`Bt` is declared in `trail.tur` itself, not in `effects.tur`, because
`trail.tur` is autoloaded and `effects.tur` is not, and an effect name nothing
declares resolves to *nothing*, silently. That is not hypothetical: the row sat
on the mutators for a month checking as `#fx{}` before the declaration existed.

## `bt-scope` is also a region, and `with-region` is only a region

On by default since 2026-09-05 (see the
[regions plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/regions-plan.md);
`TUR_REGIONS=0` turns it off for bisection), `bt-scope` does a second thing besides pushing a trail level: it opens an
arena **generation**, and everything allocated inside that the returned value
cannot reach is reclaimed in one rewind when the bracket exits. For a solver
that is exactly right -- one query is one generation -- and it is why
`dfs-solve` gets its reclamation for free.

But a trail level and a lifetime are independent things, so each combination
has its own spelling rather than one form meaning two:

| | region | no region |
|---|---|---|
| **trail level** | `bt-scope` | `bt-mark` / `bt-undo-to!` halves |
| **no trail level** | `with-region` | plain code |

`with-region` (`stdlib/region.tur`) is the lifetime-only bracket: same shape as
`bt-scope`, same reclamation, **no** mark, **no** undo, and therefore no
`#fx{Bt}` -- a `#fx{}`-declared function may call it. It is for a program that
builds a throwaway structure and has no backtracking at all, so it does not have
to reach for a search primitive to bound a lifetime. And the halves are the
trail-only corner: they are never a region boundary, so a caller that wants
undo without paying the escape check uses them.

With `TUR_REGIONS=0`, `with-region` is an identity call and `bt-scope` is just the
trail bracket described above; neither allocates any differently.

The escape check is not only about the returned value. A node built inside the
bracket and then `bt-set!` into a cell, pushed into a vec, or captured by a
closure that is stored outward is noted at the store and blocks the rewind
too, so a search that stashes part of its state outside the bracket keeps its
correctness and loses only the saving. See the regions section of the
[GC guide](gc-guide.md#what-is-not-gc-managed) for the full list of hooked
stores and the one thing they cannot see (user inline-C).

## Under `--interpret`

The whole surface works, and the interpreter calls the same
`src/runtime/trail.c` the compiled path does -- there is one trail, not two
that can drift. `tur --interpret`, `tur eval`, `tur repl` and the web REPL all
have it.

The exception is serializing a continuation. Under `--interpret`,
`tur_serial_cont_serialize` is an in-process deep copy rather than a byte codec,
so there is no blob to outlive the trail and nothing to refuse; the scope check
described above is a compiled-path behavior.

Bear in mind that the tree-walker retains roughly 4 KiB per trampolined step, so
an interpreted search is memory-bound well before the trail's cost matters. Use
it to explore, not to measure.

## See also

- [Backtracking Guide](backtracking-guide.md) -- the list-monad search surface.
- [Logic Programming Guide](logic-programming-guide.md) -- relational search and `Subst`.
- [Delimited Control Operators Guide](delimited-control-operators-guide.md) -- `shift`/`reset`, `call/cc*`.
- [Solver extension plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/solver-extension-plan.md) -- design rationale, sections 3.2-3.5.
