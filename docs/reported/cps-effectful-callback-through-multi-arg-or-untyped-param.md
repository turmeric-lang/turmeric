# An effectful callback through a multi-argument or untyped fn parameter is refused

**Narrowed 2026-10-09: scalar arguments and results thread, pointer
results and capturing closures of those kinds too.**

- A `float`, `float32`, `bool` or narrow-integer argument or result crosses
  the `fn_cps` slot (see "Fixed 2026-10-09" at the end).
- A `cstr` or `ptr<void>` RESULT crosses as its word through `intptr_t`, and
  the direct fallback (`fncps_direct_call`) calls the wrapper at its real
  pointer result type. Pinned by
  `tests/fixtures/cps-effectful-callback-pointer-result`.
- A CAPTURING closure's dispatcher (`ensure_fncps_env_dispatch`) is typed
  per signature: it converts each word for the registered twin (a float at
  its own type, every other kind as the word its `__e2w` adapter takes) and
  for slot 0 (each at its C type), and delivers a float or pointer result as
  its word. Every scalar kind is admitted except an untyped `ptr<void>`
  argument and a narrow-integer or `bool` RESULT: either puts a widening
  wrapper in slot 0, which the registry does not know. Pinned by
  `tests/fixtures/cps-effectful-capturing-callback-scalar-kinds`.
- Found on the way: a function with a parameter named `k` is kept off the
  CPS backend (`param_name_clashes_cps`, for leaks in some pure shapes), so
  an effectful lambda `(fn [y : float k : int] ...)` was refused. A function
  whose effects may leave it is exempt now, as a handle-installer already
  was: evicted, its performs had no lowering anyway.
- Also found on the way, pre-existing and not CPS-specific in kind: a
  capturing closure with a `cstr` or aggregate result, handed to a
  non-retaining fn parameter inside a CPS function, leaked its env (24 B per
  call). `cps_closure_env_freeable` took scalar results only; it now takes
  what the direct emitter's `let_binding_env_freeable` takes (a `cstr`,
  by-value aggregate or carrier result, no inline C), walking the body
  exactly for inline C.

All three fixtures are leak-checked, pass the JIT harness and run clean under
`-fsanitize=function`. **What is left:**

- shape 2, an untyped `^fat` parameter;
- an aggregate (struct or ADT) argument or result;
- a capturing closure with an untyped `ptr<void>` argument or a
  narrow-integer / `bool` result.

**Narrowed 2026-10-08: shapes 1, 3 and 4 are fixed.** That covers a callback
of up to eight arguments (word integers, `cstr` or `ptr<void>`), a capturing
closure, and a `bool` or unit result.

(At that point a capturing closure was narrower still: word-integer
arguments and an `int` or unit result only. Widened 2026-10-09, above.)

**Severity: low-medium** (was medium). A compile-time refusal of a correct program; `tur
--interpret` runs it. Until 2026-10-07 these compiled and then aborted at run
time with `unhandled effect` (on `main` too). The fix for
[cps-effectful-fnval-escapes-through-unthreaded-param](../archive/cps-effectful-fnval-escapes-through-unthreaded-param.md)
turned that into this honest refusal. Annotating the parameter's row
(`(fn [int int] #fx{Ask} int)`) avoids it.

Four shapes, all through an un-annotated (empty-row) fn parameter:

- a callback of two or more arguments (the repro below);
- an untyped `^fat` parameter, `(defn app [^fat h x : int] : int (h x))`;
- a CAPTURING effectful closure, `(let [m 5] (app (fn [x : int] : int (+ m
  (perform (Ask)))) 1))`;
- an effectful callback whose result is not `int`, e.g.
  `(fn [int] void)` with `(fn [x : int] : void (println (+ x (perform
  (Ask)))))`.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn app2 [h : (fn [int int] int) x : int] : int (h x x))
