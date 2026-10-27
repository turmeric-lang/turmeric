# A `handle` over an effectful fn-field call, inside a `let` in argument position, is refused

**Severity: medium. RESOLVED 2026-10-01.** A legal program is refused at build time. This is an
expressiveness gap, not a miscompile. Found 2026-10-01 by the type fuzzer: every
one of its `GEN_REJECT`s in an 800-case run (11 of 11) is this shape, through
the `fn_field_eff` crossing.

## Repro

```turmeric
(defeffect Ef [x : int] : int)
(defstruct FE :copy [run : (fn [int] int) #fx{Ef}])
(defn fe [v : int] : int (perform (Ef v)))
(defn main [] : int
  (println (let [s (make-struct FE fe)]
             (handle (.run s 3) (Ef [x] k) (resume k x))))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset, and the direct emitter cannot lower `perform`.
```

`TUR_TRACE_EVICT=1` reports `SIG-TAINT` for both `fe` and `main`.

The same handle with the `let` OUTSIDE the call works and prints 3:
`(let [s (make-struct FE fe)] (println (handle (.run s 3) ...)))`. So does
`(handle (do (.run s 3) 0) ...)` under a let-bound `s`.
`tests/fixtures/fn-field-typed-float` covers the `:nil`-returning effects, which
work.

## Shapes that are refused

- `(println (let [s (make-struct FE fe)] (handle (.run s 3) ...)))`
- `(handle (.run (make-struct FE fe) 3) ...)` as an argument
- `(handle (let [s (make-struct FE fe)] (.run s 3)) ...)` as an argument

## Fix direction

Find which form `ensure_S`'s taint fixpoint (`emit_cps_ir.c`) marks
permanently tainted when the handled body sits under an argument-position
`let`. The working twin differs only in where the `let` is. Once fixed, the
fuzzer's `fn_field_eff` crossing generates no `GEN_REJECT`s, and its count
is the regression check.

## Resolution (2026-10-01)

**RESOLVED.** Three causes, each enough to evict the handler and the performer
together:

1. **The struct-store walker had gaps.** `expr_stores_fnval_in_struct`
   (`emit_cps_ir.c`) decides whether an effectful fn-value is stored in an
   effectful struct field, and so registered for threading (E2c). It had no
   case for `EX_BUILTIN` (`println`'s argument), `EX_GET_FIELD` (a field call
   on `(make-struct ...)`), `EX_MATCH` or `EX_LETREC`. An unregistered,
   escaping, effectful fn-value is a permanent fiber source, so it showed as
   `SIG-TAINT` for both `fe` and `main`. That is why moving the `let` out of
   `println` worked.
2. **A `handle` as a method call's operand.** A dict-dispatched method call
   is an indirect callee to the CPS translation, which delegates it whole
   and needs atomic operands ("indirect call (non-atomic args)").
   `elab_method_call` now routes its result through
   `elab_hoist_control_operands`, which binds a control-bearing operand out,
   as it already did for constructor calls.
3. **A non-atomic argument to the field call.** The E2a/E2c registry paths
   atomize their arguments into pending CPS bindings, but refused
   non-atomic ones up front (`call_args_atomic`). They now check only that
   the pending array has room (`call_args_pendable`).

Pinned by `tests/fixtures/handle-over-effectful-fn-field-shapes`: the
three refused shapes above, plus a non-atomic float argument, a float
round-trip and a method-call receiver, compiled and `--interpret`. The five
`GEN_REJECT`s from fuzz seeds 1111 and 2222 all produce their expected output.
