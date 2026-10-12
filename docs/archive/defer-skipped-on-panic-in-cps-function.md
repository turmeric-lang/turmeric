# `defer` in a CPS-converted function does not run on a caught panic

**Severity: medium (cleanup skipped: leaks, and anything else a defer was
guarding).** Found 2026-10-10 adding `mw-recover` support to tourist
(tourist-on-stdlib-httpd-plan H4). In a function compiled directly, a `defer`
fires when a panic passes through (`tur_frame_fire_lifo` before each
`if (tur_panicking) return`). When the function is CPS-converted (it performs
an effect, or calls something that does), its `__cps` body runs the deferred
form inline at the normal exit only; the panic path returns without it.

## Repro

```turmeric
(defn note [s : cstr] : int
  ```c printf("%s\n", s); fflush(stdout); return 0; ```)

(defn boom [x : int] : int
  (if (> x 0) (panic "boom") x))

(defn direct [x : int] : int
  (defer (note "direct: defer ran"))
  (boom x))

(defeffect Ask [] : int)

(defn cps [x : int] : int
  (defer (note "cps: defer ran"))
  (+ (perform (Ask)) (boom x)))

(defn main [] : int
  (let [r1 (catch-unwind (fn [] : int (direct 1)))]
    (note (if (err? r1) "direct: caught" "direct: no panic")))
  (let [r2 (catch-unwind (fn [] : int (handle (cps 1) (Ask [] k) (resume k 1))))]
    (note (if (err? r2) "cps: caught" "cps: no panic")))
  0)
```

```
direct: defer ran
direct: caught
cps: caught            <- "cps: defer ran" is missing
```

## Where it bit

`tourist/app`'s `__tourist-handler` is CPS-converted (it reaches user closures
through the `use!` chain), so `(defer (tourist-ctx-free ctx))` there leaked the
request's ctx, its attrs and its route captures on every panicking route. The
same defer in spices/httpd's (direct) `srv-serve-view` works.

## Fix directions

The CPS emitter should register the defer frame as the direct emitter does, and
fire it on every `if (tur_panicking) return` it emits. The archived
`defer-in-generic-hof-skipped-on-caught-panic` is the same family.

## Workaround in use

tourist calls `__tourist-run` from an inline-C guard (`__tourist-run-guarded`)
that frees the ctx when `tur_panicking` is set after the call.

## Resolved 2026-10-11

Both halves fixed; fixture `tests/fixtures/defer-cps-caught-panic` carries
the repro above and seven more shapes (the panic before the perform, in the
perform's continuation, in a colored non-tail callee, in the handler itself
with no resume, two defers LIFO with a third declared after the panic point,
a defer reading a local from a continuation that only delivers to a join,
and the no-panic exit).

**CPS half.** The P5b lowering (src/passes/cps_ir.c) already threads each
`(defer D)` body into the block's continuation, which is why the normal exit
ran it; the panic path left the C function at an `if (tur_panicking)
return` before that continuation ran, and when the panic was in a callee
that never resumed, the continuation frame never ran at all. The chain now
also says where a defer is pending: push / pop marker nodes (`CTerm
.defer_push` / `.defer_pop`, inert `CT_LETVAL`s of the unit atom) bracket
the region from the defer's declaration to its threaded fire, exactly as the
direct emitter's frame registers a defer when control reaches it. The CPS
emitter (src/compiler/emit_cps_ir.c) keeps a pending stack along its walk
and:

- fires it, LIFO, inside every panic-signal return it writes -- its own
  `cps_panic_check` and the direct emitter's checks inside delegated
  expressions, through a hook in `emit_panic_signal_return`
  (`g_cps_panic_fire`) -- with `tur_panicking` cleared around the fires;
- gives every lifted continuation frame (perform / join / reset / handle /
  await continuations) an unwinder helper `<name>_unw(env)` that fires the
  same bodies from the frame's env, attached with `__dk_with_unwind`. The
  DK runtime walks the chain a panic abandons and fires each unwinder once,
  innermost first (`__dk_panic_abandon`): from `dk_run_impl` when a frame or
  a shift body returns by signal, from `dk_perform` when a handler case
  returns by signal, and from every panic return the CPS emitter writes in a
  body that holds its chain (`__dk_panic_abandon(__kont)` ahead of the
  fires). A frame the driver runs has its unwinder cleared first (it fires
  its own), and a copy of a frame never inherits one -- so the originals a
  perform copies for its handler, which never run, are never fired: a
  resumed continuation's defers run exactly once, in the copy, and a frame
  abandoned by something other than a panic is never walked.
- keeps each defer owned by exactly one place: a frame that exists before
  the code being emitted runs (a join, reset or handle continuation created
  earlier in the body) owns the fires in its body, and nothing emitted after
  it fires them again (`g_pend_downstream`); a handler case, a shift body
  and a loop helper are callees of the chain and see no pending defers.
  The captures a helper needs for the fires ride its env
  (`collect_caps`); a defer whose variable the env cannot carry is skipped
  in that helper, as before.

**Direct half, found on the way.** On the panic path the direct emitter
fired the frame's defers with `tur_panicking` still set, so a defer body
that called a Turmeric function ran only that function's first statement --
its own per-call-site check returned after the first call. `(defer
(cleanup))` ran half of `cleanup`. The fire now runs with the signal cleared
and raises it again after (`emit_panic_signal_return`, src/compiler/
emit_expr.c); `tur_panic_in_progress` stays set, so a defer body that panics
is the double panic it always was.

**Not covered.** A continuation frame abandoned by something other than a
panic -- an abortive handler that never resumes, an escape -- still does not
run its defers, and a later panic does not run them late either (fixture
case 10); that is
[cps-defer-skipped-on-abortive-effect](cps-defer-skipped-on-abortive-effect.md) (since resolved).
A `defer` inside a `let` body
whose enclosing `do` also carries defers still evicts the function from the
CPS backend (`BODY-TAINT`), which predates this change: the block then has
no lowering for its `perform` at all, a hard error rather than a skipped
defer.
