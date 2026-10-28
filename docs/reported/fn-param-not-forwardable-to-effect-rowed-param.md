# A plain function-typed parameter cannot be passed to one whose type has an effect row

**Severity:** medium-low (expressiveness; misleading message). A higher-order
function cannot hand its callback on to another one that declares an effect
row, including the documented row-polymorphic idiom.

## Repro

```turmeric
(defn inc1 [x : int] : int (+ x 1))
(defn rowed [g : (fn [int] #fx{e} int) x : int] : int (g x))   ; or #fx{} / #fx{IO}
(defn forward [g : (fn [int] int)] : int (rowed g 1))
(defn main [] : int (println (forward inc1)) 0)
```

```
error [TUR-E0001]: function 'rowed' arg 1: expected (fn [int] : int), got ptr<void>
```

The same error appears for `#fx{}`, `#fx{IO}` and the row variable `#fx{e}`.
`(rowed inc1 1)` works, and so does forwarding when `forward`'s own parameter
also carries a row. The other direction works too: a rowed parameter passed to
a plain one (`fw3` in the scan).

## Root cause

The two parameters are represented differently. A plain, effect-free
`(fn [A] R)` parameter rides the typed `:fn` carrier (binding type `ptr<void>`,
signature in `poly_type`). A parameter whose type carries an effect row keeps
its nominal `TY_FN`, because the carrier has no row slot (the `effectful` gate
beside `carrier_ok` in `elab_defn`, `src/compiler/elab_fns.c`). The argument
check in `elab_call_fn_inner` (`src/compiler/elab_call.c:10435`) then compares
`ptr<void>` with `TY_FN` and reports the carrier's C type, a type the program
never wrote. Nothing bridges a carrier value into a nominal slot, although the
opposite bridge exists (`EX_POLY_WRAP`).

## Fix directions

- Accept a poly-carrier binding (`is_poly_fn`, concrete `TY_FN` `poly_type`)
  for a nominal `TY_FN` parameter. Lower it by unwrapping the carrier to the
  fat or thin form the parameter expects, and check the row as for any
  argument. Note the
  [effect-row-check-skips-let-bound-fn-values](effect-row-check-skips-let-bound-fn-values.md)
  gap: a plain parameter's row is unknown, not empty.
- At minimum, print `poly_type` rather than `ptr<void>` in the message, and say
  that the declared rows differ.
- Workaround today: give the forwarding parameter a row too
  (`[g : (fn [int] #fx{e} int)] #fx{e}`).
