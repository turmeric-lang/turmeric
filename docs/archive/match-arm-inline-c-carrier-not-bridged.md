# A `match` arm returning an inline-C `Option` / `Result` is not unboxed into the result

**Severity: high** (a silent wrong value for `(Option T)` with a niche
payload; a cc error for a by-value `(Result T E)`). Found 2026-10-16 while
fixing [byvalue-sum-from-vec-get-bridged-twice](../archive/byvalue-sum-from-vec-get-bridged-twice.md).
Reproduced on `main` at `32387e3d`, before and after that fix.

## Repro

```turmeric
(load "stdlib/string.tur")

(defn mk-opt [n : int] : (Option String)
  ```c
  if (n == 0) return tur_none();
  return tur_some_ptr(tur_string_from_bytes("hi", 2));
  ```)

(defn show-opt [o : (Option String)] : nil
  (if (some? o) (println (string/to-cstr (unwrap o))) (println "none")))

(defn main [] : int
  (show-opt (if (< 1 2) (mk-opt 1) (mk-opt 0)))              ;; hi
  (show-opt (match 2 1 (mk-opt 0) _ (mk-opt 1)))             ;; prints an empty line
  (let [o (match 2 1 (mk-opt 0) _ (mk-opt 1))] (show-opt o)) ;; prints an empty line
  0)
```

The `if` prints `hi`; both `match` forms print an empty line, with no
diagnostic. The same `match` over an inline-C `(Result String int)` producer
does not compile:
`incompatible types when assigning to type 'tur_adt_Result__String__int' from type 'int64_t'`.

## Root cause

An inline-C body declared `(Option T)` / `(Result T E)` returns the int64
carrier box (`tur_some_ptr` / `tur_ok_ptr`). A control form whose value is
such a producer has to convert it into its result temp.

- `emit_if_value` (`src/compiler/emit_expr.c`, the then and else arms around
  `:5710`) declares the temp from `fn_body_tail_byvalue_carrier_type` and
  bridges each carrier-producing arm `CK_CARRIER -> CK_CONCRETE`.
- The `match` emitters declare the temp from `match_result_temp_type`
  (`emit_expr.c:21279-21296` for an ADT scrutinee; the literal-pattern path
  declares its own) and assign each arm with `emit_merge_assign_bridged`
  (`:20985`, `:21153`, `:21842`, `:22319`), which never unboxes.
  - For `(Option String)` the temp is the niche form, a bare `void *`, and
    `if_arm_word_ptr_cast` casts the carrier box pointer to it. The box
    address is then read as the `String`.
  - For a by-value `(Result String int)` the temp is a struct, and assigning
    the int64 to it is the cc error.

## Fix directions

- Give the `match` arm assignment the `if` arm's bridge: compute the
  by-value carrier type once (`fn_body_tail_byvalue_carrier_type` already has
  an `EX_MATCH` case), and for an arm that produces the carrier
  (`!fn_body_tail_emits_byvalue_carrier_abi`, not already a recorded by-value
  aggregate) bridge it before assigning. The temp's declared type must agree
  with the bridged type, as `emit_if_value` arranges.
- Pin it with the repro above, a `Result` arm, and a `match` over an ADT
  scrutinee as well as a literal one, since they take different emitters.

## Resolution (2026-10-16)

Every `match` arm assignment now goes through `match_arm_carrier_bridge`
(`src/compiler/emit_expr.c`, beside `emit_merge_assign_bridged`). It applies
the `if` arm's carrier->concrete bridge to an arm when all of these hold:

- the arm is a carrier producer (`fn_body_tail_is_carrier_producer`) whose
  tail does not already emit the by-value aggregate;
- the arm's by-value type (`fn_body_tail_byvalue_carrier_type`) spells
  exactly the C type the merge temp was declared and recorded with;
- the value in hand is not already a recorded by-value local.

It is called at all five arm sites: the ADT if-chain and switch emitters, the
literal-pattern emitter, the `any`/union emitter and the session emitter. The
gate is tighter than the `if` arm's because the match temp is declared from
the match's own type, not from its arms: an arm that already yields the
by-value form (a constructor built by value, a variable) is left alone.

A match in a function's tail position needed nothing: `emit_tail` returns each
arm through the function's own return bridge.

Passing the match straight to a by-value parameter also needed the call-argument
change from
[byvalue-sum-from-vec-get-bridged-twice](byvalue-sum-from-vec-get-bridged-twice.md)
(PR #1227). The straddle forward mirror otherwise unboxes the already-bridged
temp a second time. That change landed first, in #1227.

Pinned by `match-arm-inline-c-carrier-bridged`:

- `(Option String)`, `(Result String int)`, `(Option int)` and `(Option float)`
  (7.1) producers;
- literal, string, ADT and `Option` scrutinees;
- tail and non-tail matches;
- producer arms mixed with constructor arms.

On the unfixed `main` that fixture has 22 cc errors.
