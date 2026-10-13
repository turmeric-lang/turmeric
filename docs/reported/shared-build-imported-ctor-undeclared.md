# `tur build --shared` of a multi-module project: an imported constructor is undeclared

**Severity: medium.** A correct multi-module project fails to build as a
shared library as soon as one module calls a constructor of an ADT defined in
another. The same project builds and runs as an executable (`tur build <dir>`,
whole-program). No effects, CPS or wide aggregates are needed. Found
2026-10-13 while checking separate compilation for
[cps-performing-fn-with-wide-adt-param-refused](../archive/cps-performing-fn-with-wide-adt-param-refused.md).

## Repro

`build.tur`:

```turmeric
(defpackage wideproj
  :name "wideproj" :version "0.1.0" :description "repro"
  :exports #map{ "app/main" ["main"] "app/geo" ["area2" "Rect" "Circle"] })
```

`src/app/geo.tur`:

```turmeric
(defmodule app/geo
  (export area2 Rect Circle)
  (defdata Shape (Circle :int) (Rect :int :int))
  (defn area2 [s : Shape] : int
    (match s (Circle r) (* 3 r) (Rect w h) (* w h))))
```

`src/app/main.tur`:

```turmeric
(defmodule app/main
  (import app/geo :refer [area2 Rect Circle])
  (defn main [] : int
    (println (area2 (Rect 2 4)))
    0))
```

| | result |
| --- | --- |
| `tur build <dir> -o app` | builds, prints `8` |
| `tur build --shared <dir> -o lib.so` | cc: `implicit declaration of function 'ctor_Shape_Rect'`, then `invalid initializer` |
| `Shape` narrowed to `(Rect :int)` | same, plus `incompatible type for argument 1 of 'app__geo__area2'` |

## Root cause

A constructor is emitted as a `static` function in the defining module's
translation unit only (`static tur_adt_Shape ctor_Shape_Rect(...)`:
`emit_adt_typedef_and_ctors` and the `emit_program` mirror in
`src/compiler/emit_module.c`, near the two `mangle_ctor_symbol` calls).
`app__geo.h` carries the `tur_adt_Shape` typedef (under `TUR_TD_tur_adt_Shape`)
and the exported function's prototype, but nothing declares the constructor.
`app__main.c` calls `ctor_Shape_Rect` anyway: an implicit `int` declaration,
which then cannot initialize a `tur_adt_Shape`.

## Fix directions

1. Emit an imported ADT's constructors into the importer's TU too, as the
   typedef already is. They are `static` and small (a struct literal for a
   by-value ADT, a malloc + fill for a heap one), so a per-TU copy needs no
   linkage change. The header could carry them as `static inline` under the
   same `TUR_TD_` guard.
2. Or give constructors external linkage with a module-qualified symbol and
   declare them in the header. More symbols in the `.so`, and the
   `ctor_<Adt>_<Ctor>` name would need the module prefix to stay unique.
