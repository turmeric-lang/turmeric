# Plan: Refinements in Function Types (Contravariant Subtyping)

> **Status:** DRAFT (2026-10-27). Nothing implemented. Would ship behind
> `--enable=refined-fn-types` until graduation, per the experimental-features
> rule in CLAUDE.md.
> **Type:** Compiler / Type System (refinement types)
> **Predecessors:** `docs/archive/refinement-types-plan.md` ("Indirect calls",
> 2026-07-24 -- the function-typed parameter case was left open there)
> **Related:** `docs/guides/refinement-types-guide.md` (the "[prototype]
> Higher-order callees are not checked" limitation),
> `docs/guides/refinement-solver-internals-guide.md` (RT1 crossing points),
> `docs/guides/advanced-type-system-rationale.md` (the "Subtyping" row of the
> refinement-types table)

## The gap

A function type cannot carry a refinement. Writing one is a hard error,
`TUR-E0378` (`reject_fn_type_contract`, `src/compiler/elab_types.c:710`):

```turmeric
(defn apply2 [f : (fn [int #refine{ v : int | (not= v 0) }] int)] : int ...)
;; error[TUR-E0378]: a refinement cannot be written on the parameter of a
;; (fn ...) type; function types do not carry refinements
```

So a value reached through a function-typed parameter is invisible to the
refinement checker in both directions:

```turmeric
(defn safe-div [a : int b : #refine{ v : int | (not= v 0) }] : int (/ a b))
(defn apply1 [f : (fn [int int] int) x : int] : int (f 10 x))

(apply1 safe-div 0)   ; compiles; fails at run time in safe-div's entry check
```

- The **body** of `apply1` cannot be told what `f` needs, so `(f 10 x)` is not
  checked.
- The **caller** of `apply1` cannot be told what `apply1` will pass to `f`, so
  there is no way to say "this slot only accepts functions that are happy with
  a non-zero second argument" and have that checked.
- Nothing can flow out of `f` either: a `(fn [int] #refine{ r : int | (> r 0) })`
  result cannot be written, so a caller of `f` learns nothing about what it
  returns.

None of this is unsound today. Every function's own entry checks still run
(`rt_inject_param_checks`, `src/compiler/elab_internal.h:1362`), so `safe-div`
catches the zero. What is lost is the *static* crossing: the error surfaces
at run time, and checks that could have been proved away are kept.

What already works, so the scope here stays narrow
(`tests/fixtures/refine-indirect-calls`): direct calls, a `let`-bound alias of
a global (`(let [g safe-div] (g 10 0))`, folded to the global in `elab_call`),
and a call to a lambda with refined parameters. The gap is specifically a
value reached through a *function-typed binding*: a parameter, a struct field,
or anything else whose type is `(fn ...)`.

## The rule

When is a function of type `S` safe to use where a function of type `T` is
expected (`S <: T`)? Write `S.p_i` and `T.p_i` for the refinement on parameter
`i`, and `S.r` / `T.r` for the result refinement (an absent one is `true`):

- **Parameters are contravariant:** `T.p_i |- S.p_i` for every `i`. Whoever
  holds the value at type `T` will only promise arguments satisfying `T.p_i`,
  so the real function must accept at least that much. A function that accepts
  *more* is fine; one that demands *more* is not.
- **The result is covariant:** `S.r |- T.r`. Whoever holds the value at type
  `T` relies on `T.r` about what comes back, so the real function must deliver
  at least that much.

Concretely, with `NonZero = (not= v 0)` and `Pos = (> v 0)`:

| Real function `S` | Expected `T` | `S <: T`? | Why |
|---|---|---|---|
| `(fn [NonZero] int)` | `(fn [Pos] int)` | yes | `Pos \|- NonZero`: every positive is non-zero |
| `(fn [Pos] int)` | `(fn [NonZero] int)` | **no** | `-1` is non-zero but not positive |
| `(fn [int] Pos)` | `(fn [int] NonZero)` | yes | `Pos \|- NonZero` |
| `(fn [int] NonZero)` | `(fn [int] Pos)` | **no** | it may return `-1` |

Each check is an ordinary entailment between two predicates over the same
variable, so the existing solver chain (S0-S3) decides it with no new solver
work.

**The compiler already runs exactly this check** in one place: a typeclass
instance against its class signature (`TUR-E0374`,
`src/compiler/elab_typeclasses.c:5060-5160`). There the class signature plays
`T` and the instance method plays `S`. The parameter loop proves
`class_pred(p) |- instance_pred(p)`, and the result block (comment at line
5119, "RESULT variance, which runs the OPPOSITE way to parameters") proves
`instance_pred(r) |- class_pred(r)`. Both are decided as speculative
obligations, with `refine_model_search` asked for a witness before anything
is reported. The core of this plan is to lift that check out of the instance
path into a shared helper and call it wherever one function value meets
another function type.

## Design decisions

### D1 -- Representation: refinements ride on `TY_FN`, erased for identity

`TY_FN` (`src/compiler/types.h`, the `as.fn` arm) gains, per parameter, a
`const Form *` predicate and a `const char *` bound-variable name (NULL when
unrefined), plus the same pair for the result. They are carried the way
`param_type_forms` already is: an arena array of length `arity`.

They are **erased for type identity**: `type_eq`, unification, mangling, C
typedef naming (`register_fn_ptr_typedef`) and the ABI cache all ignore them.
Two function types that differ only in refinements are the same type to
everything except the refinement checker. That keeps unification decidable
and generic code unchanged, and it is the same split `TY_CONTRACT` already
lives with: base type for the type system, predicate on the side for the
checker (`rt_peel_contract`, `src/compiler/elab_internal.h:1334`).

The contract peel was missed at three separate annotation sites before it got
one helper. Missing an erasure site here has the same shape: refinements
silently changing which overload or instance is picked, or a typedef name
changing. So the erasure must go through **one** helper. F1's exit test is
that emitted C is byte-identical with and without refinements in function
types.

### D2 -- Unrefined function types stay unchecked (gradual)

Under a strict reading, `(apply1 safe-div 0)`'s `safe-div` is *not* a subtype
of `(fn [int int] int)`: `true |- (not= b 0)` fails. Enforcing that would turn
a program that compiles today into an error, and would make every existing
higher-order API (`map`, `for-each`, reactor callbacks) reject refined
functions.

So a position that the target type leaves **unrefined** is not checked, which
keeps today's behaviour exactly: the real function's own entry check is the
guard. The variance check runs only for positions the target type refines.
Refinements in function types are opt-in, and the cost is paid only by code
that writes one.

(An optional lint for "refined function passed into an unrefined slot" is
listed under open questions. It is not part of this plan.)

### D3 -- Predicate scope in v1: the position's own variable only

Parameters of a `(fn ...)` type are unnamed, so a predicate on parameter `i`
can mention only its own bound variable. A predicate that mentions another
parameter, or a result predicate that mentions an argument
(`(fn [x : int] #refine{ r : int | (> r x) })`), needs named parameters in
function types. That is a separate syntax decision and is **deferred**. In v1
it is rejected with a pointed diagnostic, not dropped.

### D4 -- Where obligations come from

Three sites. Each reuses machinery that exists today.

**(a) Calls through a refined function-typed binding (the HOF body).**
`(f 10 x)` where `f : (fn [int #refine{ v : int | (not= v 0) }] int)` is a
crossing exactly like a direct call. Each argument becomes an obligation
against `T.p_i`, recorded through the same path as `refine_note_call_site`
(`src/compiler/elab_fns.c:4488`), but reading the predicates off the binding's
*type* instead of off a `defn`'s parameter list. Reporting is the
`runtime_guarded` policy (`refine_collect.h`, `RefineObligation`): an error
only on a closed refutation (`(f 10 0)`), otherwise `TUR-W0372` and a runtime
check.

That runtime check has to be **new**, emitted at the indirect call site. The
real function's entry check only enforces `S.p_i`, which may be weaker than
`T.p_i`. Without a check at the call site, a refinement written in the type
would have no runtime meaning, which breaks the invariant the whole feature
rests on ("every refinement already has a runtime meaning"). When the
obligation is proved, the check is elided as usual.

**(b) Results of calls through a refined function-typed binding.**
The value of `(f 10 x)` is assumed to satisfy `T.r`, which gives the solver a
hypothesis about it. That assumption is sound only if every function that can
reach `f` really delivers `T.r`. The static check at (c) cannot always prove
that, and it cannot see values that arrive through erased (`:int`, `Any`) or
C-side paths. So **v1 checks the result at the indirect call site** and then
assumes it: a post-call contract check, never elided in v1. F4 revisits
elision.

**(c) Coercion sites -- the contravariant check proper.**
Wherever a function value of type `S` flows into a slot of refined function
type `T`, run the variance check from "The rule", for the positions `T`
refines:

- a call argument to a function-typed parameter (`(apply2 safe-div ...)`);
- an annotated `let`;
- a `defn` return whose declared type is a refined function type;
- struct/ADT constructor arguments and `set-field!` into a refined
  function-typed field.

Where `S`'s predicates come from:

- a global `defn` or a lambda: its binding's contract-parameter arrays (the
  same ones `refine_note_call_site` reads);
