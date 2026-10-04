# `tur lsp` publishes another file's diagnostics under the open document's URI

**Severity:** low-medium.
**Found:** 2026-10-03, against v0.60.1, while resolving
[lsp-ignores-the-file-extension](../archive/lsp-ignores-the-file-extension.md)
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
