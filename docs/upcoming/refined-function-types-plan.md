# Plan: Refinements in Function Types (Contravariant Subtyping)

> **Status:** PROTOTYPE (2026-10-28), behind `--enable=refined-fn-types` (an
> `EXPERIMENTS[]` row, introduced 0.72.0). F0-F3 are implemented, and F5's
> docs and its `--warn-unrefined-fn-slot` lint (`TUR-W0388`). Open: F4 (decide
> the result-check elision by measurement) and graduation, which waits on a
> named first consumer (open question 6) -- see "Implementation status" at the
> end. Lower priority; not on the v1 critical path.
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
*type* instead of off a `defn`'s parameter list. Reporting is **lenient on
severity, loud on visibility** (open question 5): an error only on a closed
refutation (`(f 10 0)`); otherwise `TUR-W0372`, **always**, and a runtime
check. This is *not* the direct-call `runtime_guarded` policy, which is silent
when undecided. That silence is justified for a direct call only because no
check is added there (the callee's entry check runs regardless). Here an
undecided argument *adds* a check, and code on a hot path must hear about it.
Neither stock policy fits (the own-claim path would make an open
counterexample a `TUR-E0371` error), so it is a `quiet` obligation worded by
its caller, as `rt_decide_variance` already does for instances: closed
refutation is `TUR-E0371`, and anything else is `TUR-W0372` (an error under
`--strict-refine`).

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
elision. Under `--no-contracts` there is no such check, so `T.r` is **not**
assumed either -- the same rule the class-result propagation already follows.

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
- The opt-in lint `--warn-unrefined-fn-slot` (open question 1): at a D4c
  coercion site whose target position is unrefined (the D2 skip), warn when
  the function value's own parameter in that position is refined, naming the
  slot whose type could carry it. A diagnostic-strictness knob, so no
  `EXPERIMENTS[]` row. It lands here rather than earlier because its advice
  ("refine the slot's type") is only actionable once F1-F3 ship.
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

1. ~~A lint for a refined function passed into an unrefined function-typed
   slot?~~ **Resolved 2026-10-27: opt-in flag.** `--warn-unrefined-fn-slot`,
   off by default like `--warn-unused-result`, added in F5 (see there). Only
   the parameter direction is linted; a refined *result* going into an
   unrefined slot is just lost information, not a possible panic.
2. ~~Should the parameter direction's `UNKNOWN` be silent rather than
   `TUR-W0372`?~~ **Resolved 2026-10-27: warn.** The real function's entry
   check keeps it sound, but a silent "couldn't tell" leaves a runtime panic
   with no compile-time signal. That includes under `--strict-refine`, which
   never sees the obligation today because the instance path decides it as a
   `speculative` probe (`refine_discharge.c:512` returns before any
   reporting). **The instance path warns too** (decided the same day): the
   F3 helper reports an undecided parameter pair as `TUR-W0372` on both
   paths, `--strict-refine` makes it an error, and the probe is counted as a
   real obligation rather than under `templates_tried`. The `TUR-E0374`
   explain text and the guide's "an undecidable pair keeps the runtime check"
   change with it. **Landed 2026-10-27, ahead of this plan** (it needs
   nothing else from it): `rt_decide_variance` in
   `src/compiler/elab_typeclasses.c`, pinned by
   `refine-instance-variance-unknown` and
   `errors/refine-instance-variance-unknown-strict`. F3 extracts that helper
   rather than writing a new one.
3. Named parameters in function types (D3): which spelling, and is there
   demand beyond `(> r x)`-style result bounds? **Deferred** (2026-10-27).
4. F4: wrapper or keep the check. **Decided by measurement** (2026-10-27),
   as F4 says.
5. **D4a reporting policy.** An argument at a call through `f` is reported
   with the lenient `runtime_guarded` policy (error only on a closed
   refutation). The alternative is to treat it like a function's own return
   refinement -- the HOF's author wrote the function type, so the HOF owes the
   proof, and an open counterexample is a `TUR-E0371` error. Lenient matches
   direct calls; strict matches who made the promise. **Resolved 2026-10-27:
   lenient on severity, loud on visibility** (D4a). An undecided argument
   compiles with a runtime check but always warns, because unlike a direct
   call it adds a check that performance-sensitive code needs to know about.
   Turning that warning into an error *where the code asks for it* is
   `docs/upcoming/strict-refine-scopes-plan.md` (planning stage). Prior art splits along one line: whether the language has a
   runtime fallback. Fully static systems (Liquid Haskell, F*, Dafny) reject
   any unproved obligation, because there is nothing else to fall back on.
   Systems with a runtime fallback -- hybrid type checking (Flanagan 2006) and
   Sage, gradual refinement types (Lehmann and Tanter 2017), Racket's
   higher-order contracts, Ada/SPARK -- reject only what is provably wrong and
   insert a check for the rest, with full proof as an opt-in mode. Turmeric is
   in the second group, and its default plus `--strict-refine` already has
   that shape. One idea to take from Racket regardless: the failure message of
   the call-site check should blame the caller of `f` (the HOF body that broke
   `f`'s type), not `f`.
6. **Demand and priority.** *Noted 2026-10-27: not blocked, lower priority;
   planning continues.* The one consumer that has looked at this,
   `ecs-refinement-typed-apis-plan.md`, lists refined function types as out of
   scope because it does not need them. Before F1 starts, name a first
   consumer (a stdlib HOF, a spice API, a scheduler holding function values)
   and decide whether this belongs on the v1 track or after it. The
   instance-path warning from question 2 does not depend on the rest and could
   land on its own first.

## Implementation status (2026-10-28)

What landed, phase by phase, and where it lives. Everything is behind the gate
except the lint.

- **F0.** The pinning fixture became a real one rather than an `expected.xfail`,
  because F2 landed with it: `errors/refine-hof-param-crossing` (a closed
  refutation through `f` is `TUR-E0371`). `errors/refine-in-fn-type` is
  unchanged and still green with the gate off.
- **F1.** `TY_FN` carries `param_refine_preds` / `param_refine_vars` /
  `result_refine_pred` / `result_refine_var` (`types.h`), read only through
  `type_fn_refinements_of` and, for a binding, `rt_binding_refined_fn_type`
  (`elab_internal.h`) -- a plain `(fn ...)` parameter rides the typed `:fn`
  carrier, so its signature is `poly_type`, not `type`. There is one parser for
  `(fn ...)` types (`type_expr_from_form`; `fn_type_from_form_impl` routes the
  head there), so one site fills them: `fn_type_slot_contract` in
  `elab_types.c`, which also keeps `TUR-E0378` for `c-fn` and for a D3
  violation. The "one erasure helper" turned out to be no helper at all: the
  fields are a side-car that no identity function reads, so erasure is the
  default and the accessor is the only reader.
  `tests/check-refined-fn-types-erased.sh` (ctest
  `tur_refined_fn_types_erased`) is the byte-identical exit test, run under
  `--no-contracts` because with contracts on F2 is meant to change the C.
  `type_name` does NOT show refinements: diagnostics name the slot instead
  ("the function type of argument 1 of 'apply2'"), which reads better than a
  printed type and keeps `type_name` out of the identity question entirely.
- **F2.** `rt_elab_refined_fn_call` (`elab_fns.c`), hooked in `elab_call`
  ahead of the alias fold and of every lowering path, and in
  `elab_call_head_expr` for a call through an expression (`((.f h) 3)`).
  Arguments at refined positions are checked at the call; a non-trivial one is
  evaluated once into a temporary, with every non-trivial argument before it,
  so evaluation order is unchanged. The crossing is a `RefineCallSite` with
  `fn_type` set, decided by `rt_resolve_fn_type_crossing` under the D4a policy;
  a proof nils the check. The result is checked after the call (never elided:
  F4) and assumed through `rt_resolve_fn_with_locals`, for parameters the body
  does not rebind. Under `--no-contracts` there are no checks and nothing is
  assumed. The interpreter runs the same `Expr` tree, so `run-turi.sh` sees the
  same panic text and location. Fixtures: `refine-fn-type-call-proved`
  (including the result hypothesis proving a later crossing, under
  `--strict-refine`), `refine-fn-type-call-unknown`,
  `refine-fn-type-result-check`. Not covered: partial and over-application
  through a refined binding, and a variadic one, which fall back to the
  function's own entry checks.
- **F3.** `rt_decide_entailment` (`elab_fns.c`) is the extracted helper; the
  instance check in `elab_typeclasses.c` calls it for both directions, so
  `TUR-E0374` and the `refine-instance-variance-*` fixtures are unchanged.
  `rt_check_fn_coercion` runs it at every D4c site: call arguments and
  constructor arguments (`elab_call`), annotated `let` (`elab_forms.c`), `def`
  and `defn` results (`elab_fns.c`), `set!` into a field (`elab_set_field`),
  and a lambda literal at any of them, checked against the expected type its
  slot pushes. Codes `TUR-E0386` / `TUR-E0387`, with `tur explain` entries.
  Fixtures: `refine-fn-variance-ok`, `errors/refine-fn-variance-param`,
  `errors/refine-fn-variance-result`, `errors/refine-in-fn-type-scope` (D3,
  D6).
- **F4.** Not started; still "measure first".
- **F5.** The guide's "Limits" bullet, the internals guide's RT1 list and codes
  table, the `TUR-E0378` explain text and the rationale's "Subtyping" row are
  updated. `--warn-unrefined-fn-slot` / `TUR-W0388` is in, off by default and
  independent of the gate (fixture `refine-fn-unrefined-slot-lint`).
  Graduation is not done: it waits on F4 and on a first consumer.

