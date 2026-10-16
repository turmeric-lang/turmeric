# A nested `import` resolves from the entry file's directory, and says otherwise

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
