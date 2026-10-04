# `tur lsp` publishes another file's diagnostics under the open document's URI

**Severity:** low-medium.
**Status:** RESOLVED (2026-10-03). Foreign diagnostics are anchored on the form
that names their file, with the real location as `relatedInformation`. See
[Resolution](#resolution).
**Found:** 2026-10-03, against v0.60.1, while resolving
[lsp-ignores-the-file-extension](lsp-ignores-the-file-extension.md)
(where it was first noted, as a secondary observation, and called latent).
**Impact:** an error in a loaded or imported file is drawn on the open
buffer, at the line and column it has in the *other* file.

## Repro

```sh
$ cat xf/b.tur
(defn helper [] : int (undefined-fn-xyz 1))

$ cat xf/a.tur          # opened in the editor
(load "/abs/path/xf/b.tur")
(defn main [] : int 0)
```

`didOpen` on `a.tur` publishes a single `publishDiagnostics`:

```json
{"uri": "file:///abs/path/xf/a.tur",
 "diagnostics": [{"message": "unknown function or operator 'undefined-fn-xyz'",
                  "range": {"start": {"line": 0, ...}}, "file": "/abs/path/xf/b.tur"}]}
```

So the error belongs to `b.tur` line 1, but a client that trusts the URI
underlines line 1 of `a.tur`. The only hint is the non-standard `"file"` key,
which no stock client reads. It is not latent: any `load` or `import` of a
file with an error triggers it.

## Where it is

`lsp_build_array` (`src/compiler/diag.c:3307`) writes every collected
diagnostic into one array. `lsp_append` records each one's source path in
`e->file`, and the array emits it only as the `"file"` key. The LSP handler
then publishes the whole array under the document's URI.

## Fix directions

Group the entries by `e->file` and send one `publishDiagnostics` per URI:
entries for the open document under its URI (after the existing scratch-path
remap), and others under their own `file://` URI, which is the standard
shape. A server that does this also has to clear those other URIs on the next
clean analysis, or their diagnostics outlive the fix. The smaller alternative
is to drop foreign-file entries and add one diagnostic on the `load` /
`import` form that says the dependency has errors.

## Resolution

The second direction, not the first. One `publishDiagnostics` per URI is the
textbook shape, but in this server it would be wrong. When the other file is
also open in the editor, its own analysis publishes its diagnostics, and the
last publish for a URI wins, so each document's analysis would overwrite the
other's. A file shared by several open documents would flicker between
their views of it. Clearing those URIs again on the next clean analysis
adds bookkeeping for no gain.

So the diagnostics stay on the open document, as clangd does for errors in an
included header:

- `diag_lsp_flush_array_for` (`src/compiler/diag.c`) moves every entry whose
  `file` is not the document onto it, through a relocation callback;
- the LSP's callback (`relocate_foreign_diag`, `src/lsp/lsp.c`) anchors it on
  the form that pulled the file in: `lsp_reference_anchor`
  (`src/lsp/lsp_util.c`) finds the `load` / `import` / `include` line naming
  it, as a string (`"b.tur"`) or a module path (`foo/bar`), in s-expression or
  sweet-exp source, matched by whole path components;
- the message is prefixed with the real location, relative to the document's
  directory: `in lib.tur:1:24: unknown function or operator ...`;
- the real location goes along as standard LSP `relatedInformation`, so a
  client can jump to it. The `"file"` key stays for existing consumers.

A file this document does not name directly -- one loaded by a loaded file --
is anchored through its origin chain (follow-up, 2026-10-04). The loader of
each `load`ed or imported file records the span of the form that named it
(`diag_set_file_origin`, `src/compiler/diag.c`); `lsp_append` walks that chain
up to the file the document names, and the anchor is the document's `load` /
`import` of it -- found by the same text scan, with the recorded span as the
fallback. The message names the step: `in deep.tur:1:29 (via mid.tur): ...`.
Only a file the chain does not reach (the auto-loaded stdlib) falls back to
the first line.

A sweep of every negative fixture for diagnostics located in a stdlib file
(2026-10-04) found that this last case is in practice an error inside a MACRO
EXPANSION: template spans survive expansion, so the error sits in the
DEFMACRO's file (`stdlib/map.tur` for `map-get`), auto-loaded or not. The
elaborator now reports the outermost macro call to diag
(`diag_set_expansion_site`, set and restored in `elab_call.c`), and the LSP
anchors such an error on that call -- ahead of any `load` anchor, since the
call is the code the user wrote -- with `(expanding map-get)` in the message.

The same sweep found the only other kind: TUR-W0039 fired on `arrow.tur`'s own
deliberate `arr` / `>>>` fallback defns whenever the file was loaded
explicitly, because the lint skipped only the auto-loaded band
(`is_from_stdlib`). It now also skips bindings whose file is under `stdlib/`
(`elab_toplevel.c`), so a clean `(load "stdlib/arrow.tur")` is clean.

That leaves an error in an auto-loaded stdlib file that no macro call leads
to -- a genuine stdlib bug, or a `TUR_STDLIB_DIR` pointing at a stdlib from
another release. It is reachable (plant an error in a copy of the stdlib and
open a clean file), and it was published as a one-character squiggle at line 0
reading `in bstd/option.tur:271:32: ...`, which looks like the user's mistake.
It now marks the document's whole first line, names the file
`stdlib/option.tur` wherever the stdlib is installed, and adds: "this error is
in the standard library, not in this file: check that the stdlib at <dir>
matches this compiler (tur --version), and report it as a stdlib bug if it
does". Every stdlib file is named `stdlib/<file>` in these messages. `tur mcp` and `tur check --json` keep
the old shape: they are not publishing for a document, so the entries keep
their own coordinates.

Pinned by `tests/lsp/cross-file-diagnostics.py` (ctest
`lsp_cross_file_diagnostics`): a loaded file's error is anchored on the `load`
string with the prefix and related location, a sibling import's error on the
module name, a load-of-a-load and an import-of-an-import on the document's
own form with `(via ...)` in the message, an auto-loaded stdlib macro's error
on the call, the stdlib's own error (a broken copy via `TUR_STDLIB_DIR`) on the
first line with the explanation, a clean explicit stdlib load with no diagnostics, and an
own-file error stays where it is (the control).
