---
title: Separate Compilation and Module Loading
category: Compiler Internals
description: How the compiler finds, loads and resolves modules, which builds split a program into one C translation unit per module, what goes into each module's .h and .c, who owns a generic specialization or an instance across TUs, and how to read the link errors when the split goes wrong
---

# Separate Compilation and Module Loading (internals)

This guide is for contributors working on how a Turmeric program made of
several files becomes C. It covers two layers:

- **Module loading** -- `defmodule`, `import`/`export`, `load`, the stdlib
  autoload, the search path, and qualified-name resolution. This happens in the
  elaborator and is the same for every build.
- **Separate compilation** -- the builds that emit one `.h`/`.c` pair per
  module, and the rules that keep those translation units (TUs) agreeing at
  link time.

The user-level view of modules is in the
[module system guide](module-system-guide.md). Symbol spelling is in the
[name-mangling guide](name-mangling-guide.md). The build driver (build
directories, `cc` lines, caches) is in the
[build-driver internals guide](build-driver-internals-guide.md), and the
shared runtime TU is in the
[runtime preamble and linkage guide](runtime-preamble-and-linkage-guide.md).

> **Trust the code over the header comments.** Several comments in this area
> describe an earlier state (for example the "SX1 experiment gate" in
> `stdlib_autoload.c`, or "the cache" for a file nothing reads). Where a
> comment and this guide disagree, read the code. If the code disagrees with
> this guide, fix the guide.

---

## The short version

1. Every compile prepends the **autoloaded stdlib** (34 files) to the entry
   file's forms. Their bindings are marked `is_from_stdlib`.
2. `(load "path")` is a **textual splice**, expanded before elaboration,
   at most once per path per compile.
3. `(import m)` **elaborates** `m`'s file into the same global scope, once
   per compile, and records its exports.
4. Most builds are **whole-program**: every imported module's definitions are
   flattened into one C file. `tur build <dir>` with a `build.tur` and exactly
   one `main` is whole-program too.
5. Separate compilation runs only for `tur build --shared`, a bare directory
   without `build.tur`, a project with zero or several `main`s or a non-entry
   `#[used]` module, and `tur emit-c --output-dir`. There each module is
   elaborated on its own, and its imports become `#include "<mod>.h"`.
6. Across TUs, the rule is **one owner, external linkage; everyone else
   declares or keeps a `static` copy**. Stdlib code is always a per-TU
   `static` copy. A generic specialization a borrower needs is pushed back to
   its owner in a second pass ("forced specs").

---

## Source map

