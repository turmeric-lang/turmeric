---
title: Module System Guide
category: Language Basics
description: Module system, namespacing, exports
---

# Turmeric Module System Guide

Turmeric's module system gives you namespaced, self-contained units of code
with controlled visibility. Use it to organize larger programs, prevent name
collisions, and keep the public surface of a library narrow.

This guide covers everything a working programmer needs to know:

1. Declaring a module
2. Importing other modules
3. Exports and visibility
4. Module-level defer
5. C symbol mangling and FFI naming
6. Auto-loaded stdlib modules
7. Common errors and how to fix them

---

## Declaring a Module

A module is a single `.tur` file with a `defmodule` form at the top:

```turmeric
;; src/geom/vector.tur
(defmodule geom/vector
  (export Point make-vector vector-x vector-y magnitude)

  (defn make-vector [x : int y : int] : ptr
    (...))

  (defn vector-x [^Point p] : int (.x p))
  (defn vector-y [^Point p] : int (.y p))

  (defn magnitude [^Point p] : int
    (...)))
```

```sweet-exp
;; src/geom/vector.tur
defmodule geom/vector
  export Point make-vector vector-x vector-y magnitude

  defn make-vector [x :int y :int] :ptr
    (...)

  defn vector-x [^Point p] :int
    .x(p)
  defn vector-y [^Point p] :int
    .y(p)

  defn magnitude [^Point p] :int
    (...)
```

**Rules:**

- `defmodule` must be the first form in **the file containing it** -- not
  the first form of the whole compilation unit. A `(load ...)` chain may
  bring in flat helper files before a defmodule-wrapped file; each
  `(load ...)`-spliced file gets its own scope for the check, and the
  defmodule remains valid as long as nothing precedes it in its own
  source file.
- Only one `defmodule` per file.
- The module name is a symbol; it may contain `/` to express nesting
  (`geom/vector`, `db/postgres/conn`).
- An optional docstring may follow the name:
  `(defmodule geom/vector "2-D vector math" ...)`.

### Name → file-path convention

Convention is that the module name matches the file path under the project
root:

| Module name         | File path                  |
|---------------------|----------------------------|
| `geom/vector`       | `geom/vector.tur`          |
| `db/postgres/conn`  | `db/postgres/conn.tur`     |
| `app`               | `app.tur`                  |

The compiler resolves imports by translating the module name to a path under
the importing file's directory. The convention is not strictly enforced (the
name in the `defmodule` form is the canonical identifier), but breaking it
will produce confusing "module not found" errors.

---

## Importing Other Modules

Use `(import ...)` inside `defmodule` to bring in another module:

```turmeric
(defmodule app
  (import geom/vector :as v)
  (import math :refer [sqrt abs])

  (defn main [] : int
    (let [p (v/make-vector 3 4)]
      (println (v/magnitude p))
      0)))
```

```sweet-exp
defmodule app
  import geom/vector :as v
  import math :refer [sqrt abs]

  defn main [] :int
    let [p v/make-vector(3 4)]
      println(v/magnitude(p))
      0
```

### Forms of `import`

| Form                                | Effect                                                   |
|-------------------------------------|----------------------------------------------------------|
| `(import math)`                     | Names available as fully-qualified: `math/sqrt`.         |
| `(import math :as m)`               | Alias: `m/sqrt`.                                          |
| `(import math :refer [sqrt abs])`   | Pull selected names directly into scope: `sqrt`, `abs`. |
| `(import math :for-macros)`         | Macro-time only: `math`'s functions become callable from `defmacro*` bodies at expansion time; nothing is imported at runtime. |

