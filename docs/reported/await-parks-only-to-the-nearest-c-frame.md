# An await below a direct-style call or a non-tail resume parks only part of the async body

**Narrowed 2026-10-14 (seventh pass): the shapes left open by the sixth pass
park.** An `export-as` function, a lambda inside a generic capturing a `T`
(at `bool`, `int`, `float`, `cstr`, a struct, and in a constrained generic),
and a rank-2 call with a float or string position all print the right answer
now. What is left: a CONSTRAINED rank-2 call, and a rank-2 call with a
computed argument and a float or pointer position. Both warn and are refused
at run time. See "Seventh pass" at the end.

**Narrowed 2026-10-14 (sixth pass): rank-2 awaiters run, and the warning is
exact.** An awaiter reached through a rank-2 (`forall`) parameter -- called,
handed on, or captured into a lambda -- now parks the whole body (1213 for
1213 in each shape). `TUR-W0044` follows an awaiter through function values,
fires only for functions reachable from an `async` body, and names an
unthreaded rank-2 call. What is left: a capturing lambda inside a generic, a
rank-2 call with a float, string or pointer position or a constraint, and
`export-as`. Each
draws the warning and is refused at run time. See "Rank-2 admitted, warning
made exact" at the end.

**Narrowed 2026-10-14 (fifth pass): the refusal is announced at compile
time.** A function that may await and that the CPS backend evicts now draws
`TUR-W0044` at its definition, naming the function and why it is off the
backend (`export-as`, a signature or capture shape, an inline-C body, a form
with no lowering). The run-time refusal stays as the floor; the warning is
what fix direction 2 asked for, as a warning rather than an error because a
program whose awaits only ever meet ready futures runs unchanged. See
"Warned at compile time" at the end.

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

### Warned at compile time (2026-10-14)

The eviction site (`emit_cps_ir_try_fn_impl`, `src/compiler/emit_cps_ir.c`)
already classified every colored fallback for `TUR_TRACE_EVICT`. It now also
emits `TUR-W0044` once per function that `cps_fn_may_await` and is evicted,
at the function's definition span, with the category's reason in prose:

```
input.tur:43:33: warning [TUR-W0044]: `level2` may await but is compiled
direct-style: it is exported under a fixed C name (export-as), which keeps
the C ABI. Inside an async body, a pending future awaited below it would
park at this function's root instead of the body's, so the program refuses
the park at run time (`await: a pending future would park below a
direct-style call`); a ready future is unaffected. Move the await, or the
call to the awaiting function, out of this function (SIG-EXPORT)
```

`tur explain TUR-W0044` has the long form. Two evictions are left out on
purpose, both measured as false positives on the await fixture set:

- `main`: it is never below an async body, so an await in it has no body to
  lose (it fired on five fixtures whose `main` awaits a spawned future).
- A rank-2 adapter `__poly_<N>` (`make_poly_wrapper`): only its base entry
  is refused at the site; the fat box a colored caller dispatches through
  carries the adapter's own `__poly_<N>__cps` twin, so a park never crosses
  the base entry (`await-through-fn-value` parks through three of them and
  prints the right answers).

Across every fixture that awaits, the warning fires on exactly two: the
`export-as` function of `await-below-exported-function-refused` (now in its
`expected.stderr`) and `worker` in `cps-oracle-async-placement`, which
awaits a future it has just spawned -- ready on the compiled path, so the
program is correct and the warning is the "ready future is unaffected"
case. `unexpected.stderr` pins its absence on the five pending-future
fixtures that park correctly: `await-through-fn-value`,
`await-below-evicted-caller-or-nontail-resume`,
`await-below-struct-capturing-lambda`,
`await-in-struct-capturing-async-body`, `cps-perform-and-await-parking`.

Still open at the time: each eviction reason an awaiter can still hit wanted
its own admission (a generic or rank-2 capture, a poly-fn parameter), and the
warning did not know whether the evicted function was reachable from an
async body. Both are taken up in the next section.

### Rank-2 admitted, warning made exact (2026-10-14, sixth pass)

Five probes on the shape-1 driver (`level3` awaits a pending future 300
times; the body is `7 + 2 * (3 + level3 ...)`, so 1213). Before:

| Shape | Before | After |
|---|---|---|
| awaiter called through a rank-2 param | refused, no warning | 1213 |
| rank-2 param handed on to another | refused, no warning | 1213 |
| lambda capturing the rank-2 param | refused, no warning | 1213 |
| the same through a `bool` position | refused, no warning | 1213 |
| generic awaiter (no lambda) | 1213 | 1213 |
| lambda capturing a generic's `T` | refused, warned | refused, warned |
| rank-2 call with a float position | refused, no warning | refused, warned |

