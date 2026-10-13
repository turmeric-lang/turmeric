# A generic function that performs an effect its caller handles is refused

**RESOLVED 2026-10-13.** The repro prints `10` compiled, as the interpreter
does. The root cause below was a guess, and wrong on its main point: the ABI
scan already mints a clone for every call of a generic that performs, at every
type, `int` included (`emit_cps_ir_colored_fn_needs_mono`, read by
`emit_abi_register_call`), and the CPS backend admits those clones
(`--dump-cps-mono`: `emit_twice__spec__int64_t_int64_t ... ADMISSIBLE`). Three
things went wrong around them:

- **The carrier base was still emitted.** `emit_abi_fn_skip_generic`
  (`src/compiler/emit_module.c`) drops a generic's carrier base only when it is
  "generic-unsafe": a type variable nested in a compound type, such as
  `(Option A)`. That is why `cps-backend-generic-cross-fn` worked. A bare
  `[x : T]` is carrier-safe, so its base was emitted, and the direct emitter
  has no lowering for its `perform`, so it was a hard error even though
  nothing called the base. The base of a generic that needs a monomorph is now
  skipped on the same "no carrier call" condition
  (`emit_cps_ir_generic_base_is_template`). It asks only of a function whose
  signature has a type variable: a non-generic function has no clones and no
  carrier calls on record, so the first cut, which asked of every function,
  dropped two SIG-REJECTed `defer` performers outright.
- **A generic that performs only through a callee got no clone.** The
  needs-a-monomorph test was syntactic: an effect form in the function's own
  body. `(defn outer [T] [x : T] : T (pass-through x))`, or a generic calling
  a plain performer, failed it, so its base ran, called the callee's
  fresh-root entry, and took the effect off the backend program-wide. For an
  unconstrained, non-instance generic the test now follows direct calls
  (`fn_body_reaches_effect_op`, `src/compiler/emit_cps_ir.c`; each function is
  visited once per query). Typeclass methods and constrained generics keep the
  strict seed, as the `unsafe` note in that function asks.
- **The callee clone minted from a generic base was refused.** `outer`'s
  erased base body registers `pass-through`'s clone with `T` bound to
  `outer`'s own `T`, and the concrete `int` clone shares its name
  (`__spec__int64_t_int64_t`), so that is the one emitted. Under its resolver
  `x` still resolves to a type variable, and the slot gate refused it at the
  `perform`'s continuation. Inside a monomorph such a type is the erased
  carrier, which the clone's signature spells `int64_t`, so `slot_ok_t` and
  `cap_ty_ok` admit it as that word (`mono_erased_tyvar`; only while a spec
  resolver is active, so the template gates are unchanged).

Pinned by `tests/fixtures/cps-generic-performs-caller-handled`
(leak-checked): the repro; `pass-through` at `int`, `cstr`, `float` 7.1, a
narrow and a wide ADT; an unused performing generic; `outer` at `int`,
`float`, `cstr` and a wide ADT; a generic calling a plain performer; an island
generic; a non-generic sibling sharing the effect; and a second effect with an
argument. The same answers under `tur --interpret` and `tur jit`.

---

**Severity: medium (expressiveness).** A correct program is refused at compile
time, never miscompiled. Any function with a type parameter that `perform`s an
effect handled outside its own body fails with "this effect operation has no
lowering here". This holds at every instantiation, `int` included, and every
function sharing the effect is evicted with it. The interpreter runs it. Found
2026-10-13 while probing
[cps-performing-fn-with-wide-adt-param-refused](../archive/cps-performing-fn-with-wide-adt-param-refused.md);
it does not depend on aggregate width.

## Repro

This is the example
[cps-backend-generic-monomorph-classification-plan](../archive/cps-backend-generic-monomorph-classification-plan.md)
gives as "verified sound" (`direct == cps == 10`):

```turmeric
(defeffect A [] :int)
(defn emit-twice [T] [x : T] : int { (perform (A)) + (perform (A)) })
(defn run [] : int (handle (emit-twice 0) (A [] k) (resume k 5)))
(defn main [] : nil (println (run)))
```

| | result |
| --- | --- |
| `tur --interpret` | `10` |
| `tur run` | "this effect operation has no lowering here" at the first `perform` |
| `(defn pass-through [T] [x : T] : T (do (perform (A)) x))` at `int`, a 16-byte ADT, a 24-byte ADT | the same, all three |
| implicit type variable (`[x : a]`) | the same |

`TUR_TRACE_EVICT=1`: `SIG-REJECT eff=1 emit-twice`.

## Root cause (partly measured)

The plan's soundness argument was that a generic which cannot be CPS-classified
runs its effect on the fiber machine, and the taint fixpoint co-classifies its
handler to fiber too. The fiber effect runtime has since been retired, so the
fallback now has nowhere to go and the `perform` is refused instead.

On the CPS side, the generic template `SIG-REJECT`s on its type-variable
parameter (`fn_sig_ok`). The G3b route for a generic is the mono-template
(`mono_template_all_admissible`, `src/compiler/emit_cps_ir.c`). It stands in
only when every ABI clone (`ctx->abi_specializations` with `spec->fn == fd`)
passes `mono_sig_ok` and the core check, and it answers false when there is no
clone at all. A scalar instantiation is likely called through the int64
carrier with no clone (`emit_abi_note_carrier_call`), so `any` stays false.
Why the aggregate instantiations miss too is not measured. `is_cps_island`
admits only a generic that handles its own effects (`cps-backend-generic-island`).

## Fix directions

1. Let a carrier-called generic's base function be its own CPS entry: its
   signature is the uniform int64 carrier, so a `__cps` twin over carrier
   words is ABI-consistent where a concrete one is not. That is the
   `sig_slot_ok` name-collision hazard `fn_single_concrete_sig` guards
   against, from the other side.
2. Or force an ABI clone for every instantiation of a generic that performs
   an escaping effect, so the mono-template route has something to admit.
   Then find what stops the aggregate clones (trace `mono_sig_ok` /
   `term_core_ok` under `TUR_TRACE_CORE=1` with the clone active).
