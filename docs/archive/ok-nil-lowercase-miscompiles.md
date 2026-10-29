# `(ok nil)` for a `(Result nil E)` emits uncompilable C

**Severity:** low-medium (expressiveness; a C compile error, never a wrong
answer). `(Ok nil)` -- the constructor -- works and is the workaround.

Found 2026-10-25 building `spices/lmdb`, whose every setter returns
`(Result nil LmdbError)`.

## Repro

```turmeric
(defdata E (Bad int))
(defn f [n : int] : (Result nil E) (if (> n 0) (ok nil) (err (Bad n))))
(defn main [] : int (do (println (if (ok? (f 1)) "ok" "err")) 0))
```

```
error: too many arguments to function 'ok__spec__tur_adt_Result__nil__E_void'
error: parameter 1 ('x') has incomplete type
```

`tur check` accepts it.

## Root cause

`ok` (stdlib/result.tur) is monomorphised at `A := nil`, and the spec's
parameter is spelled at nil's C name, `void x`. The call site still passes
one argument. The ctor path already maps a nil payload to the int64 slot
(`adt_field_c_type`, "result-nil-ok-payload-emits-void-field"), which is why
`(Ok nil)` works; the generic-function spec path has no such rule.
`ok-val` at `A := nil` had the same defect in its result temp and was fixed
2026-10-25 (`question-op-on-unit-result`); the parameter side is this.

## Fix directions

- Spell a spec parameter whose type resolves to nil as `int64_t` (the slot
  nil already rides), and pass `0` -- the mirror of the result-temp fix.
- Or rewrite `(ok nil)` / `(some nil)` to the constructor at elaboration.

## Resolution (2026-10-29)

Fixed the first way the report suggested, and for every function, not just
`ok`: a parameter whose type is `nil` is spelled `int64_t`
(`emit_param_c_spell`, `src/compiler/emit_internal.h`), at the four places a
direct function's parameters are written (the forward declarations of plain
functions and ABI specs in `emit_module.c`, the definitions in `emit_fns.c`).
A call site handing a nil-typed argument to a parameter the signature table
recorded as `int64_t` passes `((void)(arg), INT64_C(0))` (`emit_expr.c`),
the twin of the constructor path's rule. The argument is still evaluated, so
its side effects still happen.

That covers `(ok nil)`, `(some nil)`, `(id nil)` at any generic, and a
declared `(defn g [x : nil] ...)`, which failed the same way. Pinned by
`tests/fixtures/ok-nil-lowercase`, which passes compiled and under
`--interpret`.
