# A plain function-typed parameter cannot be passed to one whose type has an effect row

**RESOLVED 2026-11-05** -- see "Resolution" at the end.

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

## Note for whoever fixes this (2026-10-29)

Bridging the parameter must not admit an effectful function into a `#fx{}`
slot. The parameter's type `(fn [int] int)` carries no row, and the ER4
call-site check reads "no row" as "no effects". So once `(pure-hof h 1)`
compiles with `h` a parameter, the caller can pass `noisy` through it.
[effect-row-check-skips-let-bound-fn-values](../archive/effect-row-check-skips-let-bound-fn-values.md)
closed the same hole for `let` aliases by following them to their
initializer. A parameter has no initializer to follow, so a row-less
function type flowing into a rowed slot has to be refused or need an
annotation.

## Resolution (2026-11-05)

Both halves of the note above, split by whether the slot's row is open.

- **An open row** -- one that names a row variable, `#fx{e}` or `#fx{IO e}`
  (`effect_row_is_open`, `src/compiler/elab_call.c`) -- accepts a poly-carrier
  parameter whose signature matches the slot's (`fn_sig_kinds_match`). The
  nominal-slot conversion that already ran for fat-normalized parameters boxes
  it (`EX_POLY_TO_FAT`). The row variable binds to whatever the parameter
  carries, so this admits nothing a direct `(g x)` call through the same
  parameter does not already admit.
- **A closed row** -- `#fx{}`, `#fx{IO}` -- is refused, TUR-E0009, naming the
  parameter and the row, with a help line that says to give the parameter a
  row:

  ```
  error [TUR-E0009]: function 'pure-hof' arg 1: 'g' is a parameter typed
  (fn [int] : int), with no effect row, so it may carry any effects; it cannot
  be passed where 'pure-hof' declares the closed row #fx{}
  ```

  That keeps the hole the 2026-10-29 note warned about closed: `noisy` cannot
  reach a `#fx{}` slot through a row-less parameter.

Pinned by `tests/fixtures/fn-param-forwarded-to-row-variable-param` (both
engines: a top-level fn, an `IO` one, a lambda, a capturing closure, a
`#fx{IO e}` slot) and `tests/fixtures/errors/fn-param-forwarded-to-closed-row-param`.

Left as it was:

- The box is the malloc'd one, 32 B per call, as at a type-variable slot
  ([fn-param-at-type-variable-slot-boxed-per-call](../reported/fn-param-at-type-variable-slot-boxed-per-call.md)).
- A callback that performs a user effect and is forwarded through a row
  variable does not compile ("this effect operation has no lowering here").
  That is not the bridge: the documented workaround, a `#fx{e}` row on the
  forwarding parameter, fails the same way on the same program, while a
  direct call through the plain parameter works.
