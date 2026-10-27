# effect-handler-capture-loop: a while loop with an interior control op has no native CPS lowering

**RESOLVED** (2026-07-17) -- landed as `CT_LOOP`/`CT_CONTINUE`: an `EX_WHILE`
with an interior control op now lowers to a synthesized tail-recursive colored
`__cps` helper under `--enable=cps-tramp-resume`.  `run` emits `run__cps` (zero
`eff=1`) and prints `100`.  Implementation paper trail:
`docs/archive/history/cps-while-loop-with-interior-handle-no-native-lowering.md`.
Regression fixtures: `cps-tramp-resume-while-handle` (1507),
`cps-tramp-resume-while-handle-escape` (effect escapes outward -> 50),
`cps-tramp-resume-while-readset` (read-after-set -> guard evicts -> 10).  The
SUPERSEDING FINDING below is what shipped: NOT the `CT_LOOP`-as-same-function-join
of the DEEPER SCOPE section, but a real recursive `__cps` function (forced by
`emit_handle` lifting the handle continuation into a separate C function).

---


**Severity:** medium (blocks `effect-handler-capture-loop` from the CPS/DK backend under
`--enable=cps-tramp-resume`; correctness is fine -- it runs on the fiber via whole-body
delegation, output `100`). This is a REAL fiber-live fixture (performs+handles `Ask`), so it
is a genuine migration target, unlike the `Unsafe`-marker or session-thread cases.

## The fixture

```turmeric
(defeffect Ask [] : int)
(defn run [] : int
  (let [^mut i     0
        ^mut total 0]
    (while (< i 5)
      (let [cur i]
        (set! total
          (+ total
             (handle                       ; <-- a control op (handle/perform/resume)
               (perform (Ask))             ;     INSIDE the loop body, per iteration
               (Ask [] k)
               (resume k (* cur 10))))))
      (set! i (+ i 1)))
    total))
(println (run))                            ; => 100  (10*(0+1+2+3+4))
```

Each iteration installs a fresh `Ask` handler that closes over the per-iteration snapshot
`cur` and resumes with `cur*10`.

## Exact eviction reason (pinned)

- **flag OFF:** `run` is SIG-TAINT (tainted, evicted to the fiber; runs correct).
- **flag ON:** `run` is **BODY-UNSUPPORTED, `unsupported form: EX_WHILE`**. The CPS IR dump
  (`tur emit-c --enable=cps-tramp-resume --dump-cps`) is:

  ```
  cps-fn run [] k:cont<int> entry
    let i = 0
    let total = 0
    <unsupported: unsupported form: EX_WHILE>
  cps-end
  ```

The CPS translator emits a `CT_UNSUPPORTED` for the `EX_WHILE`. There is NO native
`EX_WHILE` lowering in the CPS transform (`src/passes/cps_ir.c`): `EX_WHILE` appears only in
HELPER walks (`IFC` / `safe_to_delegate` / `expr_has_unsafe_control` / `body_calls_binding`)
and the P5 whole-body-delegation predicate -- never in the `cps_tail` / `cps_bind` transform
that would lower it, so it falls through to `unsupported_form(...)`.

## What IS handled today (and why it is not enough)

- A **control-op-free** while loop CPS-emits fine: `safe_to_delegate` admits it and the whole
  loop rides the direct emitter as a delegated region (`CT_LETRAW`) inside `run`'s DK body.
  Verified: a `while` with no interior `handle`/`perform` does not evict.
- A **self-contained-handle** loop is kept CORRECT on the fiber by P5 whole-body delegation
  (task-15 "native EX_WHILE lowering" is actually this DELEGATION -- it hands the loop to the
  direct/fiber emitter, so `run` runs on the FIBER, not the DK). That is why the fixture
  passes, but it does NOT put `run` on the DK.

Minimal trigger (isolated): a `while` with a self-contained `handle` in its body evicts
`BODY-UNSUPPORTED EX_WHILE`; move the same `handle` BEFORE the loop and `run` CPS-emits.
So the blocker is precisely: **a control op (handle/perform/resume) INSIDE a `while` body**,
on the CPS-split path (flag-on, once the effect is de-tainted and `run` becomes a genuine DK
candidate rather than a tainted fiber fallback).