- a value already of refined function type: the type (D1);
- anything else (`extern-c`, a value read back from an erased slot): `S.p_i =
  true`, `S.r = true`. The parameter direction then always holds, and the
  result direction is `UNKNOWN`, covered at run time by (b).

Verdicts, mirroring `TUR-E0374`:

| Verdict | Parameter direction (`T.p \|- S.p`) | Result direction (`S.r \|- T.r`) |
|---|---|---|
| proved | nothing emitted | nothing emitted |
| refuted with a witness | **error** (new code) | **error** (new code) |
| unknown | `TUR-W0372`; `S`'s own entry check is the guard | `TUR-W0372`; (b)'s call-site result check is the guard |

A refutation is an error rather than a warning because it is a
declaration-versus-declaration inconsistency the programmer wrote down. That
is the same judgement `TUR-E0374` makes for instances.

### D5 -- Gating

Behind `--enable=refined-fn-types`: an `EXPERIMENTS[]` row in
`src/runtime/experiments.c` with every descriptor field, `plan_path` pointing
here, and `experiment_warn_if_used` called where a refined function type is
first accepted. With the gate off, `TUR-E0378` stays exactly as it is.
Graduation removes the gate and retires `TUR-E0378` for `(fn ...)` types
(D6 keeps it for `c-fn`).

