# A nested `import` resolves from the entry file's directory, and says otherwise

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`errors/module-not-found`.

**Severity:** low. The search directory contradicts both
[module-system-guide](../guides/module-system-guide.md) ("relative to the
importing file") and the diagnostic's own label.

## Repro

```
app.tur          (import a/x ...)
a/x.tur          (defmodule a/x (import y :refer [yv]) ...)
a/y.tur          (defmodule y ...)
```

```
$ tur check app.tur
./a/x.tur:3:3: error: module 'y' not found
  searched:
    ./y.tur    (importing file's directory)
```

The importing file is `a/x.tur`, so the search should have looked at
`./a/y.tur`. With `y.tur` moved next to `app.tur`, the import resolves.

## Root cause

`src/compiler/elab_module.c:elab_load_module_impl` sets `module_base_dir` once
per compile, from the entry file. Nested imports reuse it.

## Fix directions

- Decide the rule. If imports are meant to be relative to the importing file,
  push and pop `module_base_dir` per loaded module. If they are meant to be
  relative to the entry, fix the label and the guide.

## Resolution (2026-10-17)

The rule is the entry file's directory, the project root: the module-system
guide's own "Name -> file-path convention" says a module name is the file path
under the project root, so `a/x.tur` imports its sibling as `(import a/y)`.
Resolution was right; the label and the prose were wrong. The "not found"
listing calls the row `(project root)`, the `:for-macros` error says "project
root", and the module-system guide's resolution paragraph and its "module not
found" entry describe the root rule, including for nested imports. Pinned by
`errors/module-not-found`, whose needle names the new label.