---

## DEEPER SCOPE (measured on branch `claude/effect-reopen-report-w2n5zh`, 2026-07-17)

**The original "Fix direction" below (`add an EX_WHILE case`) materially understates the
work.** An audit of `src/passes/cps_ir.c` + `src/passes/cps_ir.h` shows the CPS IR today has
NO representation for either half of this construct -- not just the loop, but the MUTATION it
carries -- and its join node cannot express a multi-variable loop. Native lowering is THREE
coupled foundational additions that must land together (no sound partial moves the fixture; a
transform that admits the shape but mishandles mutation would MISCOMPILE, not evict):

### Obstacle 1 -- the IR identifies a value by its source `Binding`; there is no mutation

`atom_of` (`cps_ir.c:307`) maps `EX_VAR` -> `CA_VAR{ .var = binding }` DIRECTLY. A value in
the CPS IR *is* its source `Binding` -- an implicit SSA assumption that holds for every form
EXCEPT `^mut` + `set!`. There is no binding->current-value map anywhere in the builder
(`CpsB`, `cps_ir.c:38`, has an arena, a counter, the return kont, and pap registrations --
nothing else). So `EX_SET` (`{ Binding *target; Expr *value; }`) has no transform case and
falls to `unsupported_form` (`cps_bind`/`cps_tail`), and a `^mut` read after a `set!` would
read the STALE binding. Native `set!` therefore requires a **mutable rebinding environment**
(source `Binding` -> current `CVar`) threaded through `cps_tail`/`cps_bind`, so `set! v e`
lowers `e` to a fresh `CVar` and updates the map, and every subsequent `^mut` read of `v`
resolves through the map. Crucially this map must thread THROUGH continuations: in the fixture
`(set! total (+ total (handle ...)))` -- the new value of `total` is produced INSIDE the
handle's continuation, so the rebind happens in a nested CPS term, not straight-line.

### Obstacle 2 -- `CT_LETCONT` is single-parameter; there is no multi-arg loop join

`CT_LETCONT` is `{ CVar j; CVar param; CTerm *jbody; CTerm *body; }` (`cps_ir.h:182`) -- ONE
param. The report's sketch (`CT_LETCONT loop(i, total)`) is not expressible: a loop carrying
two `^mut` vars needs a two-arg join. Options, both real work:
  (a) Extend `CT_LETCONT` (and the `KK_VAR` jump / `CT_APPCONT`) to N params. Touches the IR
      struct, every `CT_LETCONT` producer/consumer, `needs_heap_join`, `collect_caps`, and the
      emitter.
  (b) Pack the loop-carried vars into ONE aggregate value (a synthesized tuple/struct) so the
      single param suffices -- but there is no ad-hoc tuple type at CPS-translation time
      (types come from elaboration), and per-iteration pack/unpack (`make-struct`/`get-field`)
      are themselves delegated `CT_LETRAW` ops.

### Obstacle 3 -- the emitter has no self-recursive loop emission

`emit_heap_join` (`emit_cps_ir.c`) lowers a `KK_VAR` join as a value-transform DK FRAME
(`dk_frame`/`dk_frame_resume`) for a NON-tail cps->cps call result -- a one-shot downstream
frame, not a self-jump. A loop join re-enters ITSELF with new args; the natural emission is a
C loop (params as reassigned C locals, the back-edge as `continue`/`goto`) -- a NEW emitter
construct. It must compose with the interior `CT_HANDLE` (a fresh DK prompt installed and
fully resolved WITHIN each iteration -- the handle completes before the back-edge, so the loop
body returns to straight-line before re-entry) and stay flat under the E7 trampoline for a
large iteration count.

### Why there is no sound bounded sub-slice

The fixture's loop condition reads `i` and `set! i` mutates it, so even the counter needs
Obstacle 1; the two live-after vars (`i`, `total`) need Obstacle 2; and the interior handle
forbids delegating the loop body to the fiber (that is the whole point), so the loop must be
lowered natively AS A WHOLE -- Obstacle 3. None of the three moves the fixture alone, and a
transform that admits `EX_WHILE` without correct mutation SSA would silently miscompile every
flag-on mutating loop. So this is a coordinated foundational transform, not an incremental
admission widening.