What changed:

- **may_await follows function values into parameters.**
  `Binding.may_hold_awaiter` marks a parameter some call hands an awaiting
  function (named, a lambda, or another such parameter); calling, handing
  on, or capturing it then may await (`src/passes/cps.c`, the may_await
  fixpoint). On the binding, so a lambda capturing the parameter sees it.
  Before this, `r2-pass`/`r2-cap` read as non-awaiting: evicted for reasons
  an awaiter is exempt from, and silent.
- **A rank-2 call threads.** `fncps_param_call_ok` refused every `forall`
  parameter as "a different, wider ABI". An unconstrained one called on int
  and bool positions has the slot's ABI: the elaborator's wrapper
  `__poly_N(void *, int64_t...)` and its twin `__poly_N__cps(void *,
  int64_t..., DK *)`. It now dispatches through `fn_cps` like a plain `fn`
  parameter (`fncps_forall_call_ok`, `src/passes/cps_ir.c`); for those kinds
  the `.fn` fallback casts exactly as the delegated call did. A string or
  pointer result is read back through the int64 carrier by the direct path,
  so a fallback cast to a pointer-returning type trapped under
  `-fsanitize=function` (`hrt-poly-call-cstr-result`); those kinds stay
  delegated.
- **A rank-2 param handed on is passed, not spilled.** `(r2-apply f x)` wraps
  `f` in an identity `EX_POLY_WRAP`, which atomized into a `void *` temp that
  cannot hold the fat struct. The translator passes the parameter's own atom
  (`cps_ir_rank2_handed_on`) and `call_args_ok` admits it
  (`fat_param_handed_on`); `fn_sig_ok` admits a fat parameter that may hold
  an awaiter.
- **A rank-2 capture.** `fn_sig_ok` admits a poly-fn capture that may hold an
  awaiter. The env-struct helper in `emit_expr.c` spelled it `void *`; it
  now has the `tur_poly_fn_t` rule its lockstep twin in `emit_fns.c` has.
- **Registry.** An address-taken function that may await registers its
  direct -> `__cps` row even when the threadability tiers did not admit it
  (a value handed on or captured reads as an escape there). The adapter
  twin and the fat dispatch look the awaiter up there; a miss called its
  direct entry.

The warning, now exact:

- **Reachability.** `FnDef.async_reachable`, from every `(async thunk)` and
  closed over calls and references; a thunk that is not a known function
  makes everything reachable. Only reachable functions warn, matching the
  run-time refusal's own `tur_async_body_depth` test. `worker` in
  `cps-oracle-async-placement` no longer warns.
- **Rank-2 calls.** A CPS-emitted function with an unthreaded call through
  an awaiter-holding rank-2 parameter warns at the call, naming the
  parameter and suggesting a plain `fn`-typed parameter (which threads
  floats: the float probe prints 607 that way).
- A lifted lambda is named "this lambda" (the span points at its `fn`).

Pinned by `await-through-rank2-param` (four rank-2 shapes, leak-checked,
`unexpected.stderr` TUR-W0044) and `await-below-unthreaded-frame-refused`
(the generic capture and the float rank-2 call: three warnings, then the
refusal).

**Still open:**

- *A capturing lambda inside a generic.* `--dump-cps-mono` calls the
  generic's specialization admissible, but none is emitted: the program runs
  the generic's erased-carrier base and one base lambda that carries `T` as
  a word. The base refuses type-variable parameters and captures by design
  (`fn_carrier_param_ok`: a carrier `__cps` signature would be hit by
  concrete CPS callers with by-value struct arguments). Fixing it means
  CPS-emitting closure spec clones (`mono_sig_ok` refuses every closure
  clone; its env unpack would need `env_name_override`) or specializing such
  generics at all. A generic awaiter without a lambda works.
- *A rank-2 call with a position that is not an int or bool, or a
  constrained forall.* The slot carries a float at its own type, which the
  wrapper's `int64_t` parameter does not declare; a string or pointer result
  goes through the carrier on the direct path; a constrained forall adds
  dictionary slots. Warned at the call; a plain `fn` parameter is the
  workaround.
- *`export-as`* stays a refusal: its C ABI is fixed.

### Seventh pass (2026-10-14): export-as, generic captures, every rank-2 kind