| File | Role |
|---|---|
| `src/compiler/elab_module.c` | `import` loading (`elab_load_module_impl`), import specs, `defmodule`, export validation, `elab_lookup_sym` (visibility and qualified names), `elab_force_load_module` |
| `src/compiler/elab_toplevel.c` | `elaborate_program`; the `load` preprocessor (`load_expand_forms`, `load_path_key`, `elab_expand_module_loads`); the stdlib promotion sweep |
| `src/compiler/stdlib_autoload.c`, `.h` | The autoload list (`autoload_files_`), `tur_stdlib_autoload_files`, `tur_stdlib_prepend_forms` |
| `src/compiler/spice_search.c`, `.h` | Walk-up to the enclosing `build.tur` (`spice_find_root`), its `src/`, `:spices` deps and workspace siblings (`spice_search_append`) |
| `src/main.c` | Drivers: `compile_to_c` (whole-program), `compile_to_h`, `compile_to_implementation`, `load_project_prelude`, `cmd_build_project`, `cmd_build_multi`, `cmd_build_multi_files`, `cmd_emit_c_to_dir`, `auto_append_spice_includes` |
| `src/compiler/emit_module.c` | `emit_header`, `emit_implementation`, `emit_abi_clone_name`, `emit_inst_method_wants_external`, `emit_imported_adt_ctors`, the r7rs split helpers (`emit_split_lib_owns`) |
| `src/compiler/emit.h` | `ForcedAbiSpec`, `BorrowSpecInfo`, the public emit entry points |
| `src/compiler/emit_fns.c` | `emit_fn_def`: the `needs_static` linkage decision |
| `src/compiler/emit_cps_ir.c` | The CPS entry wrapper's matching linkage decision |
| `src/compiler/emit_split.h` | The r7rs LIB/IMP/CLIENT split -- a different mechanism, see [below](#not-this-the-r7rs-prelude-split) |

---

## Module loading

### The four ways code enters a compile

| Mechanism | When | What happens | Dedup |
|---|---|---|---|
| Stdlib autoload | Every compile | Forms of 34 stdlib files are prepended ahead of the entry file | n/a (once per compile) |
| `(load "path")` | Top level, or directly in a `defmodule` body | The file's forms are spliced in place, before elaboration | Load-visited set, by canonical path |
| `(import m ...)` | In a `defmodule` header (top level only in REPL sessions) | `m`'s file is read, its loads expanded, its forms elaborated into the global scope | `loaded_modules` registry, by module name |
| `#[used]` force-load | Whole-program builds | `elab_force_load_module` loads a module no import reaches | Same registry; a no-op under separate compilation |

### `load` is a preprocessor

`load_expand_forms` (`elab_toplevel.c`) walks the entry's forms depth-first,
in source order, before either elaboration pass:

1. A `(load "p")` form is replaced by the file's own (recursively expanded)
   forms. A file's transitive loads land before the rest of that file.
2. A `(defmodule ...)` form is descended into, so a `load` in a module body
   splices into the module's scope.
3. A Scheme `(import (scheme time))` splices that library's file the same way.
4. Anything else passes through.

The **visited set** (`Elab.load_expanded_paths`) is compilation-global. The
key is `load_path_key`: `realpath` of the path, except that a `stdlib/<rest>`
path is first resolved under the stdlib directory. Before expansion,
`elaborate_program` seeds the set with the canonical path of every autoloaded
file. So an explicit `(load "stdlib/json.tur")` of an autoloaded file is a
no-op, not a redefinition.

Two consequences worth knowing:

- **The path is relative to the process's working directory**, not to the
  file containing the `load`. `elab_read_file(path_buf)` reads it as given;
  the only fallback retries a `stdlib/<rest>` path under the stdlib directory.
  Verified: `tur run sub/app.tur` fails with
  `load: cannot open 'helpers.tur'`, and the same command from `sub/` works.
- **The first load claims the file.** Because the set is global, a second
  module's `load` of the same path splices nothing. For a `stdlib/` path this
  is harmless: `load_expand_forms` (`elab_toplevel.c`) hoists a stdlib load
  written inside a `defmodule` body to the level the module sits at, so its
  definitions are global wherever the load is written
  ([load-in-defmodule-claims-file-for-one-module](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/load-in-defmodule-claims-file-for-one-module.md),
  resolved 2026-10-16). A non-stdlib file loaded in a module body still
  splices into that module, so a second module that loads it gets nothing.

A `load` that survives to elaboration (inside a `defn` or `let` body) reaches
`elab_load` (`elab_module.c`), which is a hard error.

### `import` elaborates the module in place

`elab_load_module_impl` (`elab_module.c`), in order:

1. Refuse in a sandboxed Elab.
2. Look the name up in `e->loaded_modules`. A hit returns the cached entry;
   a hit with `is_loading` set is `circular import: module 'x' is already
   being loaded`.
3. Reserve a registry slot (`is_loading = true`) before reading anything.
4. Find and read the file (next section). Register a `SourceFile` with a
   fresh `file_id` and honour its `#lang` line.
5. Parse with the shared reader-macro registry, expand its loads
   (`elab_expand_module_loads`, same visited set), and lower it if it is
   `#lang r7rs`.
6. Elaborate the forms with `in_imported_module = true`, after a
   forward-declaration pass over its top-level `defn`s.
7. **Whole-program only** (`!e->separate_compilation`): register the
   module's `EX_DEFMODULE`, and any bare top-level definitions a `load`
   spliced in, for file-scope emission (`elab_register_file_def`). This is
   what flattens an imported module into the importer's C file.
8. Collect the export table into the slot: bindings whose
   `defining_module_name` is this module and `is_exported`, plus
   `export-from` re-exports (the defining module's own `Binding`, so no
   wrapper), exported macros, and exported effects.

Step 7 is the whole difference between the two build shapes. Under separate
compilation the imported module is still fully elaborated -- the importer
needs its types and signatures -- but none of its definitions are emitted.

The registry is per Elab, so per compile. Under separate compilation every TU
re-elaborates each module it imports.

### `defmodule`

`elab_defmodule` (`elab_module.c`) accepts
`(defmodule name [doc] (export ...) (export-from m ...) (import ...)... body...)`:

- Only one per file (`has_defmodule`; reset at each file boundary of a load
  splice, so two loaded module files do not trip it).
- Names may use `[A-Za-z0-9_-/]`, not starting or ending with `/`.
- Imports run first (`elab_apply_import`), then a forward-declaration pass
  over the body, then the body. Body errors are all reported, not just the
  first.
- After the body, each `(export x)` must name a binding or macro defined in
  this module; `(mut g)` additionally requires a `^mut` data global.
  `export-from` must name a module this module imports and a name that module
  exports.

### Name resolution

Imported bindings live in the **one global scope**. Visibility is a check on
the binding, not a separate scope. `elab_lookup_sym` (`elab_module.c`):

1. **Direct lookup** of the symbol as written. If found:
   - a binding of a `tur/`-prefixed module is always visible (the implicit
     `tur/` import);
   - a binding of another module that is not exported is
     `symbol 'x' is private to module 'm'`, with a note at the definition;
   - a deprecated binding warns (or errors under `--Werror=deprecated`).
2. A symbol without `/` that missed is unbound.
3. **Self-qualified**: `<current-module>/x` looks up `x`.
4. **Inside a module**: `alias/x` for each `:as alias` import, then
   `full/module/name/x` for each import. Only exports resolve; a private
   match gets a note saying so.
5. **At top level** (no current module): REPL `:as` aliases, then the symbol
   is split at each `/` from the right and matched against exported global
   bindings of that module. So `a/b/f` tries module `a/b` first.

Because of step 1, `(import util)` with no `:refer` already makes util's
exported names visible unqualified. Verified with `(import util)` followed by
a bare `(twice 4)`, which compiles and prints `8`. `:refer` adds the binding
to the scope a second time and is what injects macros and effects; for plain
bindings it is not what grants access.

After the stdlib region, the **promotion sweep** in `elaborate_program`
rewrites every `tur/`-module binding's `defining_module_name` to NULL and sets
`is_from_stdlib`, so stdlib names behave as pre-module globals.

---

## The module search path

`elab_load_module_impl` tries, in order, and stops at the first file found:

| # | Directory | Source |
|---|---|---|
| 1 | `e->module_base_dir` | The **entry** file's directory (`dir_of_path(path)` in the driver) |
| 2 | `e->module_stdlib_dir` | `TUR_STDLIB_DIR`, else the literal `stdlib` |
| 3 | `e->module_include_dirs[0..n)` | `-I` flags first, then auto-discovered spice dirs |

Under each directory it tries `<name>.tur`, then `<name>.scm`
(`module_read_named`). `elab_module_resolve_path` (used by `:for-macros` and
Scheme library lookup) walks the same order and must stay in lockstep.

**Row 1 is the entry file's directory, not the importing file's.** It is set
once per compile and not updated for nested imports. Verified: with
`app.tur` importing `a/x`, and `a/x.tur` importing `y`, the compiler looks for
`./y.tur` next to `app.tur`, not `a/y.tur`. That is the design -- a module
name is a path under the project root -- and the "not found" diagnostic
labels the row `(project root)`. It used to say `(importing file's
directory)`
([nested-import-resolves-from-entry-dir](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/nested-import-resolves-from-entry-dir.md)).

A "not found" lists every path tried and adds a hint: with no `-I` dirs, that
this looks like an intra-spice import; for a Scheme `(import (two a))`, that a
library is found by its name.

### Auto-spice discovery

`auto_append_spice_includes` (`main.c`) appends to the include list, after the
user's `-I` flags, whatever `spice_search_append` (`spice_search.c`) finds.
It is skipped under `--no-auto-spice`.

1. `spice_find_root` walks up from the input file's directory, at most 16
   steps (`TUR_SPICE_WALK_MAX`), to the first directory holding `build.tur`
   or `build.tur.sweet`.
2. That spice's own `<root>/src`, if it exists.
3. Each `:spices` dep of its manifest:
   - `:global` -- the `tur install` registry;
   - `:path` -- `<root>/<path>`;
   - with `:ref` -- `<root>/spices/<name>-<ref>`, else `<root>/spices/<name>`;
   - then `:subdir`, then `src/` if present (else the dep dir itself). Only
     existing directories are added.
4. **Workspace siblings**: walking up from the root, the first ancestor with a
   manifest ends the walk. If its `:members` lists this spice (matched by
   `realpath`), every other member's `src/` is added. An import satisfied
   there warns once, unless the consumer declares that sibling in `:spices`.

The per-file commands that call it: `check`, `expand`, `audit-spans`,
`emit-c` (both forms), `emit-h`, `run <file>`, `compile`, the interpreter
entry points -- and **`build <file>`**. Verified: in a spice with
`src/demo/main.tur` importing `demo/lib`, `tur build src/demo/main.tur`
links and prints `42`, and fails with `module 'demo/lib' not found` only under
`--no-auto-spice`.

A project build (`tur build <dir>` with a manifest) resolves its include path
from the manifest in `cmd_build_project` instead.

---

## Stdlib autoload

`autoload_files_` (`stdlib_autoload.c`) is the list, in load order. Order is
load-bearing: macros first, typeclasses before the containers that instance
them.

| Group | Files |
|---|---|
| Core | `macros.tur`, `safe.tur`, `contract.tur`, `hamt.tur` |
| Typeclasses | `typeclass-eq`, `-ord`, `-functor`, `-clone`, `-drop`, `-hash`, `-applicative`, `-alternative`, `-monad`, `-monaderror`, `-bifunctor` (each `.tur`) |
| Data | `char.tur`, `map.tur`, `vec.tur`, `slice.tur`, `option.tur`, `result.tur`, `pair.tur`, `tuple.tur`, `list.tur`, `grid.tur`, `zipper.tur`, `set.tur`, `mutmap.tur` |
| Other | `json.tur`, `schema.tur`, `sym.tur`, `unique.tur`, `trail.tur`, `region.tur` |

`tur_stdlib_autoload_files` returns that list, plus the entry dialect's
prelude (`g_lang_prelude`: `saffron/prelude.tur` or `r7rs/prelude.tur`)
appended last. Always read the list through the accessor.

Two loaders consume it:

| Loader | Used by | Notes |
|---|---|---|
| `tur_stdlib_prepend_forms` via `prepend_stdlib_forms` | `compile_to_c` (whole-program) | Honours `--no-auto-stdlib` (check/expand/audit-spans only): when the entry file is itself an autoload file, that file and every later one are skipped. Records where the file-id band ends (`diag_note_autoload_file_ids`). |
| `load_project_prelude` | `compile_to_h`, `compile_to_implementation` | Same list, same order. No `--no-auto-stdlib`. Does not record the file-id band. |

Both read each file from the stdlib root, skip a missing file silently, and
return the number of prepended forms. `elaborate_program` uses that count as
`stdlib_prefix`: forms below it elaborate with `in_stdlib_load = true`, which
sets `Binding.is_from_stdlib`. That flag is what separate compilation keys
its per-TU `static` copies on.

**To add a file:** add it to `autoload_files_` at the right point in the
order. Both loaders and the WASM analyzer (`src/web/wasm_lsp.c`) pick it up.
The file should be a `(defmodule tur/<name> ...)` so the promotion sweep makes
its exports global. Check that nothing in it collides with a name a user
`(load "stdlib/<it>.tur")` already relied on; the visited-set seed makes such
a load a no-op.

---

## Which builds split into TUs

| Command | Shape | Driver |
|---|---|---|
| `tur build <file>`, `tur run <file>`, `tur emit-c <file>` | Whole-program, one TU | `compile_to_c` -> `emit_program` |
| `tur build <dir>` with `build.tur`, exactly one `main`, no `#[used]` outside the entry | Whole-program, one TU | `cmd_build_project` -> `cmd_build` |
| `tur build <dir>` with `build.tur`: `--shared`, zero or several `main`s, or a non-entry `#[used]` file | Separate | `cmd_build_project` -> `cmd_build_multi_files` |
| `tur build <dir>` without `build.tur` | Separate | `cmd_build_multi` -> `cmd_build_multi_files` |
| `tur emit-c --output-dir <dir> a.tur b.tur ...` | Separate, emit only | `cmd_emit_c_to_dir` |
| `tur emit-h <file>` | One header, first pass only | `compile_to_h` |

The single-`main` route exists because whole-program emission carries the full
runtime and sees every function at once, which separate compilation cannot.
Anything that only works whole-program -- see [Known gaps](#known-gaps) --
therefore works in most project builds and breaks under `--shared`.

`#[used]` disqualifies the shortcut because a module reached only through a
raw C symbol is not in the entry's import closure (`file_has_used_attr`). On
the whole-program file path, `cmd_build` instead force-loads `#[used]` modules
found on the `-I` dirs.

### The two-pass driver

`cmd_build_multi_files` (and its twin `cmd_emit_c_to_dir`):

1. Name each module from its **file path** relative to the source root
   (`derive_module_name`), and its files by the mangled form:
   `src/demo/lib.tur` -> `demo/lib` -> `obj/demo__lib.{h,c}`. The importer's
   `#include` comes from the **import's** module name, so a file whose path
   does not match its `defmodule` name gets an include that no file answers.
2. **Pass 1.** For each module: `compile_to_h` then
   `compile_to_implementation`, each a full read + prelude + core passes
   with `separate_compilation = true`. Collect each module's borrow specs.
3. **Forced specs.** For each borrow spec, find the owner by
   `owning_module` and add a `ForcedAbiSpec` to the owner's list (deduped by
   clone name).
4. **Pass 2.** Recompile (header and implementation) every owner that has
   forced specs.
5. Write `.tur-abi-cache/index` (unless disabled).
6. Write `obj/_main.c` (executables only; it only `#include`s the module
   headers -- `main` must come from a module), `obj/tur_runtime.h`, and
   `obj/tur_runtime.c` (`#define TUR_RT_OWNER 1`, the one owner of the
   runtime's globals).
7. One `cc` call over all of it. `TUR_SHOW_CC=1` prints it.

So a module is elaborated two to four times as a build target, plus once per
TU that imports it. The intermediates stay in `<build-dir>/obj/` for
inspection.

`tur emit-c --output-dir` writes the module pairs and the cache but no
`tur_runtime.h`, so its output does not compile as is.

---

## What goes into each file

### The header (`emit_header_inner`)

In order, with `separate_compilation` true:

1. Guard `TUR_<sanitized module>_H`, then `#include "tur_runtime.h"`.
2. System headers; three Windows/c2mir CRT prototypes; hoisted inline-C
   `#include`s (so every importer sees their typedefs).
3. The guarded `tur_poly_fn_t` carrier and `struct __tur_sym;`.
4. `#include "<mangled import>.h"` for each import of this file's
   `defmodule`.
5. Types: base ADT typedefs that embed no monomorph (pass A), then every
   ADT-application monomorph and fn-pointer typedef the module's items reach
   (a full body scan, not just exported signatures), then the remaining base
   typedefs (pass B). All of them are behind `TUR_TD_`/`TUR_TY_` guards, so
   two headers carrying the same type do not collide
   ([result-typedef-duplicated-across-modules](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/result-typedef-duplicated-across-modules.md)).
6. Prototypes of the spec clones this module **owns** -- only those with a
   real owning module (`defining_module_name != NULL`), including forced ones.
7. Prototypes of exported functions and of this module's user instance
   methods, `extern` prototypes of every `extern-c`, and `extern` declarations
   of exported `def`s. Stdlib functions are skipped. Each prototype mirrors
   the `.c` definition's spelling rules (fn params as `int64_t`, `^fat` as
   `int64_t`, pass-by-pointer aggregates as `const T *`, inline-C results as
   `int64_t`).

### The implementation (`emit_implementation_inner`)

1. `#include "<module>.h"`, the cons helper, and the per-TU closure/fat
   runtime (`emit_closure_fat_runtime`, all `static` or macros).
2. The ABI scan (`emit_abi_scan_program`) finds every specialization this TU
   calls. A spec whose generic `FnDef` is in this TU's items is **owned**
   (`fn_expr != NULL`); one whose generic lives in an imported module is a
   **borrow** spec (`fn_expr == NULL`): its call sites are rewritten to the
   clone name and no body is emitted.
3. Linkage of owned specs: external if the binding has a defining module,
   otherwise (stdlib) `static`. Forced specs are added as owned and external.
4. Borrow specs are copied out as `BorrowSpecInfo` for the driver.
5. Base ADT typedefs and constructors of this module's ADTs, plus `static`
   copies of the constructors of every **imported** ADT this module builds
   (`emit_imported_adt_ctors`).
6. File-scope inline-C blocks (deduped), then definitions: globals
   (`static` unless exported), function bodies, spec clone bodies, the
   `__ffi` export shims.
7. Assembly: symbol records (weak), types, forward declarations, defer
   thunks, bodies, fat-box definitions, the per-TU instance row table, CPS
   group flush, and the static-init constructor. **No `main` is
   synthesized** under separate compilation.

### Linkage, in one table

| Item | Owning TU | Every other TU |
|---|---|---|
| Exported `defn` | external definition | prototype via the header |
| Private `defn` | `static` | nothing |
| `#[used]` defn (`retain_c_linkage`) | external | nothing (reached by raw C name) |
| User module's instance method | external (`emit_inst_method_wants_external`) | prototype via the header |
| Stdlib `defn`, stdlib or stdlib-loaded instance | `static` copy | its own `static` copy |
| Spec clone of a user generic | external, prototype in the owner's header | call sites renamed (borrow) |
| Spec clone of a stdlib generic | `static` copy | its own `static` copy |
| Exported `def` | external definition | `extern` in the header |
| Constructors of an ADT | `static` | `static` copy if it constructs one |
| `__cps` twin of a colored defn | `static` | nothing |
| CPS entry wrapper (plain name) | linkage of the defn | as the defn |

The function decision is `needs_static` in `emit_fn_def` (`emit_fns.c`):
external iff `separate_compilation` and (`is_exported` or `retain_c_linkage`
or a user instance method) and not `is_from_stdlib`. The CPS entry wrapper
(`emit_cps_ir.c`) repeats the same test; the two must stay in lockstep or cc
reports `static declaration follows non-static declaration`.

### Generic specializations across TUs

`emit_abi_clone_name` (`emit_module.c`) builds a clone's name from:

1. the binding's **bare** Turmeric name -- no module prefix;
2. `__spec__`;
3. the C type name of the **result**;
4. `_` plus the C type name of each argument, in order.

Every non-alphanumeric character becomes `_`. If the name is already taken
by another spec in the same TU, `__h1`, `__h2`, ... is appended in intern
order.

Real names from `tur emit-c`:

| Clone | Reading |
|---|---|
| `pick__spec__const_char___int64_t_const_char__` | `pick` -> `cstr`, args `int`, `cstr` |
| `some__spec__tur_adt_Option__int_int64_t` | `some` -> `(Option int)`, arg `int` |
| `some___spec__bool_tur_adt_Option__int` | `some?` (`?` -> `_`) -> `bool`, arg `(Option int)` |

Ownership: the module that defines the generic owns every clone of it. A
borrower's clone reaches the owner as a `ForcedAbiSpec`
`{clone_name, fn_symbol, result_kind, arg_kinds[], n_args}`, and the owner
emits the body and the header prototype in pass 2. In the tiny project below,
`app` calls `util`'s `ident` at `cstr`, and the result is:

```
$ tur build .            # util.tur + app.tur, no build.tur
$ grep -n ident__spec build/obj/*.h build/obj/*.c      (trimmed)
build/obj/util.h:551:const char * ident__spec__const_char___const_char__(const char *);
build/obj/util.c:5435:const char * ident__spec__const_char___const_char__(const char * x) {
build/obj/app.c:5414:  ... (ident__spec__const_char___const_char__("hi"));
```

A forced spec carries `TypeKind`s, not types: the owner rebuilds them with
`emit_type_from_kind`. That is exact for scalars and `cstr`, and loses the
identity of an aggregate (see [Known gaps](#known-gaps)).

### Typeclass instances across modules

An instance method is never `is_exported` (a `definstance` has no export
list), so `emit_inst_method_wants_external` decides: external when the
function is an instance method (`owner_instance` set), its binding is not
`is_from_stdlib`, and the instance's `origin_file_id` does not resolve to a
path containing `stdlib/`. The header loop declares
such methods even though they are not exported. History:
[definstance-not-dispatchable-across-modules](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/definstance-not-dispatchable-across-modules.md).

The predicate does not detect a non-stdlib file `(load ...)`-ed into several
modules; each TU then defines the instance externally (see
[Known gaps](#known-gaps)).

`in_imported_module` keeps an imported module's self-registering forms
(classes, instances) from registering for emission in the importer's TU.

### CPS functions across TUs

Coloring and classification (`ensure_S`, see the
[CPS internals guide](cps-dk-internals-guide.md)) run over the program being
emitted, which under separate compilation is one module. The `__cps` twin is
always `static`. A call into another module's colored function is therefore an
ordinary direct call to its **entry wrapper**, which seeds a fresh root
continuation with `__dk_enter0`. Handlers installed by the caller are not on
that continuation.

`emit_split_lib_owns` is not involved: it belongs to the r7rs split below and
returns false whenever that split is off.

### Not this: the r7rs prelude split

`emit_split.h` describes `EMIT_SPLIT_LIB` / `IMP` / `CLIENT`: a single
`#lang r7rs` program emitted as two or three units (the prelude library, the
imported libraries, the program) so the library unit's object can be cached.
It runs inside `compile_to_c` when `prelude_split_applies()` holds: an
`r7rs` entry on a 64-bit host, Linux or macOS unless `TUR_PRELUDE_SPLIT=1`
forces it elsewhere, and `TUR_PRELUDE_SPLIT=0` turns it off. Ownership there
is by `emit_split_binding_home`: `is_from_stdlib` -> LIB, `is_from_implib` ->
IMP, else CLIENT. It has nothing to do with module TUs.

---

## The ABI cache (`.tur-abi-cache/`)

`cmd_build_multi_files` and `cmd_emit_c_to_dir` write
`<build-dir>/.tur-abi-cache/index` after pass 2 (via `index.tmp` and a
rename). One line per borrow spec, tab-separated:

```
clone_name  owning_module  fn_symbol  result_kind  n_args  arg_kind...
```

The kinds are `TypeKind` integers. For the example above:

```
ident__spec__const_char___const_char__  util  ident  5  1  5
```

**Nothing reads it.** The forced specs of a build are always recomputed by
pass 1 of that build; no source file opens the index (searched: only the two
write sites and the `pkg.c` directory skip mention it). `--no-abi-cache` or
`TUR_NO_ABI_CACHE=1` (any value but `0`) suppresses the write and changes
nothing else. Treat the index as a debugging record of which module was asked
to own which clone. Build-directory placement is in the
[build-driver internals guide](build-driver-internals-guide.md).

---

## Diagnosing a separate-compilation failure

First, confirm the build really split: a project with one `main` and no
`--shared` is whole-program, so a split-only bug will not reproduce there.
Reproduce with a bare directory (`tur build <dir>` without `build.tur`) or
`--shared`.

Then look at the evidence:

1. `<build-dir>/obj/<mod>.{h,c}` -- the emitted pair. Grep for the symbol in
   every `.h` and `.c`: is it defined twice, defined `static`, or declared
   but never defined?
2. `<build-dir>/.tur-abi-cache/index` -- which owner each borrowed clone was
   forced onto.
3. `TUR_SHOW_CC=1` -- the exact `cc` line.
4. `tur build <entry-file>` -- the whole-program build of the same code. If
   it works, the bug is in the split.

### Failure shapes

| Symptom | Usual cause |
|---|---|
| `multiple definition of '<fn>__spec__...'` in two module `.c` files | Two modules define generics with the same bare name, instantiated at the same types: clone names are not module-qualified |
| `multiple definition of '__inst_<Class>_<m>_<T>'` | A `definstance` in a non-stdlib file that several modules `(load ...)` |
| `incompatible type for argument N of '<fn>__spec__tur_adt_...'` | A forced spec at an aggregate type: the owner's clone takes `int64_t` |
| `undefined reference to '<mod>__<fn>'` | The module is not in the build set, or inline C spells a mangled name by hand ([cross-module-private-helper-dropped-at-link](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/cross-module-private-helper-dropped-at-link.md)) |
| `undefined reference to 'main'` | No module of a separate executable build defines `main`; `_main.c` does not synthesize one |
| `static declaration follows non-static declaration`, `conflicting types` | The header prototype and the `.c` definition disagree (linkage or parameter spelling) -- e.g. [sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch.md) |
| `unknown type name 'tur_adt_X'`, `field has incomplete type` | A typedef the header did not carry or ordered wrongly -- e.g. [sepcomp-recursive-byval-fixpoint-adt-incomplete-header-type](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/sepcomp-recursive-byval-fixpoint-adt-incomplete-header-type.md) |
| `redefinition of 'Result__...'` | A monomorph typedef without its include guard ([result-typedef-duplicated-across-modules](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/result-typedef-duplicated-across-modules.md)) |
| ``error: `f` (module m) can perform the effect `E`, and this call crosses into it from another module`` | A call into another module whose callee can perform a user effect. Refused at build time since 2026-10-18; before, it built and aborted at run time with `tur: unhandled effect (tag N)` |

### A worked failure

Two modules each export `(defn ident [A] [x : A] : A x)`. `user1` calls
`mb`'s at `cstr`; `app` calls `ma`'s at `cstr` and imports `user1`. Each
importing TU sees only one `ident`, so no `__h1` suffix is added, and both
owners are forced to emit the same name:

```
$ tur build .                                 (trimmed)
ma.c:(.text+0x10): multiple definition of
  `ident__spec__const_char___const_char__'; mb.c:(.text+0x10): first defined here
collect2: error: ld returned 1 exit status
$ cat build/.tur-abi-cache/index
ident__spec__const_char___const_char__  mb  ident  5  1  5
ident__spec__const_char___const_char__  ma  ident  5  1  5
```

The index shows the cause directly: one clone name, two owners. When `app`
imports both modules itself, its TU interns both specs and the second becomes
`..._h1`, which is why the simplest two-importer repro does not fail.

---

## Known gaps

Measured on 2026-10-16 with tiny bare-directory projects. Whole-program
builds of the same sources work in every case. None of these was filed when
this guide was written; link the reports here once they are.

| Gap | Where |
|---|---|
| Spec clone names are not module-qualified, so same-named generics of two modules collide at link | `emit_module.c:emit_abi_clone_name` |
| A forced spec at an aggregate type is emitted with `int64_t` parameters and result | `ForcedAbiSpec` / `BorrowSpecInfo` carry `TypeKind`; `emit_type_from_kind` in `emit_header_inner` and `emit_implementation_inner` |
| A user file `load`ed by two modules duplicates its instance methods | `emit_module.c:emit_inst_method_wants_external` (the residual recorded in the definstance report) |
| An effect performed in one module's TU cannot reach a `handle` in another's. It is refused at the call site, not threaded | per-TU coloring; `static` `__cps` twins; the entry wrapper's fresh root continuation; the refusal is `effect_check.c` (`foreign_check`) |
| Nested imports resolve against the entry file's directory, and the diagnostic calls it the importing file's | `elab_module.c:elab_load_module_impl` (`module_base_dir`) |

---

## Testing

| What | Where |
|---|---|
| Multi-module whole-program fixtures | `tests/fixtures/module-*`, `load-in-imported-module`, `cross-module-*` (run by `tests/run.sh`) |
| Project builds, manifest descent, `--shared`, the runtime owner TU | `tests/run-build-project.sh` |
| Two headers carrying one monomorph | `tests/run-result-typedef-multi-module.sh` (ctest `tur_result_typedef_multi_module`) |

Most `module-*` fixtures run whole-program, so they do not exercise the split.
A change to header or implementation emission wants a case in
`run-build-project.sh` (or a bare-directory build) as well.

---

## Bugs found while writing this guide

Each was reproduced against the compiler this guide describes and filed in
[`docs/reported/`](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/README.md). When one is fixed and archived,
update the passage above that describes it and drop it from this list.

- [sepcomp-cross-module-effect-unhandled](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/sepcomp-cross-module-effect-unhandled.md) -- Separately compiled: an effect performed in one module cannot reach a handler in another (refused at build time since 2026-10-18)
- [spec-clone-names-not-module-qualified](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/spec-clone-names-not-module-qualified.md) -- Specializations of same-named generics in two modules collide at link
- [forced-spec-loses-aggregate-type](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/forced-spec-loses-aggregate-type.md) -- A cross-module specialization at a struct type is emitted at `int64_t`
- [loaded-user-definstance-duplicated-across-tus](https://github.com/turmeric-lang/turmeric/blob/main/docs/reported/loaded-user-definstance-duplicated-across-tus.md) -- A `definstance` in a file `load`-ed by two modules is defined twice

## Maintenance

Update this guide in the same PR when you:

- change the autoload list, the module search order, or `load` path
  resolution;
- change which `tur build` shapes take the whole-program route;
- change a linkage rule (`needs_static`, `emit_inst_method_wants_external`,
  spec `external_linkage`) or the header contents;
- change `emit_abi_clone_name`, `ForcedAbiSpec` or `BorrowSpecInfo`;
- start reading `.tur-abi-cache/`, or delete it;
- close one of the [known gaps](#known-gaps) -- move it to the failure-shape
  table with a link to its archived report.

Cite functions, not line numbers. Regenerate the excerpts with `tur build`
on a scratch directory when the shapes they show change.