### Obstacle 4 (THE crux) -- build order is continuation-first; mutation is execution-order

Measured: `cps_tail`'s `EX_DO` (`cps_ir.c:2650`) lowers items RIGHT-TO-LEFT -- it builds the
last item's term first (`rest = cps_tail(items[n-1], kont)`) then wraps earlier items
(`rest = cps_bind(items[i], _, rest)` for `i = n-2 .. 0`).  So a naive mutable `Binding ->
CVar` map in `CpsB` -- the report's original "rebinding environment" -- is **UNSOUND**: it
would be updated by later-EXECUTION `set!`s before earlier ones are lowered, so a `^mut` read
would resolve to the wrong version.  General correctness needs either an SSA-renaming PRE-PASS
(compute versions in a forward walk, then run the mutation-free CPS transform) or an explicit
forward-threaded environment (a structural change to `cps_tail`/`cps_bind`).

### The sound, contained algorithm (sidesteps general SSA -- covers the counter-loop idiom)

The build-order hazard only bites when a `set!`'s value reads a mut var that ANOTHER `set!`
wrote earlier in the SAME iteration.  Restrict to loops where every in-body read of a
loop-carried var resolves to the loop-ENTRY (loop-param) version -- i.e. no `set!` value
observes another `set!`'s result within the iteration.  Then reads resolve to the fixed
loop-param CVars regardless of build order, and no forward env / SSA pass is needed.  This
covers the fizzbuzz/counter idiom and the fixture (its `set! total` and `set! i` each read
only entry versions; `cur = i` reads the entry `i`).  Concretely:

1. **A dedicated `CT_LOOP` IR node** (NOT extending single-param `CT_LETCONT`, which would
   disturb every existing heap-join).  `{ CVar *params; uint32_t n; CTerm *body; }`, plus a
   `CT_CONTINUE { CAtom *args; uint32_t n; }` back-edge.  Body = `CT_IF(cond, <iter>, <exit>)`;
   `<iter>` ends in `CT_CONTINUE(next values)`; `<exit>` delivers the live-after vars to the
   enclosing continuation.
2. **Loop-scoped read resolution.**  Only while lowering a `CT_LOOP` body, `atomize`
   (`cps_ir.c:1663`, already takes `b`) resolves a loop-carried `^mut` read to the loop-param
   CVar; `EX_SET` records the var's exit CVar in a loop-local table (no global `CpsB` map).
   `EX_SET` outside a loop, or a value that reads an already-set var this iteration -> EVICT
   (conservative, sound).
3. **`CT_LOOP` emission** (`emit_cps_ir.c`): a C `while (1) { <body> }` with the params as C
   locals; `CT_CONTINUE` assigns them and `continue`s; the exit delivers.  The interior
   `CT_HANDLE` emits its DK chain per iteration (it installs + resolves within the iteration,
   so it returns to straight-line before the back-edge); verify flat under the E7 trampoline.

### Ruled out -- "just emit a C `while` with C mutation and the handle inline"

Tempting (C has loops + mutation, so `set!` -> C assignment, `while` -> C loop, `^mut` read ->
C local), but UNSOUND for this construct: `emit_handle` lifts the handle's continuation
(`handle.body`) as an `LH_RESET_CONT` **DK frame** (`emit_cps_ir.c:5445`), so the interior
handle delivers its result through a DK continuation chain, NOT by returning to inline C.  The
loop back-edge (`total += h; i++; iterate`) is part of that continuation, so it must live in
the DK world -- a C `continue` cannot re-enter a DK continuation.  Hence the loop MUST be a
continuation-based DK loop (`CT_LOOP` + multi-arg join), not a C loop wrapping the handle.

### Recommended build order (all gated on the flag; each verified against suite + flag-on sweep)

1. `CT_LOOP`/`CT_CONTINUE` IR + emitter (additive; nothing produces them yet).
2. The `EX_WHILE` transform + loop-scoped read/`set!` handling + the conservative guard, so the
   fixture emits `run__cps` (zero `eff=1`) and prints `100`.  Companion fixtures: interior
   self-contained handle -> DK; effect ESCAPES outward -> lower-or-cleanly-evict (no
   miscompile); no-control-op while -> stays delegated, unchanged; a `set!`-reads-earlier-`set!`
   loop -> cleanly EVICTS (guard holds).

