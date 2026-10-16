# A `definstance` in a file `load`-ed by two modules is defined twice

**Severity:** medium (link failure). This is the leftover of the archived
definstance cross-module report. It was noted there but never filed.

## Repro

```turmeric
;; shared/desc.tur
(defclass Describe [a]
  (describe [x : a] : int))
(definstance Describe [int]
  (describe [x] (* x 10)))
```

```turmeric
;; ma.tur                              ;; mb.tur
(load "../shared/desc.tur")            (load "../shared/desc.tur")
(defmodule ma                          (defmodule mb
  (export fa)                            (export fb)
  (defn fa [] : int (describe 1)))       (defn fb [] : int (describe 2)))
```

plus an `app.tur` that imports both. Separately compiled (`tur build .` with
no `build.tur`):

```
multiple definition of `__inst_Describe_describe_int'
```

## Root cause

`src/compiler/emit_module.c:emit_inst_method_wants_external` decides whether
a unit references an instance method or defines it. It only recognizes
stdlib paths as "defined elsewhere". A user file loaded into two modules is
spliced into both, and both units define the method.

## Fix directions

- Treat an instance as owned only by the unit whose own file defines it
  (`origin_file_id == 0`); every other unit declares it `extern`. One unit
  must still be chosen to own instances from a file no module owns, e.g. the
  first module (in build order) that loads it.
