# A separately compiled module's `def` initializers never run

**Severity:** high (silent wrong value). Every `def` in a module built by
separate compilation -- `tur build --shared <dir>`, the spice REPL's library,
`tur emit-c --output-dir` -- starts zero-filled, and the module's code reads
that zero. No diagnostic. Found 2026-10-29 while sizing split images for the
compiled REPL (aot-compiled-repl-plan C5).

## Repro

```turmeric
;; build.tur
(defpackage glib
  :version "0.1.0"
  :exports [glob])

;; src/glob.tur
(defmodule glob
  (export get-counter bump counter)
  (def ^mut counter 5)
  (defn get-counter [] : int counter)
  (defn bump [] : int (do (set! counter (+ counter 1)) counter)))
```

```sh
tur build --shared .
```

Calling the library's exports, from a C host through `dlsym` or from `tur
repl` in the project directory (which loads the library and binds its
exports):

```
=> 0      ; (get-counter) -- expected 5
=> 1      ; (bump)        -- expected 6
```

`build/obj/glob.c` holds `int64_t counter_4;` and no store of `5` anywhere.

## Root cause

`emit_implementation_inner` (`src/compiler/emit_module.c`, Pass 1b, ~24733)
emits each top-level `def`'s initializer into the `body` buffer, as the
whole-program path does. The assembly (~24922-24938) then writes `body` only
into a user's `main` (`emit_toplevel_stmts_for_user_main`) or into the `main`
it generates, and it generates one only when `!separate_compilation`. A
library module has neither, so `body` is freed unwritten. The whole-program
emitter puts the same statements in `__tur_module_def_init`, registered with
`static_init_register(..., STATIC_INIT_DEFS)` (~23066, ~23118), which
`__tur_static_init` runs -- and separate compilation already emits
`__tur_static_init` plus its `__attribute__((constructor))` wrapper, so the
hook to run it from is there.

Executables never reach this path: since
project-mode-rc-runtime-preamble-missing, `tur build <dir>` compiles a
project with an entry module whole-program (`src/driver/project.c`, ~1174),
so only libraries (and `emit-c --output-dir`) are affected.

## Fix directions

- Emit `body` as `static void __tur_module_def_init(void)` and register it
  with `static_init_register("__tur_module_def_init", STATIC_INIT_DEFS)`
  when `separate_compilation && !user_has_main`, as `emit_program` does.
  The constructor then runs it at `dlopen`.
- A `tests/run-build-shared.sh` case asserting an exported `def`'s value
  through the library, and a `def` whose initializer has an effect, so it
  runs exactly once.
- Module-level `defer`s in the same file are handled after Pass 1b; check
  they still order after the initializers.

## Resolution (2026-10-29)

`emit_implementation_inner` (`src/compiler/emit_module.c`) now writes `body`
as `static void __tur_module_def_init(void)` when compiling separately with no
user `main`, and registers it in the `STATIC_INIT_DEFS` band. This TU's
`__tur_static_init` runs that band, and its constructor wrapper fires at
`dlopen`. The module's stdlib `def`s (`SCHEMA_*` and friends) were
uninitialized the same way and are now set too. Module-level `defer`s
register in the earlier `ATEXIT` band and fire at exit, after the
initializers.

Pinned by `tests/fixtures/build-shared-def-init` through
`tests/run-build-shared.sh`: a library's `counter` reads 5 and then 6 through
its exports, and an initializer that prints runs exactly once at load.
