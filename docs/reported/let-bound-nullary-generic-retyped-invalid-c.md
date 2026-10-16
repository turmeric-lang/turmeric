# A `let`-bound call to a nullary generic emits invalid C

**Severity:** medium (cc failure after `tur check` passes).

## Repro

```turmeric
(defn mk [A] [] : (Option A) (none))

(defn main [] : int
  (let [o (mk)]
    (println (unwrap-or o 7)))
  0)
```

- `tur check`: succeeds.
- `tur run`: cc fails:

  ```
  error: invalid initializer
      tur_adt_Option__int o_4 = __ps_202;
  ```

Annotating the binder, `(let [o : (Option int) (mk)] ...)`, works.

## Root cause

The S4 forward-element step in `src/compiler/elab_call.c:elab_call_fn_inner`
learns `A = int` from the later use `(unwrap-or o 7)` and rewrites the
**binding's** type to `(Option int)`. The `(mk)` initializer call is not
re-grounded: it records no `abi_bindings`, so the emitter still emits it on
the `int64` carrier. The binder is then declared as the by-value monomorph and
initialized from the carrier word.

## Fix directions

- Restrict S4's binding retype to heap-pointer types, where the carrier word
  is the pointer; or
- when S4 rewrites a binding, also ground the initializer call (record its
  `abi_bindings`) so the emitter emits the specialization.
