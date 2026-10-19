# `transmute` / `reinterpret` convert instead of reinterpreting, and the interpreter gets `ptr-null?` and `raw-realloc` wrong

**Severity: medium (silently wrong values in unsafe code; the engines
disagree).** Found 2026-10-19 checking the examples for the unsafe builtins'
API reference (`stdlib/forms/unsafe.tur`).

## What happens

Inside `(unsafe ...)`:

| Expression | Compiled | Interpreted | Documented as |
| --- | --- | --- | --- |
| `(transmute 1.5 :int)` | `1` | `1.5` | bitwise reinterpretation: 4609434218613702656 |
| `(transmute 4609434218613702656 :float)` | `4.60943e+18` | `4609434218613702656` | 1.5 |
| `(reinterpret 1.5 :int)` | `1` | `1.5` | bitwise reinterpretation |
| `(ptr-null? (raw-malloc 8))` | `false` | `true` (and UBSan: `load of value 48, which is not a valid value for type '_Bool'`) | `false` |
| `(raw-realloc (raw-malloc 8) 64)` | the new block | `0` (nil); the old block leaks | the new block |

So `transmute` and `reinterpret` are a C value conversion when compiled and
the identity when interpreted; neither reinterprets bits, which is what the
elaborator's own comments say they do. `tur/bits`'s `float->bits` /
`bits->float` are the working way to read a float's bit pattern today.

## Repro

```turmeric
(unsafe
  (println (transmute 1.5 :int))
  (println (reinterpret 1.5 :int))
  (println (let [p (raw-malloc 8)] (let [n (ptr-null? p)] (raw-free p) n)))
  (println (let [p (raw-realloc (raw-malloc 8) 64)]
             (array-set-unchecked p 7 3)
             (let [v (array-get-unchecked p 7)] (raw-free p) v))))
```

`tur run`: `1 1 false 3`. `tur --interpret`: `1.5 1.5 true 0`.

## Root cause

- **`transmute` / `reinterpret`, compiled.** `emit_builtin`
  (`src/compiler/emit_core.c`) writes `((T)x)` for `BS_TRANSMUTE` and
  `((int64_t)x)` for `BS_REINTERPRET`: a C cast, which converts a `double`'s
  value. A bit-cast needs `memcpy` or a union, as `float->bits` does.
  `tests/check-emitted-float-conversions.py` would report it on any program
  that transmutes a float.
- **`transmute` / `reinterpret` / `unsafe-cast`, interpreted.** One arm returns
  `args[0]` unchanged (`src/turi/eval.c:5356-5360`), keeping the float tag.
- **`ptr-null?`, interpreted.** Its spec shares `c_op` `"!"` with `not`
  (`src/compiler/builtins.c`, the `ptr-null?` row), and the interpreter's
  `BS_PREFIX_UNARY` arm answers `"!"` with `!args[0].as_bool`
  (`src/turi/eval.c:5344`) -- the low byte of the pointer read as a `_Bool`.
- **`raw-realloc`, interpreted.** `BS_RAW_REALLOC` appears in the capability
  gate (`eval.c:5192`) but has no arm in `eval_builtin`, so the call yields
  nil.

## Fix directions

1. Emit a bit-cast for `transmute` and `reinterpret` between kinds of equal
   size (`({ T __t; memcpy(&__t, &x, sizeof __t); __t; })` or a union), and
   mirror it in the interpreter (`TURI_FLOAT` <-> `TURI_INT` by bits).
   `reinterpret` should also yield its target type, as `transmute` does,
   rather than `:int`.
2. Give `ptr-null?` its own test in the interpreter
   (`args[0].as_int == 0`), keyed on the spec, not on `c_op`.
3. Add the `BS_RAW_REALLOC` arm (`realloc((void *)p, n)`).
4. A fixture over the unsafe builtins on both engines (run-turi.sh skips a
   file with inline C, so keep it free of one).
