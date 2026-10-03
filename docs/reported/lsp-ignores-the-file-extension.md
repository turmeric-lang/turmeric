# `tur lsp` ignores the file extension when choosing a reader

**Severity:** medium.
**Found:** 2026-10-03, against v0.60.1 (and reproduced on v0.59.0).
**Impact:** every headerless `.scm` and `.tur.sweet` file gets a wall of errors
from the wrong reader, and no symbols at all.

## What happens

`tur lsp` selects its reader from the `#lang` line in the document text and
nothing else. The compiler honours the extension — `reader_type_from_extension`
(`src/compiler/reader.c:5792`) maps `.tur.sweet` to `READER_SWEET` and `.scm` to
`READER_R7RS` — so the two disagree about the same file.

Driven over stdio with `initialize` + `didOpen` + `textDocument/documentSymbol`:

| document | diagnostics | symbols |
|---|---|---|
| `p.scm`, `(define (f x) (* x 2))`, no header | **2 errors** | **none** |
| `p.scm`, same body with `#lang r7rs` | clean | `f`, `main` |
| `p.tur.sweet`, `defn double [x]` / `  {x * 2}`, no header | **errors** | **none** |
| `p.tur.sweet`, same body with `#lang turmeric/sweet` | clean | `double` |
| `p.tur` with `#lang saffron` | clean | `double` |
| `p.tur`, Scheme body, no header (control) | 2 errors | none |

The errors on the headerless `.scm` are

```
define name must be a symbol
unknown function or operator 'f'
```

and on the headerless `.tur.sweet`, `TUR-E0003 unbound symbol 'defn'`.

The control row is the isolation: a Scheme body in a `.tur` file produces the
same two errors, so the extension is being ignored rather than the header being
required.

## Why it matters more than it looks

`.scm` is precisely the extension that makes a header unnecessary — it selects
the Scheme *language* as well as its reader (`src/turi/eval.c:15195`,
`src/main.c:236`). So the idiomatic way to write a Scheme file is the way that
breaks the language server. The same holds for `.tur.sweet`: none of the
sweet-exp fixtures in Trowel's own test tree carry a header, because they have
never needed one.

## Where it is

The LSP's analysis path never consults the extension at all —
`reader_type_from_extension` appears exactly once in `src/lsp/`, and it is not
on that path.

**The formatting handler already gets this right**, which is what makes the gap
easy to close. `on_formatting` passes `reader_type_from_extension(doc->path)`
into `fmt_format_document` (`src/lsp/lsp.c:2110`), and that function calls
`detect_lang_dialect` on the source itself and prefers the header whenever the
extension said plain Turmeric (`src/compiler/fmt.c:2235-2240`) — upstream's own
`chosen = (ext_type != READER_TURMERIC) ? ext_type : lang_type` precedence.
Verified over stdio: `textDocument/formatting` on a headerless `.scm` re-indents
it as Scheme, and on a `.tur` carrying `#lang r7rs` it also re-indents it as
Scheme and preserves the header.

So the server already contains a correct reader resolution for a document, used
by one handler and not by the other.

## Suggested fix

Resolve the reader once where the document is opened, with the same precedence
`fmt_format_document` already implements, and have the analysis path read that
instead of looking only at the text. The formatting handler then keeps working
unchanged.

## Secondary observation

A `publishDiagnostics` for one document can carry diagnostics belonging to
*other* files, distinguished only by a non-standard `"file"` key on each
diagnostic. With a consistent stdlib none are produced, so this is currently
latent — but a client that trusts the published URI will attribute an imported
module's errors to whatever buffer is open. It is presumably why the `"file"`
key exists; a separate `publishDiagnostics` per URI would be the standard
shape.

## Client-side consequence

*(Paths below are in [Trowel](https://github.com/rjungemann/trowel), a
separate repo — the editor this was found from. Included so the report names a
real consumer and so the workaround can be deleted alongside a fix.)*

Trowel works around this by sending such a document with the header it implies
prepended, and shifting every line number back across the boundary
(`LspManager::shiftedText`, `src/lsp/lsp_manager.cpp`). That is a per-document
line shift threaded through every outgoing position and every incoming range,
so it is a real cost rather than a one-liner; it goes away with this report.