| Shape | Sixth pass | Now |
|---|---|---|
| awaiter below an `export-as` function | refused, warned | 1211 |
| lambda in a generic capturing `T`, at `bool` / `float` / `cstr` / struct | refused, warned | 1213 |
| the same at `int` | refused, warned | 1213 |
| the same in a constrained generic (`[(Eq T)]`) | refused, warned | 1213 |
| rank-2 call with a float position | refused, warned | 607 (7.1 + 600) |
| rank-2 call with a string result | refused, no warning | right answer |
| rank-2 call with a computed int argument | 1213 | 1213 |
| rank-2 call with a computed float argument | refused, warned | refused, warned |
| constrained rank-2 call | refused, warned | refused, warned |

- **`export-as`.** The classifier perm-routed every genuine export. A
  may-await one is now a candidate: internal callers thread into its
  `__cps` twin, and the exported symbol is the entry wrapper, whose linkage
  rule mirrors the direct emitter's `needs_static` line for line (neither
  reads `c_export_name`) and whose name and signature are the direct
  definition's. The FFI shim calls that symbol. A C caller still enters at a
  fresh root, as before.
- **The rank-2 fallback.** The threaded slot was never the problem for a
  float or a pointer: the slot call packs every argument as a word and the
  value's twin unpacks it. The `.fn` fallback was: `.fn` holds whatever the
  elaborator built (an int64 carrier adapter read back through
  `(const char *)(intptr_t)`, or a typed wrapper), and casting it to the call
  site's types trapped under `-fsanitize=function`
  (`hrt-poly-call-cstr-result`). With atomic arguments the fallback is now
  the direct emitter's own rendering of the call (`fncps_fallback_value`), so
  every slot kind threads. A computed argument cannot be re-rendered, so it
  keeps the slot cast, which matches the delegated one on int and bool only.
- **Generic captures, three parts.** The sixth pass misread this one: the
  generic's specialization IS on the backend; the frame in the way was the
  lambda. (1) It is emitted once, erased, its env holding the `T` capture as
  the int64 carrier, and `fn_sig_ok` refused a `TY_TYVAR` capture, so it had
  no `__cps` twin and the fat dispatch missed in the registry. A lambda that
  may await may now capture a bare `T`; its body is classified and rendered
  with `T` read as the erased carrier word, exactly as its direct thunk reads
  it (`g_cps_erased_base`, scoped to that function, so the template gates
  still refuse a bare type variable elsewhere). (2) At `int` no monomorph was
  minted at all: `expr_reaches_effect_op`, which decides whether a generic
  needs one for an effect it reaches only through calls, did not follow a
  function value passed as an argument. It does now (`fnvalue_arg_reaches_
  effect_op`). (3) A constrained generic never took that route ("keep the
  strict syntactic seed, as the `unsafe` lesson recommends"); the lesson was
  about a `handle`, which this walk does not count, so a constrained,
  non-instance generic takes it too. 650 typeclass / constraint / effect /
  generic fixtures pass.
- **Warning.** A generic base whose every call reaches a specialization (no
  carrier call, never a value) no longer warns: the specializations are what
  run (`generic_base_never_runs`).

Found on the way, not about `await`:
[erased-capture-aggregate-box-never-freed](erased-capture-aggregate-box-never-freed.md)
-- the struct instantiation boxes `v` into the erased env field and the shared
drop glue never frees it (16 B per closure, on the direct path too).

Pinned by `await-below-exported-function` (was `...-refused`),
`await-in-generic-capturing-lambda` (bool/int/float/cstr/constrained),
`await-in-generic-capturing-lambda-struct` (`known-leak`), the float, string
and computed-int shapes added to `await-through-rank2-param`, and
`await-below-unthreaded-frame-refused`, now the two remaining shapes. All
leak-checked except the refusal; fnsan clean over 208 forall / await / async /
generic fixtures.

**Still open:**

- *A constrained rank-2 call* (`(f x 0)` through `f (forall [a] [(Eq a)]
  ...)`): the dictionary argument makes the call non-atomic, so the
  translator reports "indirect call (non-atomic args)" and the function
  evicts. Threading it means teaching the fn_cps slot the dictionary slots
  (`make_poly_wrapper_ex`'s `n_lead_ignore`).
- *A rank-2 call with a computed argument and a float or pointer position*:
  `let`-binding the argument threads it; the fix would be re-rendering the
  call over the translator's own argument temps.
- Any other eviction an awaiter can hit (an inline-C body, a signature shape
  the backend does not thread) stays the run-time refusal, warned.
