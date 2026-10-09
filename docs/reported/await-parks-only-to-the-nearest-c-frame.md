# An await below a direct-style call or a non-tail resume parks only part of the async body

**Narrowed 2026-10-09 (fourth pass): a function that both performs an effect
and awaits a future compiles.** Every ordering -- perform then await, await
then perform, either under the function's own `handle` -- was refused
outright at compile time ("this effect operation has no lowering here"),
since neither continuation predicate admitted the other's control op; see
"perform and await in one function" under "Fixed 2026-10-09". A loop whose
turn performs and awaits inside ONE function (the recursive call in the
await's continuation) is still refused, by the recursive-await design; put
the recursion in the caller.

**Narrowed 2026-10-09 (third pass): the struct-capturing shapes compile, and
no silent wrong answer is left.** `fn_sig_ok` now admits a capturing lambda
that may await even when it is not threadable, and a concrete aggregate
capture (no type variable in it). The report's remaining repro prints 2428,
and the same lambda as the async body itself prints 605. For any function
that still stays off the CPS path between an async body and its await (an
`export-as` function, which keeps its C ABI), the await now refuses to park
and aborts with a diagnostic instead of losing the body (see "Refused at run
time" at the end). What is left is that refusal for such programs; `tur
--interpret` runs them.

**Narrowed 2026-10-09: both shapes below are fixed, and so are two more found
on the way; what is left is a may-await function the CPS backend evicts for
some other reason.** See "Fixed 2026-10-09" at the end for what changed and
the shape that still gives a wrong answer.

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

## Fixed 2026-10-09

- **Shape 1.** `param_name_clashes_cps` no longer evicts a function that may
  await (`cps_fn_may_await`) for a parameter named `k`, nor for the defensive
  `t<N>` rule. Both rules stay for every other function: lifting the `k` rule
  outright still leaks `saffron-lambda-arg-env-freed` and
  `sum-closure-payload-dropped` (re-measured today).
- **Through a fn parameter.** `(apply3 level3 x)`, with the awaiting `level3`
  passed to `apply3 [f : (fn [int] int) x : int]`, printed 604 for 2428 once
  the callers had a `k`: `may_await` followed named callees only, so the
  caller passing the awaiter was not marked and stayed evicted.
  `cps_expr_awaits` (`src/passes/cps.c`) now counts a call that passes an
  awaiting function -- by name, or a lambda whose body awaits -- as one that
  may await, since its callee may call it.
- **Shape 2** is fixed by
  [effect-nontail-resume-under-outer-handler-splits-the-capture](../archive/effect-nontail-resume-under-outer-handler-splits-the-capture.md):
  a non-tail resume in the case's own body runs into the chain, so the await
  parks the whole body: 105, and no overwritten park.

Pinned by `tests/fixtures/await-below-evicted-caller-or-nontail-resume`
(leak-checked): the `k` callers, `t<N>` callers, a named and a capturing
awaiter through a fn parameter, and shape 2.

### perform and await in one function (fourth pass)

`(let [t (perform (Tick))] (let [r (await (async (fn [] : int (+ k 1))))]
(+ t r)))` was refused: `TUR_TRACE_CORE=1` names the `perform` (kind 10),
whose continuation predicate (`perform_cont_reset_ok`,
`src/compiler/emit_cps_ir.c`) admitted a nested `perform` but no `await`;
the other order named the `await` (kind 11), whose predicate
(`await_cont_reset_ok`) admitted a further `await` but no `perform`. Both
continuations lift as the same resume-frame (`LH_RESUME_CONT`) threading a
run-time `__kont`, and the frame body is emitted by the one term
dispatcher, so admission was all that was missing:

- a perform continuation admits an `await` whose own continuation passes
  the await predicate -- the frame's `__kont` is the reinstalled-handler
  tail, and the await's shift to the root prompt captures past the frame
  into it, so a parked continuation carries the handler (the spine
  unification of [cps-await-cont-baked-env](../archive/cps-await-cont-baked-env.md));
- an await continuation admits a `perform` whose arguments are slot atoms
  and whose own continuation passes the await predicate -- the frame's
  `next` is the actual enclosing chain (`dk_frame_resume_borrow`), so
  `dk_perform` walks it to the handler, in place or in the copy a park
  resumes.

The await predicate still rejects every tail call, so no recursion enters
an await continuation through a perform either: a loop whose turn performs
and awaits must keep its recursive call outside the awaiting function.

Pinned by `tests/fixtures/cps-perform-and-await-ready` (ready futures, so
`tur --interpret` runs it and agrees: each order, both under a handle, a
nil-result effect between, a branch, a 100-turn loop, a counting handler)
and `cps-perform-and-await-parking` (pending futures driven from inline C:
perform-park-perform-park under a handler inside the async body, park then
perform under a counting handler, a 50-turn loop; compiled only).

### Still open

Any other reason a may-await function is evicted. A capturing lambda whose
capture is not a primitive is one (`fn_sig_ok`'s closure-capture switch):

```turmeric
(defstruct P [a : int b : int])
(defn level2 [m : int] : int
  (let [p (P m 0)]
    (* 2 (apply3 (fn [x : int] : int (level3 (+ x (.b p)))) (+ (.a p) 1)))))
```

prints 604 for 2428 in the shape-1 program. Fix direction 2 below -- refuse
an evicted may-await function at compile time -- is what would close the
class; it would also turn programs that only ever await ready futures (no
park, `__tur_await_ready`) from working into refused, so it wants a
diagnostic, not a silent eviction rule.

### Refused at run time (2026-10-09)

A pending await parks by shifting to the NEAREST root prompt. The runtime
now knows which root that should be. Each inline async spawn
(`tur_async_fiber`, `_closure`, `_via`) records the entry depth of the
body's own root in `tur_async_body_depth`; a resumed park records its own.
`__tur_await_body` refuses a park at any other depth: it prints `await: a
pending future would park below a direct-style call ...` and aborts, rather
than parking the part below and letting the frames above carry on with a
dummy 0.

That catches an evicted function anywhere BETWEEN the body and the awaiter.
It cannot catch an evicted async thunk ITSELF: a direct-style body has no
root of its own, so its awaiter's entry root sits at exactly the expected
depth. The emitter covers that case. `emit_async_direct_body`
(`src/compiler/emit_expr.c`) sets `tur_async_direct_body` just before
spawning a named thunk the CPS backend did not take (`emit_cps_ir_emits_binding`),
and the spawn then expects no park at all. A thunk the emitter cannot name
(an arbitrary fn value) keeps the depth check alone.

Pinned by `tests/fixtures/await-below-exported-function-refused`: an
`export-as` function between the body and the awaiter, which expects the
abort. The two struct-capture shapes were refusal fixtures for an hour and
became passing ones once the lambdas were admitted (below):
`await-below-struct-capturing-lambda` (2428, leak-checked) and
`await-in-struct-capturing-async-body` (605; its async env box leaks,
pre-existing,
[async-capturing-body-env-never-freed](async-capturing-body-env-never-freed.md)).
The 83 async/await fixtures and the JIT set show no false refusal. The
preamble change regenerated the split runtime and moved 159 `expected.c`
snapshots.

**The struct-capture shapes, admitted.** Two `fn_sig_ok` rules kept them
direct-style:
- A capturing lambda went to the CPS backend only when threadable. One that
  may await (`cps_fn_may_await`) is admitted too: off the CPS path its await
  parks at the wrong root, so the direct path is the one that cannot run it.
- The closure-capture switch took primitives only. A concrete aggregate
  capture is admitted too: a `defstruct` / `defdata` type with no type
  parameter and no type variable. The switch's stated hazard is a
  spec-suffixed env layout, but that belongs to a monomorph clone of the
  lambda, and `mono_sig_ok` never CPS-emits one.

The fixture suite, the leak-check harness, the JIT closure/async set and
fnsan show no regression.

Still open: refusing at run time is the honest floor, not the fix. Each
eviction reason an awaiter can still hit wants its own admission or a
compile-time diagnostic: an `export-as` function (its C ABI is fixed, so
that one stays a refusal), a generic or rank-2 capture, a poly-fn parameter.
A compile-time version of the refusal (fix direction 2) would also refuse
programs that only ever await ready futures.