### Gate / verification (unchanged from below, plus the mutation guards)

Default suite (`bash tests/run.sh`, 12-min timeout) green; flag-off byte-identical (gate the
whole transform on the flag-on candidate path). Flag-on soundness sweep clean (build+run every
effect fixture under `--enable=cps-tramp-resume`, diff vs baseline) -- this is the check that
catches a mutation miscompile. Add a `cps-tramp-resume-while-handle` regression fixture that
asserts `run` emits `run__cps` (zero `eff=1`) and prints `100`; add straight-line and
effect-escapes-outward companion fixtures.

---

## SUPERSEDING FINDING (2026-07-17, branch `claude/effect-reopen-report-w2n5zh`)

Two things were pinned against the emitter this session; both change the plan.

### 1. The loop CANNOT be a same-function join -- it must be a recursive `__cps` fn

The DEEPER SCOPE section (and the `CT_LOOP`/`CT_CONTINUE` sketch) assumed the loop
back-edge could be a same-function multi-arg JOIN (a C label + `goto`, the way
`CT_LETCONT` lowers -- `emit_cps_ir.c:4233-4256`).  **Measured against
`emit_handle` (`emit_cps_ir.c:5428-5530`): it cannot.**  `emit_handle` lifts the
handle's continuation (`t->as.handle.body`) into a SEPARATE C function via
`emit_lifted(..., LH_RESET_CONT, ...)` (line 5438), installed as a `dk_frame`
(line 5490).  In this fixture the loop-carried update is
`total' = total + <handle result>` -- the new `total` is produced INSIDE that
lifted continuation function, so the back-edge (`total += h; i++; iterate`)
necessarily lives in the `_hk` continuation fn, a DIFFERENT C function from the
loop entry.  A C `goto` cannot cross that boundary, so a same-function join is
impossible for a loop whose carried state depends on an interior handle result.

Therefore the back-edge MUST be a real function call, which forces the loop to be
a **synthesized recursive colored `__cps` function** rather than a `CT_LOOP` join:

```
run$loop__cps(i, total, k):          # synthesized colored fn; params = the ^mut vars
  if (< i 5):
    cur = i
    <CT_HANDLE  perform(Ask) / case Ask k' -> resume k' (cur*10)>   # delim
    # ...continuation (the lifted _hk fn) receives the handle result h:
    total' = (+ total h)
    i'     = (+ i 1)
    return run$loop__cps(i', total', k)     # CT_TAILCALL back-edge (self-recursion)
  else:
    return k(total)                          # exit delivers the live-after value
# and run__cps(k)  ==>  return run$loop__cps(0, 0, k)
```

This is STRICTLY BETTER than the `CT_LOOP` design: the back-edge is an ordinary
`CT_TAILCALL` to a colored callee, which `emit_term` ALREADY lowers
(`emit_cps_ir.c:4207-4218`: `return <fn>__cps(args, thread)`), and it composes
with the E7 trampoline (a tail-resume inside the loop is the existing flat path).
No new looping-join emitter construct is needed.  The `CT_LOOP`/`CT_CONTINUE` IR
nodes in the DEEPER SCOPE plan are RETRACTED in favor of function synthesis.

**The remaining work is the synthesis + injection, and it is the real cost:**
a colored recursive `__cps` function does not exist in the source, so the pass
must (a) BUILD a `Binding`/`FnDef` for it (name, 2-param colored fn type,
result type), (b) INJECT it into the classification pass so `binding_in_s`
(`emit_cps_ir.c:2491`) returns true, a forward decl is emitted
(`emit_forward_decls`, line 5870), and the body is emitted as `run$loop__cps`
(main driver, lines 5980-6230), and (c) run the `EX_WHILE` transform with
loop-scoped mutation rebinding (reads of `i`/`total` resolve to the synthesized
params; `set!` values become the back-edge `CT_TAILCALL` args) under the sound
loop-entry-version-only guard.  Steps (a)/(b) are cross-cutting plumbing into the
classifier/emitter (`emit_cps_ir.c:3442-3670`); there is still no testable partial
(none of synthesis / injection / transform moves the fixture alone).

### 2. Delegating the self-contained region (CT_LETRAW) is a NON-solution

