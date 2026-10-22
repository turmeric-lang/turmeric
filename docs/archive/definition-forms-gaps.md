# The definition forms: `def : T` reinterprets a float, a one-call `binding` body is dropped, and a float-backed `defopaque` truncates

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`def-annotation-float-literal`, `dynvar-binding-nil-call-body`,
`nested-defn-recursion`, `nested-type-defs-in-body` and `defopaque-float-base`
(both engines), and `errors/def-annotation-int-float-mismatch`,
`errors/defstruct-duplicate-field` and `errors/defopaque-unknown-base`.

**Severity: high (three silent wrong results).** Found 2026-10-19 checking the
examples for the definition forms' API reference (`stdlib/forms/definitions.tur`
and `datatypes.tur`). Each repro was run on `tur run` and `tur --interpret`.

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

## 3. A `defn` nested in a body cannot call itself

```turmeric
(defn outer [] : int
  (defn fact [n : int] : int (if (= n 0) 1 (* n (fact (- n 1)))))
  (fact 5))
(defn main [] : int (println (outer)) 0)
```

`tur run`: `unknown function or operator 'fact'`. `tur --interpret` warns
`TUR-W0040: unknown name 'fact'; will runtime-dispatch` and prints 120. Two nested
`defn`s that call each other fail the same way. The syntax guide says a nested
`defn` is lifted to file scope; its name is bound after its body has been
elaborated, with no forward declaration (the file-scope pass registers one,
`elab_toplevel.c`, before elaborating bodies). The stdlib doctest harness hit it:
every example is a statement in one body.

## 4. A `defstruct` or `defdata` inside a function body breaks the C build

```turmeric
(defn main [] : int
  (defstruct Point :copy [x : int y : int])
  (println (.x (Point 3 4)))
  0)
```

`tur run`: C errors (`implicit declaration of function 'ctor_Point_Point'`,
`request for member 'x' in something not a structure or union`; for a `defdata`,
`unknown type name 'tur_adt_Shape'`). `tur --interpret` prints 3. A nested
`defopaque` and a nested `defn` compile. The syntax guide ("Definitions inside a
function body") says any `def*` form may appear there and is lifted to file
scope; the type declarations are elaborated but their C types and constructors
are not emitted. Fix: hoist them like `defn`, or reject them in a body with a
located error.

## 5. A float-backed `defopaque` truncates when compiled

```turmeric
(defopaque Celsius :float)
(defn mk [x : float] : Celsius (:: x Celsius))
(defn raw [c : Celsius] : float (:: c :float))
(defn main [] : int
  (println (raw (mk 21.5)))              ; 21 compiled, 21.5 interpreted
  (println (raw (:: 21.5 Celsius)))      ; 4.62674e+18 compiled
  0)
```

An opaque is an `int64_t` handle at the C level, so `mk` is `int64_t mk(double x)
{ return x; }` and `raw` is `double raw(int64_t c) { return c; }`: both are
value conversions, not bit reinterpretations. `python3
tests/check-emitted-float-conversions.py` reports two `implicit I2F` findings on
this file. opaques-guide's own `celsius->kelvin` example (`defopaque Celsius
:float`, `(:: (+ (:: c :float) 273.15) :Kelvin)`) prints `4.62674e+18` compiled and
294.65 interpreted. Fix: reinterpret the bits for a float base (`memcpy` /
`TUR_AS`), or give a float-based opaque a `double` carrier.

## 6. Smaller things

- `(defstruct P [x : int x : int])` is accepted; structs-guide says field names
  must be distinct.
- `(defopaque Bad :string)` is accepted with no diagnostic for the unknown base.
- An unknown type name in an annotation is an implicit type variable, so
  `(defn f [x : blah] : int 1)` is accepted
  ([unknown-type-annotation-becomes-implicit-tyvar](unknown-type-annotation-becomes-implicit-tyvar.md),
  already filed).

## Resolution (2026-10-21)

1. **`(def x : int 7.1)`** is `def 'x': type annotation does not match
   initializer (annotated int, got float)`, as a `let` already was. `elab_def`
   (`src/compiler/elab_fns.c`) applies the int/float rule before the
   same-size test that built the `EX_REINTERPRET`; an integer LITERAL under
   `: float` is the number (`(def y : float 3)` is 3.0), as `(:: 3 :float)` is.
2. **A one-call `nil` body in a `binding`** is emitted: `EX_DYNVAR_BINDING`
   (`src/compiler/emit_expr.c`) emits a nil body as a statement
   (`emit_stmt`), where it used to take the value expression and write
   nothing.
3. **A nested `defn` can call itself and its siblings.** Every `defn` in a
   body window is declared (`elab_pre_declare_toplevel_defn`) from
   `splice_internal_defines`, which every body passes through before its first
   form is elaborated, and `elab_defn` declares its own name when it was not
   reached that way. The declarations are the global bindings the lifted
   functions are given anyway.
4. **A `defstruct` / `defdata` / `defgadt` in a body is hoisted** to file scope
   like a nested `defn` (`elab_hoist_nested_type_def`,
   `src/compiler/elab_call.c`): the `EX_DEFDATA` is registered with
   `elab_register_file_def` and the body gets a nil in its place.
5. **A float-backed `defopaque`** records its base (`AdtDef.opaque_base_is_float`)
   and `::` between it and a float is an `EX_REINTERPRET` through the int64
   carrier (`elab_ascribe`), so `(raw (mk 21.5))` is 21.5 and opaques-guide's
   `celsius->kelvin` prints 294.65. `check-emitted-float-conversions.py`
   reports nothing on the report's program. A float-backed opaque stored in a
   struct field or a collection still travels as its int64 bits, which is
   what the handle is.
6. **Smaller things.** A repeated field name in a record constructor is
   `defdata: record-style variant 'P' declares field 'x' twice` (every
   `defstruct` lowers through it), and an unknown `defopaque` base keyword
   (`:string`) is `defopaque: unknown base type :string`. The unknown-annotation
   type-variable hole stays with
   [unknown-type-annotation-becomes-implicit-tyvar](unknown-type-annotation-becomes-implicit-tyvar.md).

