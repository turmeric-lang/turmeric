# An await below a direct-style call or a non-tail resume parks only part of the async body

**Severity: medium-high (silent wrong answer), in two narrow shapes.** A
pending `await` parks by shifting to the nearest root prompt. That is the
async body's root only while every frame between the await and the body is
on the DK chain. A C-stack frame in between -- a direct-style call, a non-tail
`resume` -- puts a nearer root (or the end of a resumed copy) in the way, so
the park captures only the part below it, and the part above carries on at
once with `0`. Both shapes give the same wrong answers before and after the
2026-10-08 async reclamation work; found while testing it.

## Shape 1: a caller the CPS backend evicts

```turmeric
(defn level3 [k : int] : int (+ k (sum-values 0 300 0)))   ; sum-values awaits
(defn level2 [k : int] : int (* 2 (level3 (+ k 1))))
(defn level1 [k : int] : int (+ 7 (level2 (+ k 1))))
(defn body [] : int (+ (level1 1) (level1 2)))
```

Run as `(async body)` with `sum-values` awaiting a pending future each turn
(the every-turn driver of `tests/fixtures/async-repeated-park-frees-each-turn`),
this prints **600**; the answer is **2428**. Rename the parameter `k` to `m`
and it prints 2428.

`param_name_clashes_cps` (`src/compiler/emit_cps_ir.c:3060`) keeps every
function with a parameter named `k` off the CPS path (`TUR_TRACE_EVICT=1`:
`SIG-REJECT eff=0 level3`), may-await or not. Its comment says `k` no longer
collides with anything -- the continuation parameter has been `__kont` for a
long time -- and that the rule is kept because lifting it moved Saffron
self-applying lambdas onto a path that leaked their env. An evicted function
calls the awaiting `sum-values` through its direct entry (`/* cps->direct */`),
so the await's shift stops at that entry's root: `sum-values`'s entry parks,
returns 0, and `level3` adds `k` to it and returns.

The same holds for any other reason a may-await function is evicted (a
by-value aggregate parameter, a poly-fn capture, ...); `k` is just the easiest
to hit.

## Shape 2: an await inside a non-tail resume

```turmeric
(defn after-pick [c : int] : int (+ c (next-value)))     ; next-value awaits
(defn body [] : int
  (handle (after-pick (pick))
    (Choose [] ^multishot kk) (+ (resume kk 1) (resume kk 100))))
```

Prints **102**; the answer is **105**. `(resume kk 1)` runs a copy of the
continuation from inside the case, on the C stack (`dk_invoke`). The await
in it shifts to the end of that copy, not the body's root, so the case gets 0
back at once, runs `(resume kk 100)`, whose await parks again on the same
pending future -- overwriting the first park, which leaks (536 B under ASan)
-- and the one resume that is left delivers 100 + 2.

## Fix directions

1. For shape 1, exempt a function that may await (`cps_fn_may_await`) from
   the `k` rule; the Saffron leak it guards is about self-applying lambdas.
2. For both, refuse rather than miscompile: a may-await function that the
   CPS backend evicts, or an await reachable from a non-tail `resume`, could
   be a compile-time error naming the frame in the way -- as a `perform` the
   CPS backend cannot lower already is ("this effect operation has no
   lowering here") -- instead of a park that silently captures half the
   body.
