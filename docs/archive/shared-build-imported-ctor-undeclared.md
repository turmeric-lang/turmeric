# `tur build --shared` of a multi-module project: an imported constructor is undeclared

**RESOLVED 2026-10-13 -- fix direction 1, plus a stale note it uncovered.**
The repro builds as a shared library, and its per-module C links and runs with
the whole-program build's output.

- **The constructors.** Under separate compilation, `emit_implementation`
  (`src/compiler/emit_module.c`) now walks the module's items for
  constructor calls (`emit_imported_adt_ctors` / `collect_constructed_adts`)
  and emits, into its own `.c`, the constructors of every ADT it constructs
  but does not define. They stay `static`, so the per-TU copy needs no
  linkage change, and the layout they carry is the `TUR_TD_`-guarded typedef
  the included header already defined. A call counts when it has a resolved
  CtorDef, calls a nullary constructor, or is a synthesized `__pap` body
  calling one of its result ADT's constructors by name. The walk descends
  into lambdas, which `cps_visit_children` treats as boundaries.
- **The stale note.** With the constructor declared, the build still failed:
  `tur_adt_Shape __ps = app__geo__area2(&s)` for an `int64_t` result. A
  hoisted call leaves `call_ret_note` set to its temp's type, so the
  enclosing call's argument loop can read it (SR2b). Nothing cleared it after
  the LAST argument, so it became the enclosing call's own note, and the
  panic-hoist used it whenever the callee's return type was not recorded. An
  imported function under separate compilation is exactly that callee. The
  argument loop in `emit_expr.c` now clears the note once it has captured it
  (`arg_emitted_cty`). A whole-program build always has the callee's
  forward-declared type, which is why this never showed there.
- Pinned by `tests/run-shared-imported-ctor.sh` (ctest
  `tur_shared_imported_ctor`, fixture `tests/fixtures/shared-imported-ctor`):
  a wide by-value variant, a nullary one, a record ADT, a recursive heap
  list, a parametric box, a constructor inside a lambda and a partially
  applied one, plus a `match` on an imported ADT. The harness builds
  `--shared`, checks `app__main.c` defines `ctor_Shape_Rect`, links and runs
  the per-module C, and runs the whole-program build; both print the same.
  Before the fix the shared build fails with 13 C errors.

---

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
