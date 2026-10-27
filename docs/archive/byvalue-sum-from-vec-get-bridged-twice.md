# A 16-byte by-value sum read with `vec-get` is unboxed twice as a call argument

**RESOLVED 2026-10-16** -- see [Resolution](#resolution-2026-10-16). Pinned by
`tests/fixtures/byvalue-aggregate-element-as-call-arg` and
`tests/fixtures/inline-c-option-wrapped-producer-as-call-arg`.

**Severity: medium** (a hard cc error on the default path for an ordinary
shape; the workaround is one `let`). Found 2026-10-16 writing turmeric-spices'
`signal/tuning` tests, where `(degree? (vec-get entries 0) 0)` over a
`(Vec MapEntry)` failed to compile. Reproduced on `main` at `d60c40f6`.

## Summary

Passing `(vec-get v i)` straight to a function whose parameter is a `:copy`
sum stored by value in 9..16 bytes (a tag plus one word of payload) emits the
carrier-to-aggregate dereference twice:

```c
d_qu((*(tur_adt_E *)(intptr_t)((*(tur_adt_E *)(intptr_t)(__ps_207)))), INT64_C(3))
```

The outer cast dereferences the aggregate the inner one already produced, and
cc stops with `aggregate value used where an integer was expected`.
Let-binding the element first compiles and runs correctly. A sum whose by-value
form is wider than 16 bytes is taken by pointer, goes through a different
branch, and is unaffected.

## Repro

```turmeric
(defdata E :copy
  (D :int)
  (U))

(defn d? [e : E x : int] : bool
  (match e
    (D y) (= x y)
    (U)   false))

(defn main [] : int
  (let [v (:: (vec-new) (Vec E))]
    (vec-push! v (D 3))
    (println (d? (vec-get v 0) 3)))   ;; cc: aggregate value used where an integer was expected
  0)
```

| Variant | Result |
| --- | --- |
| as above (`(D :int)`: tag + 8 bytes) | cc error |
| `(D :float)` | cc error |
| `(D :int :int)` (24 bytes, pass-by-pointer) | `true` |
| `(let [e (vec-get v 0)] (d? e 3))` | `true` |

The error is the same with `d?` in an `if` test or inside `and`.
`tur emit-c --emit-abi-trace` prints
`repr-trace bridge carrier->concrete aggregate E` twice for the one argument.

## Root cause

Two arms of the per-argument loop in `emit_value_dispatch` (EX_CALL) each
bridge the same argument, carrier to concrete:

1. The multiword-element-boxing arm, `src/compiler/emit_expr.c:15161-15193`.
   The argument is a carrier accessor (an inline-C fn whose declared result is
   a bare type variable: `vec-get`, the HAMT value readers), and the parameter
   is a wide by-value ADT, so a pass-by-value slot gets
   `emit_carrier_bridge(..., CK_CARRIER, CK_CONCRETE, mwb_param_ty)` at
   `:15190`. This is a standalone `if`, not part of the `else if` chain that
   follows.
2. The inline-C straddle "forward mirror" arm, `emit_expr.c:15338-15377`.
   `fn_body_tail_byvalue_carrier_type` reports the `vec-get` call as a carrier
   of `E`, `fn_body_tail_emits_byvalue_carrier_abi` says it does not emit the
   aggregate itself, and the callee's parameter C type is `tur_adt_E`, so it
   bridges again at `:15372`. It tests the argument EXPRESSION, not `raw`,
   so it cannot see that arm 1 already converted it.

Confirmed in gdb on a RelWithDebInfo build: both `emit_carrier_bridge` calls
are `CK_CARRIER -> CK_CONCRETE` for `E`. The second receives
`(*(tur_adt_E *)(intptr_t)(__ps_207))` as its source.

A pass-by-pointer (> 16 byte) aggregate escapes because arm 1 emits
`(const T *)(intptr_t)(raw)` and arm 2 refuses a parameter C type containing
`*`.

## Fix directions

- Have arm 1 record that it converted the argument (a local `mwb_bridged`),
  and gate the `else if` chain's carrier-to-concrete arms on it. Or move arm 1
  into the chain, so at most one bridge fires per argument.
- Either way, pin it with a fixture that passes `(vec-get v i)` of a
  one-word-payload `:copy` sum to a by-value parameter, plus the same through a
  HAMT value reader, since `mwb_arg_is_carrier_accessor` covers both.

## Resolution (2026-10-16)

Fixing it showed the defect was wider than filed. Any 9..16-byte by-value
aggregate handed straight to a by-value parameter could be dereferenced twice:

| Argument | Second bridge came from |
| --- | --- |
| `(vec-get v i)` of a one-word-payload sum, or of a two-word `defstruct` | the multiword-element-boxing arm (`emit_expr.c`) |
| `(map-get m k)` of the same | `map-get` is a macro around a `let`, whose own lowering bridges its tail into a by-value temp |
| an inline-C `(Option T)` / `(Result T E)` producer inside a `let`, `do` or `if` | the wrapper's lowering, as for `map-get` |

In every row the inline-C straddle forward-mirror arm then bridged `raw` a
second time. It asks whether the argument EXPRESSION produces a carrier
(`fn_body_tail_byvalue_carrier_type`), which stays true through a wrapper or
after another arm has converted `raw`.

The fix keeps that arm's condition as it was, because in the per-argument
`else if` chain the condition also claims the argument: an earlier attempt
that tightened it let a later arm spill a `vec-of` element into a mistyped
temp (`typeclass-applied-class-var-result` and two others, "invalid
initializer"). The two new tests guard only the bridge itself:

- `mwb_bridged`, set when the multiword-element-boxing arm converts `raw`;
- the argument, through ascriptions, is a direct `EX_CALL`
  (`expr_strip_ascribe`), the only shape whose `raw` still holds the carrier.

`tests/run.sh` 3862 passed, 0 failed. The two fixtures also pass under
`run-jit.sh`; under `run-turi.sh` the first passes and the second is the
inline-C carve-out. Both fail to compile on the unfixed `main`.
