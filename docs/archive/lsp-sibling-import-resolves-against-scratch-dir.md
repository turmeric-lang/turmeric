# A sibling `import` fails under `tur lsp`: entry-relative paths resolved against the scratch file's directory

**Severity:** medium.
**Status:** RESOLVED (2026-10-03). `compile_to_c` takes the entry file's
directory from the document when it is analysing a scratch copy. See
[Resolution](#resolution).
**Found:** 2026-10-03, against v0.60.1 (also present in v0.55.1).

*First filed as `lsp-relative-load-resolves-against-scratch-dir`, claiming a
relative `(load "b.tur")` failed under the LSP. That premise was wrong, and
this report replaces it. `load` is cwd-relative by design (the REPL guide's
`:cd` section says so): `tur check`, `tur run` and `--interpret` all fail the
same way when run from another directory, and the LSP behaves exactly like
`tur check` run from the server's working directory. The probe that seemed to
show a difference had run `tur check` from inside the file's own directory.
Checking that turned up the real bug below, which has the shape the original
report described, but for `import` rather than `load`.*

## What happens

```sh
$ cat mod/greeter.tur
(defmodule greeter
  (export greet)
  (defn greet [] : int 1))

$ cat mod/input.tur
(defmodule main-mod
  (export)
  (import greeter :refer [greet])
  (defn main [] : int (greet)))

$ cd / && tur check /path/to/mod/input.tur      # clean
```

Opened in `tur lsp`, the same document published:

```
module 'greeter' not found
  searched:
    /var/folders/.../T/          <-- the scratch file's directory
```

`#use-reader-macros "macros.tur"` had the same problem:
`reader-macros: cannot open '/var/folders/.../macros.tur'`.

So in the editor, every module that imports a sibling had a false error on its
`import` line, and none of the imported module's names were known.

## Why

`run_doc_analysis` (`src/lsp/lsp.c`) compiles a scratch copy of the buffer
from `tur_temp_dir()`. `compile_to_c` derived the entry file's directory from
the path it was given, `dir_of_path(path)`, and used it twice:

- as `PassContext.module_base_dir`, the first place `import` looks;
- implicitly, as the directory `#use-reader-macros` resolves against, because
  the entry `SourceFile` had no `base_dir`.

`tur_collect_symbols` already took the document's real path (`logical_path`)
and used it to anchor spice-include discovery, but nothing passed it on to
these two. Same family as
[lsp-ignores-the-file-extension](lsp-ignores-the-file-extension.md): a
property of the real path that the scratch path does not carry.

## Resolution

`tur_collect_symbols` sets `g_logical_entry_path` around its `compile_to_c`
call (`src/main.c`), and `compile_to_c` takes its base directory from that
when it is set: for `module_base_dir`, and as the entry `SourceFile.base_dir`
that `#use-reader-macros` already honors. Every other caller passes a real
path, and `g_logical_entry_path` is NULL for them, so they are unchanged.

Pinned by `tests/lsp/cross-file-diagnostics.py` (ctest
`lsp_cross_file_diagnostics`): a sibling import and a sibling reader-macro
file both resolve with the server started from `/`. Against v0.55.1 both rows
fail with the errors above.
