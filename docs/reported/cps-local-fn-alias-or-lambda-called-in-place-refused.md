# An effectful function called through a local name is refused

**Narrowed 2026-10-09 (filed the same day): every captureless shape is
fixed.** A `let`-bound captureless lambda, a `let` alias of a named
function, and a captureless `letrec` member (a loop, mutual recursion) all
compile now when called through the local name. See "Fixed" below. **What
is left:**

- a CAPTURING lambda called through its local name, whether `let`-bound or
  a `letrec` loop that reads an outer local;
- a lambda that RETURNS a function;
- a lambda used both through the alias AND as a value (passed on to a
  higher-order function).

Each is still refused, honestly, at compile time ("this effect operation has
no lowering here"); `tur --interpret` runs them.

**Severity: medium.** This is a compile-time refusal of a correct program,
not a wrong answer. A capturing `letrec` loop that performs is an ordinary
idiom. Found 2026-10-09 while fixing
[effect-row-lost-under-match-cast-letrec](../archive/effect-row-lost-under-match-cast-letrec.md).

## Repro (still refused)

```turmeric
(defeffect Ask [] :int)
(defn c1 [m : int] : int (let [g (fn [n : int] : int (+ m n (perform (Ask))))] (g 3)))
(defn c2 [m : int] : int
  (letrec [loop (fn [i : int] : int (if (= i 0) (+ m (perform (Ask))) (loop (- i 1))))]
    (loop 3)))
(defn main [] : int
  (println (handle (c1 5) (Ask [] k) (resume k 10)))   ; 18
  (println (handle (c2 5) (Ask [] k) (resume k 10)))   ; 15
  0)
```

## Root cause (what is left)

A capturing lambda is a closure: its call through the local goes through
the closure protocol on the env box, not to a global entry. The fixed
shapes' rewrite to a direct call does not apply to it. The E2 threadability
tally sees the `let` / `letrec` init as a value use of the lifted lambda, so
the lambda is not threadable and becomes a permanent fiber source
(`SIG-TAINT`). A `letrec` member also has no `EX_LETREC` translation when it
is not global.

## Fix directions

- Thread the call through the local closure the way E2a threads a capturing
  lambda passed as an argument. A threadable capturing lambda already
  registers its env-taking `__cps` twin. A call through a local binding of
  that closure could dispatch through the registry (`via_registry`, the
  `emit_e2a_fat_dispatch` path) instead of the direct `.fn` call. The tally
  would then count such a use as threadable, and a capturing `letrec` loop
  is the same call made from inside its own body.
- A fn-returning lambda: the direct call to its lifted `__fn_N` returns the
  int64 carrier, not the closure (pr-386, `Binding.is_lifted_lambda`), so it
  needs the closure protocol too.

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
