# An effectful function called through a local name is refused

**Narrowed a third time 2026-10-09: a CAPTURING lambda used both through
the local and as a value runs** (see "Fixed: a value use as well as calls"
below). That shape was worse than this report said: it was not refused, it
compiled and aborted `tur: unhandled effect (tag 2)` at run time. Before
that, the same day, the capturing shapes called in place and the
captureless shapes were fixed. **What is left:**

- a lambda that RETURNS a function;
- a CAPTURELESS lambda used both through the local AND as a value (the
  capturing one runs now);
- a capturing lambda called from inside ANOTHER closure (`g` calls `f`, both
  `let`-bound): `g` captures `f`, which is a value use;
- a capturing `letrec` of more than one member (mutual recursion).

Each is still refused, honestly, at compile time ("this effect operation has
no lowering here"); `tur --interpret` runs them.

**Severity: low-medium** (was medium). This is a compile-time refusal of a
correct program, not a wrong answer, and the ordinary idioms (a helper
lambda, a capturing loop, a helper both called and handed on) now compile.
Found 2026-10-09 while fixing
[effect-row-lost-under-match-cast-letrec](../archive/effect-row-lost-under-match-cast-letrec.md).

## Repro (still refused)

```turmeric
(defeffect Ask [] :int)
(defn app [h : (fn [int] #fx{Ask} int) x : int] #fx{Ask} : int (h x))
(defn v1 [] : int
  (let [g (fn [n : int] : int (+ n (perform (Ask))))]     ; captureless
    (+ (g 1) (app g 2))))                                 ; also a value
(defn v2 [m : int] : int
  (let [f (fn [n : int] : int (+ m n (perform (Ask))))
        g (fn [n : int] : int (* 2 (f n)))]               ; g captures f
    (g 1)))
(defn v3 [m : int] : int
  (letrec [ev (fn [i : int] : int (if (= i 0) (+ m (perform (Ask))) (od (- i 1))))
           od (fn [i : int] : int (if (= i 0) m (ev (- i 1))))]
    (ev 4)))
(defn main [] : int
  (println (handle (v1) (Ask [] k) (resume k 10)))     ; 23
  (println (handle (v2 5) (Ask [] k) (resume k 10)))   ; 32
  (println (handle (v3 1) (Ask [] k) (resume k 10)))   ; 11
  0)
```

## Root cause (what is left)

The rewrite below needs every use of the local to be a saturated call
(`pap_calls_saturated`). A use as a value -- an argument, or a capture by
another closure -- means the lambda can be called from where nothing
threads the caller's continuation, so it stays an fn value; the E2
threadability tally then decides it. For a CAPTURING lambda the thread-local
registration below now covers the call through the local; a captureless one
is the thin direct-entry value, which the E2 tally reads differently (its
let alias is an alias target), and its call is still evicted. A `letrec` of
several capturing members shares one env-building protocol the CPS
translation does not reproduce.

## Fix directions

- A captureless value-and-call local: register it as a thread local too
  (`cps_ir_thread_local_add(b, false)` -- the thin key, `(intptr_t)g`,
  which `e2a_lookup_key` already spells), once the tally admits its lambda
  as threadable with the alias in play.
- A closure capturing another local closure: when the captured closure is
  only CALLED in the capturing lambda, the call inside is the same env call
  through the env field.
- A fn-returning lambda: the direct call to its lifted `__fn_N` returns the
  int64 carrier, not the closure (pr-386, `Binding.is_lifted_lambda`), so it
  needs the closure protocol too.

## Fixed (2026-10-09): a value use as well as calls (capturing)

`(let [g (fn [n : int] : int (+ m n (perform (Ask))))] (+ (g 1) (app g 2)))`
compiled and aborted. The value use `(app g 2)` is a threadable argument
position, so the E2 tally admitted `__fn_N` as threadable (its env-taking
`__cps` twin registered against its direct entry) and nothing tainted
`Ask` to the fiber; but the tally never counts a CALL through the local as
a use, and the CPS translation lowered `(g 1)` as the direct emitter spells
it, `__fn_N((void *)g, 1)` -- the lambda's direct entry, which installs a
fresh root. The `perform` inside found no handler: `tur: unhandled effect
(tag 2)`, from a program that type-checked and compiled.

- **Thread locals.** Beside the thread PARAMS, the classifier now registers,
  per round, the thread LOCALS (`thread_local_visit`,
  `src/compiler/emit_cps_ir.c` -> `cps_ir_thread_local_add`,
  `src/passes/cps_ir.c`): an immutable `let`-bound local holding a fresh
  capturing lambda that is threadable and that the let's body (or a later
  sibling init) also calls through the local. A local the env-call rewrite
  already owns (every use a saturated call) is left to it; a local a nested
  closure captures is left alone (that closure's call would run from its
  own body, where the local is an env field this registration does not
  name -- the `g` calls `f` shape above, still refused).
