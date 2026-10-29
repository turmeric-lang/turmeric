# An exported global's C name does not match the one its header declares

**Severity:** medium (a library that exports a `def` cannot be used through
its header: an importing module fails to compile, a C host fails to link).
Found 2026-10-29 with
[sepcomp-module-def-initializers-never-run](sepcomp-module-def-initializers-never-run.md).

## Repro

```turmeric
;; build.tur
(defpackage glib2
  :version "0.1.0"
  :exports [glob user])

;; src/glob.tur
(defmodule glob
  (export counter)
  (def ^mut counter 5))

;; src/user.tur
(defmodule user
  (import glob :refer [counter])
  (export read-it)
  (def unrelated 1)
  (def other 2)
  (defn read-it [] : int counter))
```

```sh
tur build --shared .
```

```
build/obj/user.c:5468:16: error: 'counter_2' undeclared (first use in this function)
tur: cc invocation failed (status 256)
```

The three names in play:

| where | name |
| --- | --- |
| `glob.c` defines | `int64_t counter_2;` |
| `glob.h` declares | `extern int64_t glob__counter;` |
| `user.c` reads | `counter_2` |

A one-module library builds, but its header promises `glob__counter`, which
the library does not export (`nm -D` shows `counter_4`): a C host compiled
against `glob.h` fails to link, and `dlsym(h, "glob__counter")` is NULL.

## Root cause

The header and the implementation name the global differently, in
`src/compiler/emit_module.c`:

- the header (`emit_header_inner`, ~24213) uses `raw_name_for_binding`: the
  module-qualified `glob__counter`;
- the implementation (`emit_implementation_inner`, Pass 1b, ~24737) defines
  it with `name_for_binding`: the elaboration-order `counter_N`;
- an importing module reads it through its own `name_for_binding` of the
  imported binding, `counter_N` with that module's numbering, and declares
  nothing, relying on the header.

Functions do not have the problem: both sides spell an exported `defn`
module-qualified (`glob__get_hycounter`). Exported `def`s were left on the
local spelling. The suffixes agreeing between two modules (`counter_2` in
both above) is a coincidence of elaboration order, and the importer still
fails because nothing declares the name it uses.

## Fix directions

- Spell an exported global module-qualified everywhere: the definition in
  the owning module, the header's `extern`, and every reference from an
  importer -- the way an exported `defn` is spelled. `name_for_binding`
  could return the qualified name for an `is_exported` global under
  separate compilation.
- A `tests/run-build-shared.sh` case: a two-module library where one module
  reads and `set!`s the other's exported `def`, plus a C host that reads it
  through the header.
- Pairs with the initializer report: once both land, the value is right as
  well as reachable.
