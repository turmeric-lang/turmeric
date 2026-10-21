# Diagnostics in sweet-exp files report columns of the transformed text

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by `errors/sweet-diagnostic-column`.

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

## Resolution (2026-10-21)

The fix direction, adjusted: what is translated is the reported line and
columns, not the whole span. `render_snippet_ex` already maps a span's
offsets through the sweet map against the original text, so a span whose
offsets had been translated at the entry point would be mapped twice.

`diag_user_pos` (`src/compiler/diag.c`) returns the span with the line and
columns `diag_translate_span` computes, its offsets untouched. Every place a
position leaves `diag.c` goes through it: the stderr header of each
`diag_emit*` entry point (and their notes, suggestions and secondary spans,
and `diag_emit_multi_span`'s label column), `lsp_append` (the LSP range and
`tur check --json`), `diag_sink_dispatch` (the embed sink) and
`diag_emit_json` (`--json-diagnostics`). A file with no transform is
returned unchanged.

The repro now reports `sw.tur:3:14`, character 13 in `--json` and column 14
in `--json-diagnostics`. The fixture pins the header column on both engines
(`expected.diag` has `:5:14:`, `unexpected.diag` the old `:5:15:`). The LSP
harnesses (`r7rs-`, `cross-file-`, `saffron-diagnostics`),
`run-sweet-manifest.sh` and `run-parse-check.sh` still pass.

Not changed: a sweet-exp snippet shows the `#lang` line blank, since the
original text it renders from has that line stripped.
