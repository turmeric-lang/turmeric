# The definition forms: `def : T` reinterprets a float as an int, and a one-call `binding` body is dropped

**Severity: medium-high (two silent wrong results).** Found 2026-10-19 checking the
examples for the definition forms' API reference (`stdlib/forms/definitions.tur`
and its siblings). Each repro was run on `tur run` and `tur --interpret`.

## 1. `(def x : int 7.1)` stores the bits of 7.1

```turmeric
(def x : int 7.1)
(def y : float 3)
(println x)
(println y)
(defn main [] : int 0)
```

Both engines print `4619679907765970534` and `1.4822e-323`: the initializer is
reinterpreted, not converted and not rejected. The same annotation on a `let`
binding is `let binding 'a': type annotation does not match initializer`, and
`(:: 7.1 :int)` is the "ambiguous: convert or reinterpret" error. Root cause:
the top-level `def` path in `src/compiler/elab_fns.c` (~line 15371) wraps an
initializer whose kind differs from the declared kind in `EX_REINTERPRET`
whenever the two have the same size in bytes (`type_size_bytes`), which is true
of every `int`/`float` pair. `tests/check-emitted-float-conversions.py` reports
nothing because it looks for conversions, and this is a reinterpretation.
Fix: apply the `::` rule (an `int`/`float` pair is an error) before the
size test, and let only the representation-equal pairs through.

## 2. A compiled `binding` whose whole body is one call to a `nil` function drops the call

```turmeric
(defdynamic *level* :int 1)
(defn show [] : nil (println *level*))
(defn main [] : int
  (binding [*level* 7] (println *level*))   ; prints 7
  (binding [*level* 8] (show))              ; prints nothing when compiled
  (binding [*level* 9] (show) (show))       ; prints 9 twice
  0)
```

`tur run` prints `7`, `9`, `9`; `tur --interpret` prints `7`, `8`, `9`, `9`. At file
level the single-call form is dropped the same way. In `emit_expr.c`,
`case EX_DYNVAR_BINDING` (~line 18083) calls `emit_value` on the body and uses the
returned C expression only when the binding has a value
(`if (!nil_result) { ... result_tmp = bval; }`); for a `nil` body whose value is a
call expression that has not been emitted as a statement, nothing writes `show()`
out. `emit_c` of the program shows the `TurDynFrame` setup followed directly by
the pop. A body that is a `do`, a builtin such as `println`, or returns a value
is emitted. Fix: when `nil_result`, emit `(void)(bval);` if `bval` is not the
atom for nil, and add the case to `dynvar-*` fixtures.

## 3. Smaller things

- An unknown type name in an annotation is an implicit type variable, so
  `(defn f [x : blah] : int 1)` is accepted
  ([unknown-type-annotation-becomes-implicit-tyvar](unknown-type-annotation-becomes-implicit-tyvar.md),
  already filed).