Tempting shortcut: since flag-OFF keeps the fixture correct by delegating the
whole self-contained handle region to the direct emitter, add an `EX_WHILE` case
that does the same under the flag (a `CT_LETRAW` region), so `run` emits
`run__cps` with zero `eff=1`.  **This does not satisfy the plan.**  A
direct-emitted self-contained handle runs on `global_effect_handler_chain`
(`safe_to_delegate` EX_HANDLE comment, `cps_ir.c:1382`), which is a SEPARATE,
non-DK effect lowering -- exactly what `cps-dk-sole-effect-lowering-plan.md` wants
to DELETE alongside the fiber runtime.  Delegating would move `run` off the fiber
onto `global_effect_handler_chain` (still non-DK), not onto the DK machine.  That
is why `safe_to_delegate`'s EX_HANDLE case already returns `false` under the flag
(`cps_ir.c:1395`): the flag's whole purpose is to force interior handles onto the
DK.  Only the native recursive-`__cps` lowering (finding 1) puts the interior
`Ask` perform/resume on the DK machine.  A `CT_LETRAW` delegation is ruled out.

---

## Why native DK lowering is a real slice (not a gate change)

The DK/CPS machine has no mutation and no loops -- it is tail calls + continuations. Lowering
this natively means:

1. **Loop-carried continuation args.** The `^mut` loop vars (`i`, `total`) cannot be C
   mutation in CPS. The `while` must lower to a tail-recursive loop JOIN -- a `CT_LETCONT
   loop(i, total)` whose body re-enters `loop(i', total')` -- so `set! i` / `set! total`
   become the next-iteration arguments. `(< i 5)` is the loop's `CT_IF` guard; the false arm
   delivers `total` to `run`'s return continuation.
2. **A nested per-iteration prompt.** The interior `handle` is a fresh DK prompt inside the
   loop body; `(perform (Ask))` threads to it, the case resumes with `(cur*10)`, and its
   result feeds `(+ total ...)` -- i.e. the join's `total'` argument. Each iteration's handler
   closes over the loop-local `cur`, so the prompt/case must capture `cur` from the current
   iteration's frame (a scalar capture -- collect_caps already handles scalars).
3. **Interaction with the E7 trampoline.** A loop that resumes through a per-iteration prompt
   must stay flat (the trampolined tail-resume already added for deep recursion); the loop
   join re-entry is itself a tail call, so this should compose, but verify stack flatness on a
   large iteration count.

This is a new transform (loop -> tail-recursive join with a nested prompt), not an admission
gate. It is the single native construct the CPS IR still lacks.

## Fix direction / verification

- Add an `EX_WHILE` case to the CPS transform that builds a loop-join `CT_LETCONT` with the
  `^mut` binders as loop-carried args, the cond as the guard, and the body lowered normally
  (so an interior `handle`/`perform` lowers as it does anywhere else). Watch: only the `^mut`
  vars written in the loop become loop-carried; a `^mut` read-only var can stay a capture.
  (See the DEEPER SCOPE section above: this needs a rebinding environment + a multi-arg loop
  join + emitter self-loop support first -- it is not a lone `case EX_WHILE`.)
- Companion minimal repros to iterate on: `while` + interior self-contained handle (must ->
  DK, print correct); `while` + interior handle whose effect ESCAPES to an outer handler
  (should still lower or cleanly evict -- do not miscompile); `while` with no control op (must
  stay delegated, unchanged).
- Gate: this is a shipping-backend transform (not necessarily flag-gated). Default suite
  (`bash tests/run.sh`, 12-min timeout) green; flag-off byte-identical if the transform only
  activates on the flag-on candidate path, else check snapshot churn. Full flag-on build sweep
  (known `-lturi`/turi false-positives only). Add a `cps-tramp-resume-...` regression fixture
  that asserts `run` emits `run__cps` (zero `eff=1` evictions) and prints `100`.

## Context

One of the compound BODY-UNSUPPORTED roots in
docs/upcoming/v2/cps-dk-sole-effect-lowering-plan.md. Related landed work: the P5 whole-body
delegation (keeps it correct on the fiber) and E7 trampolined tail-resume (keeps deep resume
flat -- the loop join must compose with it).
