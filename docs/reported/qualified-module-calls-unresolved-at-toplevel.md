# Slash-qualified module member calls are unresolved at the top level

**Severity: medium (qualified calls silently degrade to runtime dispatch and
fail for exported members).** Found 2026-10-05 while building the Trowel
plugin system, which evaluates Turmeric source in-process via `turi_eval`.

## Summary

A slash-qualified call to a module member -- `(Foo/bar)` -- works inside the
defining module (self-qualification) and inside any module that imports `Foo`
(cross-module resolution), but is **not resolved by the elaborator at the top
level** (outside any `defmodule`). The elaborator emits `TUR-W0040` ("unknown
name 'Foo/bar'; will runtime-dispatch -- typo?") and defers to the interpreter's
runtime dispatch. Runtime dispatch then fails for **exported** members because
their mangled C name uses the `__` structural separator (`Foo__bar`), while the
runtime looks up the injective encoding of the literal slash (`Foo_slbar`).

The behaviour is inverted from what a user expects: **exported** members
(the ones meant to be called from outside) fail, while **non-exported** members
(the ones meant to be private) succeed at runtime because they remain in the
global env under their bare name.

## Minimal repro

```sh
# Exported member -- fails
$ tur eval '(defmodule Foo (export bar) (defn bar [] "hello")) (Foo/bar)'
<eval>:1:53: warning [TUR-W0040]: unknown name 'Foo/bar'; will runtime-dispatch -- typo?
tur: unknown function or operator 'Foo/bar'

# Non-exported member -- works (inverted!)
$ tur eval '(defmodule Foo (export baz) (defn baz [] "x") (defn bar [] "hello")) (Foo/bar)'
<eval>:1:71: warning [TUR-W0040]: unknown name 'Foo/bar'; will runtime-dispatch -- typo?
"hello"
```

File-based `tur run` is the same (it is an error, not a warning, because the
compiled path treats the unresolved name as a hard error):

```sh
$ cat > /tmp/test.tur << 'EOF'
(defmodule Foo
  (export bar)
  (defn bar [] "hello"))
(println (Foo/bar))
EOF
$ tur run /tmp/test.tur
/tmp/test.tur:4:11: error: unknown function or operator 'Foo/bar'
```

For contrast, the same call **inside** the module works:

```sh
$ cat > /tmp/test2.tur << 'EOF'
(defmodule Foo
  (export bar)
  (defn bar [] "hello")
  (defn main [] : int
    (println (Foo/bar))
    0))
EOF
$ tur run /tmp/test2.tur
hello
```

And cross-module resolution via `import` works:

```sh
$ cat > foo.tur << 'EOF'
(defmodule foo
  (export bar)
  (defn bar [] "hello"))
EOF
$ cat > main.tur << 'EOF'
(defmodule app
  (import foo)
  (defn main [] : int
    (println (foo/bar))
    0))
EOF
$ tur run main.tur
hello
```

## Root cause

`elab_lookup_sym` in `src/compiler/elab_module.c:1905` resolves qualified names
in three stages:

1. **Direct scope lookup** -- looks up the full symbol `Foo/bar` in the scope.
   No binding exists under that name; the binding is registered as `bar` with
   `defining_module_name = Foo`.
2. **Self-qualified** -- fires only when `e->current_module_name != NULL` and
   the symbol starts with `current_module_name/`. At the top level,
   `current_module_name` is NULL, so this path is skipped.
3. **Cross-module qualified** -- fires only when `e->current_module != NULL`
   and iterates `current_module->imports`. At the top level,
   `current_module` is NULL, so this path is skipped.

All three paths fail, and the call falls through to the `TUR-W0040` runtime
dispatch fallback in `elab_call.c:5935`. The fallback creates a dynamic
binding under the name `Foo/bar` and defers to the interpreter. At runtime,
the interpreter looks up the mangled name. For an **exported** member, the
mangled name is `Foo__bar` (the `/` in the module path becomes the `__`
structural separator, per the name mangling scheme in `src/compiler/mangle.c`).
But the runtime dispatch mangles the literal symbol `Foo/bar` as `Foo_slbar`
(the injective encoding of `/` is `_sl`, not `__`). The names do not match, so
the lookup fails with "unknown function or operator 'Foo/bar'".

For a **non-exported** member, the binding stays in the global env under its
bare name `bar` (the M7 module-prefix mangling only applies to exported
globals). The runtime dispatch finds `bar` in the global env and calls it --
which is itself a visibility violation (a private member called from outside
its module), but one that happens to work.

## Fix directions

**A. Top-level qualified resolution in `elab_lookup_sym`.** When
`current_module_name` is NULL and the symbol contains `/`, split on the last
`/`, look up the module in `e->loaded_modules`, and resolve the member from
the module's `exports` array. This mirrors the existing cross-module path
but does not require `current_module` or an import -- the module was already
defined in the same eval session, so it is in `loaded_modules`. This is the
smallest change that makes the common case work.

**B. Fix the runtime dispatch name.** When the `TUR-W0040` fallback mangles
the symbol for runtime lookup, recognise the `Module/member` pattern and
mangle it with the `__` structural separator instead of `_sl`. This fixes
the runtime path but leaves the elaborator warning in place -- the call
still degrades to runtime dispatch, losing type information.

**C. Both.** A is the proper fix (elaborator resolves the name, no warning,
full type checking). B is a safety net for any path that still reaches
runtime dispatch.

## Affected contexts

- `tur eval` (interpreted top-level expressions)
- `tur run` / `tur check` / `tur build` (compiled top-level expressions in a
  file that has code after the `defmodule` form)
- `turi_eval` / `turi_eval_with_path` (C embedding API, used by editor plugin
  hosts like Trowel)

## Workaround

Call module members from inside a `defmodule` using bare names (which work
inside the module) or self-qualified names (`Foo/bar` inside
`(defmodule Foo ...)`). For cross-module access, use `import` inside a
`defmodule`. Avoid top-level qualified calls outside any module.

## Notes

- Dot notation (`Foo.bar`) is not valid Turmeric syntax for module member
  access. Turmeric uses slash notation (`Foo/bar`), as documented in the
  module system guide. The Trowel plugin system plan initially used dot
  notation by mistake; this report is about the slash notation case, which
  is the correct syntax but still fails at the top level.
- The `tur/` stdlib namespace is implicitly imported everywhere (see the
  special case in `elab_lookup_sym` at `elab_module.c:1928`), so
  `tur/list/map` etc. work at the top level. The bug only affects user-defined
  modules.
