# Diagnostics in sweet-exp files report columns of the transformed text

**Severity:** medium. Error locations are wrong for every `#lang sweet-exp`
(and `.tur.sweet`) file, including in the editor through the LSP.

## Repro

```
#lang sweet-exp
defn main [] : int
  let [x +(1 "x")]
    x
```

```
$ tur check sw.tur
sw.tur:3:15: error [TUR-E0001]: '+' arg 2: type mismatch - expected int, got cstr
```

The string `"x"` starts at column 14 of line 3. The caret under the snippet is
mapped back correctly. The header's column, the JSON output and the LSP range
are not; the LSP reports character 14 where the string starts at 13.

## Root cause

The sweet-exp preprocessor rewrites the text and records an offset map.
`diag_translate_span` maps a span back to the user's source, but
`src/compiler/diag.c` calls it only when rendering the snippet. The
`diag_emitv` / `diag_emit_with_code` family, `lsp_append` and
`diag_emit_json` all use the untranslated span.

## Fix directions

- Translate the span once, at each `diag_emit*` entry point, before it reaches
  any sink. Add a sweet-exp `errors/` fixture that pins a column.
