# A `load` inside a `defmodule` claims the file for that module alone

**Severity: medium** (compile failure for a documented shape; the workaround is
to load before every `defmodule` that needs the file). Found 2026-10-16 writing
turmeric-spices' `signal/tuning`, whose API returns `(Result Scale String)`.
Reproduced on `main` at `d60c40f6`.

## Summary

`(load "stdlib/string.tur")` placed inside a `defmodule` body splices the
file's definitions into that module (the 2026-06-10 "Option A" fix,
[load-inside-defmodule-silently-loses-names](../archive/history/load-inside-defmodule-silently-loses-names.md)).
But the load-visited set is compilation-global, so the first module whose load
is expanded OWNS the file's definitions:

- every other module's `load` of the same path is a no-op, so that module sees
  none of the names, not even the type, and
- the owning module's copies are private to it, so an importer that did not
  load the file cannot call them.

So a library module that loads `stdlib/string.tur` and exports a function
returning `String` cannot be used by a program that loads `string.tur` too, or
by one that does not. Only loading the file before `defmodule` (global scope)
in every module that needs it works.

## Repro

A two-file project (`build.tur` with `:exports #map{"lib/greet" ["greet"]}`):

```turmeric
;; src/lib/greet.tur
(defmodule lib/greet
  (export greet)
  (load "stdlib/string.tur")

(defn greet [] : String
  (string/from-cstr "hi"))
)
```

and a main file in each of four shapes, `tur run main.tur`:

| Main file | Result |
| --- | --- |
| A: imports `lib/greet`, does not load `string.tur` | `error: symbol 'string/to-cstr' is private to module 'lib/greet'` |
| B: imports `lib/greet`, then `(load "stdlib/string.tur")` inside its `defmodule` | `src/lib/greet.tur:5:7: error: unsupported return type keyword 'String'` -- the LIBRARY loses `String` |
| C: `(load "stdlib/string.tur")` before its `defmodule` | prints `hi` |
| D: `(load ...)` inside its `defmodule`, BEFORE the `(import ...)` | `error: import is only allowed inside defmodule`, then `unknown function or operator 'greet'` |

Moving the library's own load above its `defmodule` changes nothing for B: the
library still loses `String`. With the library's load above its `defmodule`,
A and C both print `hi`; C in both files is the shape `signal/tuning` ships.

```turmeric
;; the main file, shape B
(defmodule app/b
  (import lib/greet :refer [greet])
  (load "stdlib/string.tur")
(defn main [] : int
  (println (string/to-cstr (greet)))
  0)
)
```

## Root cause

`load_expand_forms` (`src/compiler/elab_toplevel.c:1221`) descends into
`(defmodule ...)` bodies (`:1248`) and splices a loaded file's forms in place,
so they elaborate as that module's members. The nested walk shares the
compilation-global visited set on the `Elab` (`:1263`, `:1294`), which keys a
path once per compile, not once per scope. Whichever module's load is expanded
first gets the definitions, scoped to it. Every later `load` of the path, at
top level or in another module, splices nothing. That was right when every
load spliced into one global scope. A per-module splice breaks it.

Shape D is a second symptom of the splice. `defmodule` header parsing
(`src/compiler/elab_module.c`) collects `export` / `import` / `export-from`
items only until the first other form (`break; /* Body starts here */` at
`:1748`). A `load` above the `import` splices definitions there, so the
`import` becomes a body form and reaches `elab_toplevel_import`, whose message
(`elab_module.c:1536`) says imports must be inside a `defmodule`, which this
one is.

`signal/tuning` loads `stdlib/string.tur` above its `defmodule` and tells its
users to do the same. Other turmeric-spices modules keep stdlib `String` out of
their signatures: json, frame, regex, tourist-session and dom-core each declare
a private mirror `(defopaque String :ptr<void>)` in an `ownstr.tur`. Those were
written for an earlier, since-fixed load bug
([compiled-string-return-int-conversion](../archive/compiled-string-return-int-conversion.md)),
and this one now keeps them from collapsing back onto stdlib `String`.

## Fix directions

- A file loaded inside a module should splice into the GLOBAL scope (as
  stdlib's `tur/` exports are promoted to module-less) when it has no
  `defmodule` of its own, keyed once per compile as today. That matches what
  users expect of `(load "stdlib/...")`: a shared stdlib, not a private copy.
- Or keep per-module splicing but key the visited set by (path, module), so
  each module gets its copy. That needs the copies to share one C definition
  and one `String` type, or two modules' `String`s will not unify.
- Shape D: either hoist `load` items before the header scan ends, or give the
  stray-`import` diagnostic a variant naming the cause ("`import` must come
  before the module's definitions; a `(load ...)` above it added some").