`:for-macros` cannot combine with `:as`/`:refer` -- a module needed in both
phases is imported twice, one form per phase, so which code runs at compile
time stays explicit in the source.  Inside the macro-time env the module's
functions bind under their bare names.  A macro *defined* in another module
needs no `:for-macros`; export it and `:refer` it like any macro.  See
[macros-guide.md](macros-guide.md#macro-time-imports-for-macros).

Self-qualification works inside a module too:
`(defmodule geom/vector ... (geom/vector/magnitude p))` resolves
to the local `magnitude`.

### Circular imports

If module A imports B and B imports A, you get a clear error at the second
import attempt:

```text
error: circular import: module 'a' is already being loaded
```

There is no lazy resolution in v1 -- break the cycle by factoring a shared
helper module.

---

## Exports and Visibility

Everything declared inside a `defmodule` is **private by default**. Only names
listed in `(export ...)` are visible to other modules.

```turmeric
(defmodule list-utils
  (export map filter)

  (defn map [f xs] : ptr (...))
  (defn filter [pred xs] : ptr (...))

  ;; private helper -- only visible inside list-utils
  (defn -fold [f acc xs] : ptr (...)))
```

```sweet-exp
defmodule list-utils
  export map filter

  defn map [f xs] :ptr (...)
  defn filter [pred xs] :ptr (...)

  ;; private helper -- only visible inside list-utils
  defn -fold [f acc xs] :ptr (...)
```

### Visibility rules

- **Same module**: all bindings (public and private) are visible.
- **Other module, qualified or `:refer`'d**: only exported names are visible.
- **Top level, outside any `defmodule`**: a module defined in the program is
  reachable by its qualified names -- `(geom/vec/dot 3 4)` -- with no import,
  and only its exported names are visible there too. (Before 2026-10-07 a
  top-level qualified call was unresolved: an error when compiled, and a
  runtime lookup under the interpreter.)
- **Stdlib (`tur/...`) bindings**: globally visible -- see Auto-Loaded Stdlib Modules.

### Macros are exported the same way

```turmeric
(defmodule control-flow
  (export when2)

  (defmacro when2 [test a b]
    (list if test (do a b))))
```

```sweet-exp
defmodule control-flow
  export when2

  defmacro when2 [test a b]
    list(if test do(a b))
```

Imported macros expand correctly in the consumer module -- recursive macro
calls inside an exported macro can still reach private helpers of the
defining module (the elaborator tracks the "expansion module" for this).

The same macro reaching a file by two import paths is **not** a conflict.
Given a diamond where you import both `a` and `b`, and `b` also refers a
macro from `a`, that macro arrives twice -- it is one definition, so the
second arrival is ignored rather than reported. Two *different* modules
exporting the same macro name is still an error.

### `(export-from ...)` -- re-export without a wrapper

A module can re-export names another module already exports, so a facade
module can present a curated surface without a forwarding `defn` per name:

```turmeric
(defmodule geom
  (import geom/vector :refer [v-add v-scale])
  (import geom/matrix :refer [m-mul])
  (export geom-version)
  (export-from geom/vector v-add v-scale)
  (export-from geom/matrix m-mul)
  (defn geom-version [] : int 2))
```

```sweet-exp
defmodule geom
  import geom/vector :refer [v-add v-scale]
  import geom/matrix :refer [m-mul]
  export geom-version
  export-from geom/vector v-add v-scale
  export-from geom/matrix m-mul
  defn geom-version [] : int
    2
```

A consumer now writes `(import geom :refer [v-add m-mul])` and never names
the inner modules.

Rules:

- **The source module must be imported.** `export-from` re-exports from a
  module you already `import`; it does not load one. A second load path with
  its own resolution rules is how two modules end up disagreeing about which
  file a name came from.
- **The name must be *exported* by the source, not merely defined there.**
  Re-exporting a module-private name would smuggle it into the public surface
  through a third party. The two cases get different diagnostics -- "defined
  by 'X' but not exported from it" is one edit, "not exported by module 'X'"
  is a typo.
- **Chains work.** If `mid` re-exports from `low` and `hi` re-exports from
  `mid`, a consumer of `hi` sees the name. The check is "does the source
  module export it", not "did the source module define it".
- **Defns and macros both.** A re-exported macro forwards the original
  definition, so it expands exactly as it would when imported directly.
- **No wrapper is emitted.** The consumer resolves to the *defining* module's
  binding and calls its mangled symbol, so a re-export costs nothing at
  runtime however many hops it travels. Pinned by
  `tests/check-export-from-no-wrapper.sh`.

---

## Module-Level Defer

Top-level `(defer ...)` forms inside a module run at process exit via
`atexit`:

```turmeric
(defmodule logger
  (export log)

  (def file-handle ...)

  (defer (println "shutting down logger"))

  (defn log [msg] : int (...)))
```

```sweet-exp
defmodule logger
  export log

  def file-handle ...

  defer println("shutting down logger")

  defn log [msg] :int (...)
```

Ordering: module defers fire in LIFO order (last-defined fires first), matching
function-level defer semantics. Across modules they fire in reverse load order.

---

## C Symbol Mangling and FFI Naming

When a module exports a symbol, the emitted C name is prefixed with the
mangled module name to prevent linker collisions:

| Source                       | Emitted C symbol         |
|------------------------------|--------------------------|
| `geom/vector` exports `add2` | `geom__vector__add2`     |
| `my-lib` exports `do-thing`  | `my_lib__do_thing`       |
| `tur/safe`   exports `box`   | `box` (stdlib promotion) |

### Mangling rules

- `/` in module name → `__` (double underscore)
- `-`, `.`, and any other non-`[A-Za-z0-9_]` character → `_`
- The module prefix is followed by `__` and then the (sanitized) binding name
- Parameters and locals are NOT prefixed -- they are local to the function and
  inline-C bodies reference them by their source names.

### Collision detection

If two exported symbols mangle to the same C name (e.g. module `my-lib` and
module `my_lib` both exporting `foo`), the compiler emits a hard error at
elaboration time and tells you which two definitions collide. Rename one or
use `(export-as ...)` to override the C name (see below).

### `(export-as "c_name")` -- explicit C name override

For FFI scenarios where you need a specific C symbol name (matching a C
header, registering with a runtime, etc.), use `(export-as "...")` before
the function name:

```turmeric
(defmodule plugin
  (export init)

  (defn (export-as "plugin_init_v2") init [] : int
    (...)))
```

```sweet-exp
defmodule plugin
  export init

  defn (export-as "plugin_init_v2") init [] :int
    (...)
```

This bypasses module mangling: the emitted C function will be named exactly
`plugin_init_v2`. Useful for:

- Matching `extern` declarations in existing C headers
- Exposing a stable ABI for dynamic loading
- Working around mangled name length limits

The argument must be a string literal.

---

## Auto-Loaded Stdlib Modules

A set of stdlib files is auto-loaded into every program (the full list is
`autoload_files_` in `src/compiler/stdlib_autoload.c`). It includes, among
others:

- `stdlib/macros.tur` -- `(defmodule tur/macros ...)`: `cond`, `when`,
  `unless`, `must!`, `must-msg!`, `ignore!`, `do-m`, `for`.
- `stdlib/safe.tur`   -- `(defmodule tur/safe ...)`: `array-get`, `array-set`,
  `array-slice`, `with-c-string`, `from-c-string`, `box`, `unbox`.
- `stdlib/contract.tur`, the core typeclass files (`typeclass-eq.tur`,
  `typeclass-functor.tur`, `typeclass-monad.tur`, ...), the typed
  collections (`map.tur`, `vec.tur`, `set.tur`, `list.tur`, ...),
  `option.tur` / `result.tur` / `pair.tur` / `tuple.tur`, `json.tur`,
  `schema.tur`, `sym.tur`, and `unique.tur`.

After loading, the elaborator **promotes** every export from any module under
the `tur/` namespace back to "stdlib pre-module" status (its
`defining_module_name` is reset to NULL). This means:

- You can call `(when test body)` from user code without writing an `import`.
- The emitted C name has no `tur__macros__` prefix -- `when` is `when`.
- You can shadow stdlib names in user code without conflict.

The `tur/` namespace is reserved for stdlib. User code should not declare
modules under `tur/`.

---

## Building Multi-Module Programs

For a single-file program, `./build/tur build app.tur -o app` works as before
-- the elaborator finds and inlines any `(import ...)` targets.

For a multi-file build, place modules under the same directory tree as
the main file:

```turmeric no-check
src/
  app.tur              ;; (defmodule app (import geom/vector :as v) ...)
  geom/
    vector.tur          ;; (defmodule geom/vector ...)
```

```sweet-exp
src/
  app.tur              ;; defmodule app ...
  geom/
    vector.tur          ;; defmodule geom/vector ...
```

`./build/tur build src/app.tur -o app` recursively loads `geom/vector` from
`src/geom/vector.tur` (matching the module name to the file path under the
main file's directory).

### Separate `.h` / `.c` generation

For incremental builds or integrating into a C build system, you can emit
each module as a separate header/implementation pair:

```sh
./build/tur emit-h   src/geom/vector.tur > geom__vector.h
./build/tur emit-c   src/geom/vector.tur > geom__vector.c
```

In separate-compilation mode, exported functions get `extern` linkage in the
header, private definitions stay `static`, and the header `#include`s its
own dependencies' headers.

---

## Common Errors

### `defmodule must be the first form in the file`

Move any other top-level forms inside the `defmodule` body, or below it (the
latter is also rejected -- `defmodule` must wrap everything).

"The file" here means the source file containing the `defmodule`, not the
entire compilation unit -- a separate `(load ...)`-spliced file may
contribute earlier forms without triggering the diagnostic. The note
attached to the error points at the offending earlier form *in the same
file*, which is always the right place to fix.

### `symbol 'foo' is private to module 'mymod'`

Either add `foo` to `mymod`'s `(export ...)` list, or use `mymod/foo`
inside `mymod` itself (private names can be self-qualified).

### `symbol 'foo' is not exported from module 'mymod'`

A qualified `mymod/foo` -- from another module, or from the top level --
names a member `mymod` keeps private. Add `foo` to `mymod`'s `(export ...)`
list, or call it only from inside `mymod`.

### `exported symbol 'foo' is not defined in this module`

The `(export ...)` list names a symbol that has no matching `defn`/`def`
inside the module body. Check for typos and verify the definition is
inside the `defmodule` form.

### `module 'foo/bar' not found`

The compiler couldn't locate `foo/bar.tur` relative to the importing file's
directory. Check the path or use an absolute path via the project's
`module_base_dir`.

### `circular import: module 'foo' is already being loaded`

Module A imports B which transitively imports A. Factor out the shared parts
into a third module.

### `exported symbol 'foo' from module 'X' mangles to the same C name 'Y' as 'bar' from module 'Z'`

The mangling rules produced identical C names. Rename one of the symbols
or use `(export-as "...")` to override the C name explicitly.

---

## A Complete Example

```turmeric
;; src/math.tur
(defmodule math
  (export sqrt square)

  (defn sqrt [x :int] :int (...))
  (defn square [x :int] :int (* x x)))

;; src/geom/vector.tur
(defmodule geom/vector
  (import math :as m)
  (export Point make-vector magnitude)

  (defstruct Point [x : int y : int])

  (defn make-vector [x :int y :int] :ptr (Point x y))

  (defn magnitude [^Point p] :int
    (m/sqrt (+ (m/square (.x p))
               (m/square (.y p))))))

;; src/app.tur
(defmodule app
  (import geom/vector :refer [make-vector magnitude])

  (defn main [] :int
    (let [p (make-vector 3 4)]
      (println (magnitude p))   ;; 5
      0)))
```

```sweet-exp
;; src/math.tur
defmodule math
  export sqrt square

  defn sqrt [x :int] :int (...)
  defn square [x :int] :int
    {x * x}

;; src/geom/vector.tur
defmodule geom/vector
  import math :as m
  export Point make-vector magnitude

  defstruct Point [x : int y : int]

  defn make-vector [x :int y :int] :ptr
    Point(x y)

  defn magnitude [^Point p] :int
    m/sqrt({m/square(.x(p)) + m/square(.y(p))})

;; src/app.tur
defmodule app
  import geom/vector :refer [make-vector magnitude]

  defn main [] :int
    let [p make-vector(3 4)]
      println(magnitude(p))   ;; 5
      0
```

Build with `./build/tur build src/app.tur -o app`.

---

## Limitations and v1 Caveats

- **No dynamic imports**: all `(import ...)` statements must be at the top of
  a module body. No `(require)` at function scope.
- **No wildcard `:refer :all`**: explicit symbol lists only.
- **Module-private types in public signatures**: not yet a hard error; the
  type system will eventually enforce it.
- **Hot reload / dynamic loading**: not supported in v1.

The deferred items above are the full list; there is no separate design-history
document for them.
