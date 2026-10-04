# `#lang r7rs`: a program whose file name starts with a digit fails to compile once it imports a user library

**RESOLVED 2026-10-04.** The fix is in `scheme_lower_program` (`src/compiler/scheme_lower.c`), where a program that imports a library gets its module name from the file stem. A stem that starts with a digit is now prefixed `r7rs-program-`. Only the last dot counts as the extension now, so `1.1.scm` becomes `r7rs-program-1-1` and no longer shares `1` with `1.2.scm`. The module is internal to the program (nothing imports it), so this renames no user-visible name. `9lives.scm`, `1.1.scm` and `lives9.scm` all print `4`. Pinned by `tests/fixtures/9lives-r7rs-program`. The `sicp-guide` rough-edges entry is gone.

**Severity:** medium. The compiled back end emits C identifiers that begin
with a digit, and `cc` rejects the unit with a wall of `expected identifier
or '('` errors that never mention the file name. SICP readers name files
after sections (`1.1.scm`, `3.5-streams.scm`), which is how this was found:
the first program added to the SICP corpus (turmeric-lang/sicp-corpus,
plan D3) failed this way. `tur --interpret` is fine. Found 2026-10-03.

## Repro

```
mylib/util.scm:
(define-library (mylib util)
  (export twice)
  (import (scheme base))
  (begin (define (twice x) (* 2 x))))

9lives.scm:
(import (scheme base) (scheme write) (mylib util))
(define f (lambda (x) (+ x 1)))
(display (twice (f 1)))
(newline)
```

```
$ tur run -I . 9lives.scm
.../tur-build/..._9lives_scm.c:6872:21: error: expected identifier or '('
 6872 | static tur_tagged_t 9lives____fn_7(void);
```

| File | Imports a user library | Compiled | Interpreted |
|---|---|---|---|
| `9lives.scm` | yes | **C errors** | `4` |
| `1.1.scm` | yes | **C errors** (`1____fn_333`) | `4` |
| `lives9.scm` | yes | `4` | `4` |
| `9lives.scm` | no | `2` | `2` |

(Measured on origin/main b87ca3553. A plain `#lang turmeric` file `3d.tur`
with a `fn` compiled fine too.)

## Root cause (partial)

When the program imports a user library it is compiled as a named module,
the module name taken from the file stem (`9lives`, or `1` for `1.1`), and
anonymous functions get that name as a C prefix: `<module>____fn_<id>`
(the `__fn_%u` name is made in src/compiler/elab_fns.c:13245). Nothing
guarantees the prefix starts with a letter or underscore.
`mangle_module_name` (src/compiler/emit_module.c:20001) is one mangler that
passes a leading digit through; whether it is the one producing this prefix
is not confirmed.

## Fix directions

- Wherever a module name becomes a C identifier, prefix a leading digit
  (`_9lives`, or `m_1`), in one shared helper so every mangling path agrees.
- `1.1.scm` yielding module `1` also means `1.1.scm` and `1.2.scm` both get
  module name `1`. Harmless for separate programs; worth a look if two such
  files are ever compiled into one unit.
- Pin with a fixture named with a leading digit that imports a library.

## Guide upkeep

`docs/guides/sicp-guide.md` documents this defect in its "Rough edges" list
(name files starting with a letter). When this is fixed, delete that entry.
turmeric-lang/sicp-corpus names its files `sec-<section>.scm` because of it;
that naming can stay.
