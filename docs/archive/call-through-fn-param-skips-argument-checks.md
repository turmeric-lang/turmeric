# A call through a plain function-typed parameter checks neither argument types nor arity

**Severity:** high (silent wrong value, and the two engines disagree). A typed
program passes a value of the wrong type, or the wrong number of arguments,
to a function it holds through a `(fn [...] R)` parameter, and compiles clean.

## Repro

```turmeric
(defn inc1 [x : int] : int (+ x 1))
(defn halfp [x : float] : float (* x 0.5))
(defn via-int [f : (fn [int] int)] : int (f 7.1))
(defn via-flt [f : (fn [float] float)] : float (f 7))
(defn main [] : int (println (via-int inc1)) (println (via-flt halfp)) 0)
```

| call through `f` | compiled prints | `tur --interpret` prints | should be |
|---|---|---|---|
| `(f 7)` at `(fn [float] float)` | `1.97626e-323` | `-4674736413210574848` | TUR-E0001 |
| `(f 7.1)` at `(fn [int] int)` | `4619679907765970535` (the double's bits, +1) | -- | TUR-E0001 |
| `(f "hi")` at `(fn [int] int)` | a pointer value | -- | TUR-E0001 |
| `(f 1 2)` at `(fn [int] int)` | `2` (extra argument dropped) | -- | TUR-E0002 |
| `(f true)` at `(fn [int] int)` | `2` | -- | TUR-E0001 |

The same calls ARE checked in every other spelling: through a `let`-bound
function (`(let [g inc1] (g 7.1))`), through an annotated `let`
(`h : (fn [int] int)`), through a lambda, through a parameter whose type
carries an effect row (`(fn [int] #fx{} int)`), and as a direct call. All of
those report `TUR-E0001`.

## Root cause

A plain, effect-free `(fn [A...] R)` parameter rides the typed `:fn` carrier:
its binding is `ptr<void>` with `is_poly_fn` set and the concrete signature in
`poly_type` (the `carrier_ok` routing in `elab_defn`, `src/compiler/elab_fns.c`,
"F5 (fn-first-class-application, typed carrier)"). Calls through it go to
`elab_poly_call` (`src/compiler/elab_call.c:13658`). That function elaborates
each argument and dispatches, but never compares the arguments with
`poly_type`'s arity or parameter types. The nominal `TY_FN` path,
`elab_call_fn_inner`, does (the kind check near `elab_call.c:10435`).

A side effect worth knowing: under `--enable=refined-fn-types`, a call through
a REFINED type of this shape is wrapped (`rt_elab_refined_fn_call`), so an
ill-typed argument at a refined position reaches the refinement predicate and
is rejected there, as `TUR-E0042` pointing at the predicate rather than at the
argument. That is an accident of the wrapper, not a check.

## Fix directions

- In `elab_poly_call`, when `fn_binding->poly_type` is a concrete `TY_FN` (not
  a `forall`), check arity and each argument against `poly_type`'s parameter
  types, as `elab_call_fn_inner` does for a nominal `TY_FN`, with the same
  codes.
- Widen the arguments the checker accepts the same way the nominal path does
  (an int literal under a float parameter is the number, as `def` already
  treats it), or the fix will reject calls the nominal path allows.
- Pin it with an `errors/` fixture per row above. A positive fixture should
  run on both engines, since the interpreter disagrees with compiled code today.

## Resolution (2026-10-29)

`elab_poly_call` (`src/compiler/elab_call.c`) now checks a call through an F5
typed carrier whose `poly_type` is a concrete `TY_FN`, in a non-dynamic file:

- **Arity.** Too few arguments, or too many when the result is not a
  function, is `TUR-E0002` ("function 'f' expects 1 argument(s), got 2"). An
  under-applied call used to produce a value that was not callable either
  (`'g' is not a function`).
- **Scalar arguments.** An argument of one scalar class (`int`/`int64`,
  `float`/`float64`, `float32`, `bool`, `cstr`) at a parameter of another is
  `TUR-E0001` with the nominal path's wording. That is what a direct call
  says, `(halfp 7)` included, so no widening was needed.

Every other argument shape (type variables, `any`, carriers, aggregates) keeps
its existing tolerance. The narrow check is deliberate: it covers every row
of the table above without re-deriving the nominal path's list of carrier
bridges.

Pinned by `errors/fn-param-call-{float-into-int,int-into-float,cstr-into-int,bool-into-int,too-many-args}`,
and by `fn-param-call-checked`, which runs well-typed calls on both engines.