### D6 -- Out of scope

- **`(c-fn ...)` types.** A bare C function pointer has no Turmeric entry
  checks to fall back on. `TUR-E0378` stays for them.
- **Function types in type-argument position** (`(List (fn [...] int))`). These
  are peeled with `TUR-W0380` like every other type-argument refinement
  (`rt_peel_type_arg_contract`). Enforcing payload refinements is its own
  feature.
- **Named parameters / dependent refinements** in function types (D3).
- **Elision of the callee's own entry check.** That is whole-program and is
  already listed as a separate by-design limit.
- **Effect rows, HKT, typeclass method signatures with function-typed
  parameters.** These get whatever falls out of D1-D4 with no special casing,
  and nothing more is promised.

## Phases

### F0 -- Pin today's behaviour

- An `expected.xfail` fixture `refine-hof-param-crossing`: `apply2` declares a
  refined function-typed parameter and calls it with a provably bad argument.
  `expected.stdout` holds the answer the plan wants, and the marker names this
  plan.
- Keep `tests/fixtures/errors/refine-in-fn-type` green (gate off).

### F1 -- Representation and syntax (behind the gate)

- Add the D1 fields to `TY_FN` and fill them in both function-type parsers
  (`fn_type_from_form_impl` and the type-application loop; see the note above
  `rt_peel_type_arg_contract` for why there are two).
- With the gate on, `reject_fn_type_contract` records instead of rejecting. D3
  violations and `c-fn` keep an error.
- One erasure helper used by `type_eq`, unification, mangling and typedef
  naming.
- `type_name` shows refinements in diagnostics only.
- Exit: the full suite is unchanged, and emitted C is byte-identical for a
  fixture that differs only by refinements in function types.

### F2 -- Calls through a refined binding (D4a, D4b)

- Record argument obligations for indirect calls whose callee type carries
  refinements.
- Emit the call-site argument check (elided when proved) and the post-call
  result check. Then make `T.r` a hypothesis on the call's value.