- **The call.** A call through a thread local lowers like an empty-row call
  through a fat thread param (`call_threads_via_registry`, both the tail and
  the bind arm): a `via_registry` tailcall, the continuation reified as a
  heap join when the call is not in tail position. The emitter's fat
  dispatch (`emit_e2a_fat_dispatch`) reads slot 0 of the env box -- the
  lifted entry -- looks it up in the E2a registry and calls the env-taking
  twin with the box, the arguments and the caller's continuation, so the
  lambda's `perform` reaches the caller's handler. The same effect-row arm
  that admits a thread-param call admits a thread-local one;
  `safe_to_delegate` refuses the call (delegated, it would be the direct
  entry again).
- **Naming.** The callee is spelled by `name_for_binding`
  (`registry_callee_name`), the let local's id-suffixed name, which is also
  how a lifted continuation's frame read-out spells it: when the call sits
  after a `perform` in the let, the local rides the frame env as the int64
  scalar the existing E2c capture carries (`cap_add_fn_scalar`).
- **The box.** The env box is reaped at the DK entry boundary, as a freeable
  closure's is -- including when the let's body performs:
  `cps_closure_env_freeable` asked `closure_binding_escapes`, which reads a
  `perform` as an escape, and so refused every let whose body performed
  (24 B a run, every CPS-lowered let of a capturing lambda whose let
  crossed a `perform`, pre-existing). The reap frees at the outermost DK
  entry's exit, after every resume the entry sees, so it asks the reaped
  owner's variant (`closure_binding_escapes_reaped`) now.
- Not changed: a thread local's lambda called from a function the classifier
  evicts for some other reason still runs from a fresh root -- the same
  exposure a thread PARAM has, since withdrawal (`fnval_withdraw_walk`)
  runs before classification.

Pinned by `tests/fixtures/cps-local-closure-value-and-call` (leak-checked;
every line equals `tur --interpret`): called then passed; the call after a
`perform` (a captured local in a lifted continuation); the call in tail
position; called twice and passed twice; a two-parameter lambda; passed to
a recursive loop that calls it 100 times; the direct call and the threaded
one sharing one counting handler. Measured: the fixture suite is unchanged
otherwise (3667 passed, no snapshot moved).

## Fixed (2026-10-09): the capturing shapes

- **The call.** A capturing closure's value is its env box, and its lifted
  lambda takes that box as its first parameter; the direct emitter already
  spells a call through the local `__fn_N((void *)g, x)`. An immutable local
  bound to a fresh capturing lambda whose every use is a saturated call
  (`cps_ir_let_local_closure`, `src/passes/cps_ir.c`) is registered with the
  alias machinery as an env call (`PapInline.is_env_call`): each `(g x)`
  becomes `(__fn_N g x)`, the lambda's own CPS call, which threads the
  continuation. Unlike the captureless alias the binding stays, since the
  box is still built. Only a lambda whose effects may leave it qualifies (its
  declared or inferred row is not runtime-pure); a pure one keeps the direct
  path. `safe_to_delegate` refuses the `let` (and the calls) so it is not
  delegated whole.
- **The classifier.** The tally records the lambda as a callee and an alias
  target, so it is direct-only (no fn value), and `fn_sig_ok` admits a
  direct-only capturing lambda (`src/compiler/emit_cps_ir.c`).
