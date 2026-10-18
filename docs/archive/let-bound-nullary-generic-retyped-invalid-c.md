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

## Resolution (2026-10-18)

The second fix direction. A let binding remembers its initializer when it is
a call (`Binding.let_init`, set in `src/compiler/elab_forms.c`). When S4
retypes the binder, it grounds that call the same way: it collects the
bindings that take the call's open result type (`(Option A)`) to the
binder's new type (`(Option int)`), records them as the call's
`abi_bindings`, and retypes the call. This is what `ng_ground_nullary_arg`
already does for a nullary call passed as an argument. The emitter then
emits the specialization, which returns `tur_adt_Option__int` by value.

The first direction (no retype for a by-value type) was tried and rejected.
The binder then stays `(Option A)`, so a later
`(match o (Some x) (+ x 1) (None) 0)` stops type-checking.

Pinned by `let-bound-nullary-generic`: two nullary `(Option A)` binders, one
at `int` (read with `unwrap-or` twice and a `match`) and one at a float, a
`(Result A cstr)` one, and a `(Vec P)` that still takes the S4 path it was
written for. Checked under `tur run`, `tur --interpret` and `tur jit`.
