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
