# A defmodule file reached by both `load` and `import` is defined twice

**Severity:** low-medium (C compile error, or a misleading Turmeric error).
`(load "m.tur")` splices a file's forms in, and `(import m)` reads and
elaborates `m.tur` again: nothing tells the import that module `m` is already
defined. The program then has two copies of every definition. Filed
2026-10-18 while fixing
[time-capability-now-is-32-bit](../archive/time-capability-now-is-32-bit.md),
whose third fix direction is this shape: `stdlib/capability.tur` does
`(load "stdlib/time.tur")`, and `time.tur` is `(defmodule time ...)`.

## Repro

```turmeric
;; m.tur
(defmodule m
  (export twice)
  (defn twice [x : int] : int (* 2 x)))
```

```turmeric
;; app.tur
(load "m.tur")
(defmodule app
  (import m :refer [twice])
  (defn main [] : int
    (println (twice 21))
    0))
```

`tur run app.tur` gets as far as cc:

```
error: redefinition of 'm__twice'
```

The stdlib shape reports a Turmeric error instead, at the second definition:

```turmeric
(defmodule both
  (load "stdlib/capability.tur")
  (import time :refer [Real-Time])
  ...)
```

```
stdlib/time.tur:81:14: error: defopaque: 'Time' is already defined
```

So a program cannot use the capability helpers (`time-now`, `make-Time`) and
also `import` the `time` module. Today it reaches the time names through the
`load` instead: after `(load "stdlib/capability.tur")` at the top level,
`(Real-Time)` and `(now-ms)` resolve with no import.

## Root cause

Only `elab_load_module_impl` (`src/compiler/elab_module.c`) adds a module to
`e->loaded_modules`, and only `elab_find_loaded_module` reads it. A
`(defmodule m ...)` that a `load` spliced in is elaborated as a module but is
never entered there, so the import that follows does not find it and loads
the file again.

## Fix directions

1. Register a `defmodule` form under its name when it is elaborated, so a
   later `import` of that name reuses it (and gets its exports from it). The
   two routes then agree on one definition. Only if the loaded file is the
   one the import would read: compare the resolved paths, and keep today's
   behaviour (or refuse with a located error) when they differ.
2. Or refuse it: when an import resolves to a module that is already defined
   in the program by another route, emit a located error naming both sites,
   instead of the cc error or the "already defined" one at a stdlib line.
3. For the stdlib case alone, `capability.tur` could stop `load`ing
   `time.tur`, but it is a load-only file (no `defmodule`) and `import` is
   only allowed inside one, so that means turning `capability.tur` into a
   module, which changes how `io.tur` and every user reach it.

## Resolution (2026-10-18)

Fix direction 1, with direction 2's error for the case where the two routes
name different files.

- **A loaded defmodule is registered.** When `elab_defmodule`
  (`src/compiler/elab_module.c`) finishes a module that no import is loading,
  it adds it to `e->loaded_modules` (`module_registry_add`), marked
  `from_load`, with the canonical path of its file (`path_key`, the key
  `(load ...)` dedups on) and its export tables. The tables are filled by
  `module_collect_exports`, the code the import path already ran, moved into
  a helper so both routes build them the same way. A later `import` of that
  name finds the entry and reuses it, so nothing is defined twice.
- **Different files are refused.** If the import's search path names a file
  other than the loaded one, the import is a located error ("module 'm' is
  already defined by the loaded file ..., but this import resolves to ...").
  When the search path names no file at all, the import takes the loaded
  module, so `(load "vendor/m.tur")` then `(import m)` works.
- **The other order.** An import marks the file it reads in the load-visited
  set (`elab_load_path_mark`), so a `(load ...)` of that file expanded later --
  a module imported after it that loads it -- is the no-op a second load of
  it already was.

A `(load ...)` of a defmodule file *inside* another module's body is still
"only one defmodule is allowed per file", a separate limit this does not
change.

Pinned by `load-then-import-same-module` (a user module and the stdlib shape:
`stdlib/capability.tur` loaded beside `(import time ...)`),
`import-then-load-same-module`, and
`errors/load-and-import-name-different-files`.