(defn main [] : int
  (println (handle (app2 (fn [x : int y : int] : int (+ x y (perform (Ask)))) 1)
             (Ask [] k) (resume k 10)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

Expected `12`. The same happens with an untyped `^fat` parameter,
`(defn app [^fat h x : int] : int (h x))`, given an effectful lambda.

## Root cause

A call through an un-annotated (empty-row) fn parameter carries the caller's
continuation only through the fat value's `fn_cps` slot, and that slot's ABI
is one int-class argument (`fncps_param_call_ok`, `src/passes/cps_ir.c`:
"Restricted to the single-int-arg `tur_poly_fn_t.fn_cps` ABI"). Any other
call is a direct call. And the slot is FILLED (EX_POLY_WRAP, `emit_expr.c`)
only for a global fn of one `int` argument and an `int` result, so a
capturing closure or a non-`int` result reaches the call with an empty slot
and takes the direct `.fn` path from a fresh root. The registry path that threads an effectful-row call
(`via_registry`) is chosen only when the parameter's declared row is
non-empty (`call_is_effectful_fnvalue`), so it never applies here.

## Fix directions

- Widen the `fn_cps` slot's ABI to N word-class arguments and any result
  kind, the way `e2a_cast` already spells the registry's twins, and fill it
  for a capturing closure (an env-taking twin, as the registry's fat dispatch
  already calls).
- Or let an empty-row call through a REGISTERED thread param take the
  registry path too, when the analysis has seen an effectful value flow in.

## Fixed 2026-10-08: multi-argument, capturing, and `bool` / unit callbacks

Fix direction 1, without widening the struct.  `tur_poly_fn_t.fn_cps` stays
declared at the one-argument ABI; a slot of any other arity is stored through
a cast and called back at its own type, so every indirect call is made at the
callee's real type (`-fsanitize=function`-clean).

- **One shape question.**  `cps_ir_fncps_sig_ok` (`src/passes/cps_ir.c`) says
  whether a fn fits the slot: up to `CPS_FNCPS_MAX_ARGS` (8) arguments, each an
  `int`/`int64`, a `cstr` or a `ptr<void>` (each crosses the slot as its word,
  and the twin converts it back to the callee's own C type,
  `cps_ir_fncps_arg_ctype`), and an `int`/`int64`, `bool` or unit result.  The poly-wrap that
  FILLS the slot (`emit_expr.c`, EX_POLY_WRAP) and the analysis that counts a
  value as threaded only when the slot will be filled (`arg_fat_has_fn_cps`,
  `emit_cps_ir.c`) both ask it, where each used to hard-code "one int argument,
  int result" separately.  The call side (`fncps_param_call_ok`) takes N word
  arguments; the IR builder atomizes them all (`fncps_atomize_args`).
- **Named fns and captureless lambdas.**  `ensure_poly_wrap_cps_thunk`
  (`emit_module.c`) emits the `__poly_N__cps` twin at the fn's arity and result
  (`void` / `bool` forward declarations; a unit result delivers 0).  The
  one-argument `int` twin is byte-identical to before, so no snapshot moved.
- **Capturing closures.**  The closure arm of EX_POLY_WRAP appends a
  `__tur_fncps_env<N>` dispatcher (`ensure_fncps_env_dispatch`) for an
  effectful lambda: it keys the direct->CPS registry on the env box's slot 0 --
  the lifted entry, which a threadable capturing lambda registers against its
  env-taking `__cps` twin -- so it does not depend on knowing which lambda built
  the box; a miss makes the call an empty slot would have made.  Which lambda
  a poly-wrap packs is answered once, `emit_poly_wrap_fncps_closure`, for both
  the fill and `arg_fat_has_fn_cps`.
- **Emission.**  `fncps_slot_call` and `fncps_direct_call` (`emit_cps_ir.c`)
  spell the threaded and the direct call at any arity, in tail position and in
  the heap join.

Pinned by `tests/fixtures/cps-effectful-callback-pointer-args` (`cstr` and
`ptr<void>` arguments, named and lambda, tail and non-tail),
`tests/fixtures/cps-effectful-callback-multi-arg` (named fns and
lambdas, arities 0-3, `bool` and unit results, tail and non-tail, pure values
through the same parameters), `cps-effectful-capturing-closure-callback`
(literals and a let-bound closure, arities 0-2, `int` and unit results) and
`cps-effectful-closure-through-unannotated-param` (the refusal fixture this
report's earlier state pinned in `errors/`, now a passing fixture printing
16).  Every line equals `tur --interpret`, and both fixtures' programs also run
clean under clang's `-fsanitize=function`.

Found on the way, pre-existing and filed separately:
[cps-effectful-closure-returned-through-empty-row-aborts](../archive/cps-effectful-closure-returned-through-empty-row-aborts.md)
-- an effectful closure returned by a call, `(app1 (adder 3) 1)`, compiled
and aborted with `unhandled effect`, as it did before this change.  Since
2026-10-08 it is refused at compile time instead.

## Fixed 2026-10-09: scalar arguments and results

The slot's kinds are now every scalar (`fncps_arg_kind_ok`,
`src/passes/cps_ir.c`), and a result is any scalar but a pointer
(`fncps_result_kind_ok`). Each argument crosses as its int64 word,
`cps_ir_fncps_word_of`, and is read back by `cps_ir_fncps_value_of`. A float
crosses as its bits, which is the DK slot's own Tier-B convention, so a
result the callee's `__cps` entry delivers reads back the same way. A narrow
integer or `bool` crosses by a cast.

- **Call site** (`fncps_slot_call`, `src/compiler/emit_cps_ir.c`): it passes
  the words. The direct fallback (`fncps_direct_call`) calls the wrapper at
  each parameter's and the result's own C type. A float result is delivered
  at its own type and stored into the slot by its bits; in the heap join it
  is bit-cast before `dk_run`.
- **Twin** (`ensure_poly_wrap_cps_thunk`, `src/compiler/emit_module.c`): it
  forward-declares the direct entry at each own C spelling. It passes a
  float to the registered `__cps` entry at its own type, as every E2a call
  site does (`e2a_cast`); everything else goes as a word, which the
  `__e2w` adapter converts for a narrow or pointer parameter. A float result
  of the direct fallback is delivered by its bits.

Integer and pointer twins are byte-identical to before, so no snapshot moved.

Found on the way and fixed under
[cps-match-on-builtin-sum-evicts](../archive/cps-match-on-builtin-sum-evicts.md):
a numeric `(as float ...)` around a `perform` evicted its function, and a
lifted helper's float capture stayed recorded under its source name for the
rest of the program. The latter bit-cast an integer `x` in the prelude's
`vec-eq-loop`. Separately,
[effect-row-lost-under-match-cast-letrec](../archive/effect-row-lost-under-match-cast-letrec.md):
the probe callbacks' rows were empty because their `perform` sat under an
`as`.

Pinned by `tests/fixtures/cps-effectful-callback-scalar-kinds`. It covers:
- `float`, `float32`, `bool`, `int8` and `int32` arguments and results;
- a three-argument mix;
- tail and heap-join calls;
- named functions and lambdas;
- pure callbacks through the same parameters.

Every line equals `tur --interpret`. The fixture is leak-checked, passes the
JIT harness, runs clean under `-fsanitize=function`, and has no finding from
`check-emitted-float-conversions.py`.
