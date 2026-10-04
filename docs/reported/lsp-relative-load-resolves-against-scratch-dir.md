# A relative `load` fails under `tur lsp`: it resolves against the scratch file's directory

**Severity:** medium.
**Found:** 2026-10-03, against v0.60.1, while measuring
[lsp-publishes-other-files-diagnostics-under-one-uri](lsp-publishes-other-files-diagnostics-under-one-uri.md).
**Impact:** every document that `load`s a sibling by relative path gets a
bogus `load: cannot open` error in the editor, and none of the loaded file's
symbols, while `tur check` on the same file is clean.

## Repro

```sh
$ cat xf/a.tur
(load "b.tur")
(defn main [] : int 0)

$ tur check xf/a.tur        # resolves b.tur beside a.tur -- works
```

Opened in `tur lsp`, the same text publishes
`load: cannot open 'b.tur'` on line 1.

## Why

`run_doc_analysis` (`src/lsp/lsp.c`) writes the buffer to a scratch file in
`tur_temp_dir()` and compiles that, so a relative `load` is resolved beside
the scratch file, in the temp directory. `tur_collect_symbols` already
re-anchors spice-include discovery on the document's real path (the
`logical_path` argument, `src/main.c:1029`); `load` resolution was not given
the same anchor. This is the same shape as
[lsp-ignores-the-file-extension](../archive/lsp-ignores-the-file-extension.md):
a property of the real path that the scratch path does not carry.

## Fix directions

Make the entry file's base directory for relative `load` (and `#use-reader-macros`)
the document's directory rather than the scratch file's -- the interpreter
already has the concept (`env->module_base_dir`, `sfile->base_dir`), so the
compiled analysis path probably needs a `base_dir` on the entry `SourceFile`
set from `logical_path`. Writing the scratch file beside the document would
also work, but would litter the user's tree and race with their own tools.