- Check `tests/run-turi.sh` parity: the interpreter must fail the same checks
  with the same message and location.
- Fixtures: proved (no check in `expected.c`), refuted-closed (error), unknown
  (warning plus a firing runtime check), and a result-refinement hypothesis
  that proves a later obligation.

### F3 -- The variance check at coercion sites (D4c)

- Extract the parameter and result variance logic from
  `elab_typeclasses.c:5060-5160` into one helper. A sketch:
  `refine_check_fn_variance(Elab*, S preds, T preds, Span, what)`, returning
  a per-position verdict.
- Point the instance check at the helper, so `TUR-E0374` behaviour is
  unchanged (its fixtures are the regression net), and call it from each D4c
  site.
- Two new diagnostic codes in the free `TUR-E038x` range (0386/0387 at the
  time of writing): "function demands more of parameter N than the function
  type allows" and "function promises less about its result than the function
  type requires". Each gets a `tur explain` entry with the witness.
- Fixtures: each row of the table in "The rule", each coercion site, the D2
  unrefined-slot case (must stay silent), and the `extern-c` / erased-source
  case.

### F4 -- Result-check elision (measure first)

(b)'s post-call check is paid on every indirect call through a type with a
result refinement. Two ways to drop it, in increasing cost:

1. **Do nothing.** It is opt-in, and if measurement says it is cheap, it
   stays.
2. **Wrap at the coercion site** (Findler-Felleisen style). Where (c) cannot
   prove `S.r |- T.r`, wrap the value in a checking closure. Then every value
   at type `T` is known to deliver `T.r`, and the call-site check goes. The
   cost is an allocation per unproved coercion. The wrapper's environment fill
   is a closure-env store and is already region-hooked, but this must be
   confirmed against `tests/fixtures/region-escape-via-store`.

Pick one with numbers, not by default.

### F5 -- Docs and graduation

- `refinement-types-guide.md`: replace the "[prototype] Higher-order callees
  are not checked" bullet and the `TUR-E0378` example.
- `refinement-solver-internals-guide.md`: add the two new crossing points to
  the RT1 list and the codes to its diagnostics table.
- `diag.c`: update the `TUR-E0378` explain text, which today says this is the
  open gap.
- `advanced-type-system-rationale.md`: the "Subtyping" row moves to done.
- Then graduate: drop the gate and move this plan to `docs/archive/`.

## Risks

- **A missed erasure site** (D1). The likeliest failure is quiet: a refinement
  changes a typedef name or an overload match. Mitigation: the single helper,
  plus F1's byte-identical-C exit test.
- **Check placement on the fat-closure path.** Indirect calls go through
  several lowering paths (bare, boxed `{thunk, env}`, the `EX_FN_TO_FAT`
  shim). The F2 checks must be added before that split, keyed on the call
  form as `refine_note_call_site` is, not once per lowering.
- **Interpreter parity.** The new checks must exist on both engines, or
  `run-turi.sh` and `run.sh` disagree about which program panics.
- **Diagnostic noise.** D2 keeps unrefined code silent. Even so, any API that
  adopts a refined function type will surface `TUR-W0372` at every caller
  whose function the solver cannot relate. That is intended, but worth
  checking against one real higher-order API (a stdlib `map`/`for-each`
  variant, or the ECS plan's refined APIs) before graduating.

## Open questions

1. A lint (not an error) for a refined function passed into an unrefined
   function-typed slot. It would point at the slot that could carry the
   refinement. Useful, or noise?
2. ~~Should the parameter direction's `UNKNOWN` be silent rather than
   `TUR-W0372`?~~ **Resolved 2026-10-27: warn.** The real function's entry
   check keeps it sound, but a silent "couldn't tell" leaves a runtime panic
   with no compile-time signal. That includes under `--strict-refine`, which
   never sees the obligation today because the instance path decides it as a
   `speculative` probe (`refine_discharge.c:512` returns before any
   reporting). Whether the extracted F3 helper should also start warning on
   the *instance* path (`TUR-E0374`'s undecided pairs) is a separate call.
3. Named parameters in function types (D3): which spelling, and is there
   demand beyond `(> r x)`-style result bounds?
4. F4: wrapper or keep the check -- decided by measurement.