- **`letrec`.** A one-member `letrec` whose lambda reaches itself only by
  saturated self-calls is lowered like the `let`. Inside the lambda, a call
  through the member is a call of the lambda with its own env parameter
  (`cps_self_env_call`, mirroring the direct emitter's `tco_is_self_call`),
  and a self tail call in a closure's `__cps` body is now a backedge, as it
  already was for a named function: 1,000,000 performing turns run in flat
  C stack.
- **The box.** The let's env box is reaped at the DK entry boundary, like a
  freeable closure. Every use being a call, it never leaves the let; a
  scalar result points into nothing, and otherwise the drop glue must free
  the box alone (no rc, owned fat closure or Drop-instance capture, no
  inline C). This also frees the box of a PURE capturing lambda let-bound
  in a colored function whose body performs, which used to leak 24 B per
  run: `closure_binding_escapes` reads a `perform` as an escape, so the
  existing freeable test never passed.
- Found on the way, pre-existing on HEAD, both back ends: a mutually
  recursive `letrec` of capturing closures never freed its member envs -- the
  members capture one another, so each read as escaping into a sibling's env
  (64 B per call of a two-member loop). When no member leaves the body and
  each member's lambda only calls the others, the group is dead at scope
  exit and every box is dropped (`letrec_members_confined`,
  `src/compiler/emit_expr.c`; the drop glue releases no closure capture, so
  it is one free each). Pinned by `letrec-mutual-capturing-envs-freed`
  (leak-checked: two- and three-member groups, 100 calls each). The
  EFFECTFUL mutual `letrec` is still refused (above).
- Found on the way, pre-existing on HEAD: a mutual-tail-call group's entry
  stub zero-filled the other members' scalar slots as `(int64_t){0}`, which
  the JIT's C front end refuses ("braces around scalar initializer"), so
  `cps-local-fn-alias-called-in-place` fell back to cc under `tur jit`. A
  scalar slot's zero is now a cast.

Pinned by `tests/fixtures/cps-capturing-local-lambda-called-in-place`
(leak-checked). It covers:
- tail, non-tail and repeated calls;
- float and struct captures;
- a box crossing a `perform` in the caller;
- calls under `match` and `if`;
- a nil result;
- a lambda whose only effect is a colored callee's;
- a lambda built on every loop turn;
- cstr, struct and Option results;
- `letrec` loops (tail, non-tail, float, nested in a capturing lambda).

`tests/fixtures/cps-capturing-letrec-loop-deep` (compiled only) runs
1,000,000 performing turns of a capturing `letrec` loop. Every line equals
`tur --interpret`. Both pass the JIT harness and run clean under
`-fsanitize=function`. Measured: the fixture suite is unchanged otherwise
(3655 passed, no snapshot moved), `check-emitted-float-conversions.py
--corpus` reports none, and the source type fuzzer at seed 3333 is at its
baseline (282 ok, 0 bugs).

## Fixed (2026-10-09): the captureless shapes

- **`let` alias.** An immutable local holding a global fn, named or a lifted
  captureless lambda, is registered with the translator's existing alias
  machinery when every use is a saturated call (`cps_ir_let_global_fn_alias`,
  `src/passes/cps_ir.c`, beside the fn-PARAM alias of
  [cps-let-alias-of-effectful-fn-param-refused](../archive/cps-let-alias-of-effectful-fn-param-refused.md)).
  The binding is dropped and each call is rewritten to a direct call to the
  global. A generic target and one whose result is a function are left out.
  `safe_to_delegate` no longer delegates a call through such an alias (or
  through a `letrec` member) of a colored global: delegated, it would run
  the global's direct entry from a fresh root.
- **`letrec`.** A captureless member is lifted to a global and names its
  lambda in `source_binding` (`elab_letrec`). A call through it, including
  the lambda's own self-call, is rewritten to a direct call to the lambda
  (`cps_ir_letrec_member_target`, beside `pap_maybe_rewrite`). A `letrec`
  whose members are all such globals binds names, not values, so its body is
  all the translator lowers.
- **The tally** (`expr_collect_effects_acc`, `src/compiler/emit_cps_ir.c`)
  records such an init as a CALLEE of the target, not a value use and not an
  address taken. A value use of a `letrec` member counts as a use of its
  lambda (`fvm_ref_slots`). A lifted lambda with no value use at all, held
  by such an alias, is no fn value (`direct_only_add`): nothing calls it
  from a fresh root, so it is no fiber source.
- `pap_calls_saturated` gained a `match` arm and falls back to the shared
  operand enumeration for the other kinds. A nested fn definition still
  refuses, since it may capture the alias.

Pinned by `tests/fixtures/cps-local-fn-alias-called-in-place`. It covers:
- a let-bound lambda, tail, non-tail and called twice;
- a let alias of a named fn, with an `int` and with a `float` result;
- a `letrec` loop that performs on each of 100,000 turns;
- mutual recursion between two members;
- a call through the alias under a `match` arm.

Every line equals `tur --interpret`. The fixture is leak-checked, passes the
JIT harness and runs clean under `-fsanitize=function`. The effect pass's
half of this (the false TUR-W0033 these shapes drew) was fixed under
[effect-row-lost-under-match-cast-letrec](../archive/effect-row-lost-under-match-cast-letrec.md);
the fixture's `unexpected.stderr` pins it.
