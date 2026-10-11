# A module's export and a `load`ed global with the same name get one C name

**Status: RESOLVED 2026-10-11** (see [Resolution](#resolution)).

**Severity: medium (compile failure, or a call type-checked against the wrong
signature).** Found 2026-10-10 putting tourist on stdlib httpd
(tourist-on-stdlib-httpd-plan H4): stdlib `httpd.tur` defines a global
`router-free [r : ptr<void>]`, and the `tourist/router` module exports its own
`router-free [pat : Pattern]`. Once a tourist program also has stdlib httpd in
it (every tourist program does since H2), the two collide.

## Repro

```sh
mkdir -p /tmp/clash/src/m && cd /tmp/clash
cat > lib.tur <<'X'
(defn foo-free [x : ptr<void>] : nil
  ```c (void)x; printf("global foo-free\n"); ```)
X
cat > src/m/b.tur <<'X'
(defmodule m/b
  (export foo-free)
  (defn foo-free [x : int] : void
    ```c printf("m/b foo-free %lld\n", (long long)x); ```))
X
cat > src/m/c.tur <<'X'
(load "lib.tur")
(defmodule m/c
  (import m/b :refer [foo-free])
  (export run)
  (defn run [] : int
    (foo-free 5)
    0))
X
cat > main.tur <<'X'
(defmodule main
  (import m/c :refer [run])
  (defn main [] : int (run)))
X
tur run -I src -I . main.tur
```

```
error: conflicting types for 'm__b__foo_hyfree'; have 'void(int64_t)'
note: previous declaration of 'm__b__foo_hyfree' with type 'void(void *)'
...
tur: cc invocation failed
```

Both definitions are emitted under the module-qualified name `m__b__foo_hyfree`.

A milder shape needs no `load` in the importing file: in tourist, `dsl.tur`
imports `router-free` from `tourist/router` with `:refer`, yet the call
`(router-free (:: p :Pattern))` was emitted as
`tourist__router__router_hyfree((void *)...)` -- the right symbol called with
stdlib's `ptr<void>` signature (a `-Wint-conversion` warning, harmless only
because both are pointer-sized). The explicitly referred import lost to the
same-named global during type checking.

## Root cause (not yet located)

The C-name assignment for a module export and for a top-level global from a
`load`ed file share a key, so whichever is registered second is given the
first's mangled name. Type resolution of a bare call likewise prefers the global
over a `:refer` import. Start at `mangle_module_name` and the import handling
around `emit_module.c:21336`.

## Fix directions

- Key C names by (module, name); a global from a `load`ed file keeps its bare
  mangled name.
- A `:refer`'d import should shadow a same-named global inside the importing
  module, for type checking and emission alike.

## Workaround in use

tourist renamed its function to `pattern-free` (turmeric-spices, tourist
v0.3.0), so nothing in the httpd stack shares a name with stdlib httpd now.

## Resolution

Not two keys colliding: one BINDING shared. `elab_defn`
(`src/compiler/elab_fns.c`, "Create/update binding for the function") reuses
an existing global binding of the same name when it belongs to the same module
or to "no module yet" -- meant for a pass-1 forward declaration, whose module
is filled in when the defn arrives. The `load`ed `foo-free` is module-less
too, but it already HAS its definition. So `m/b`'s `defn foo-free` took it
over: one binding, re-stamped `defining_module_name = m/b`, carrying two
definitions -- both emitted as `m__b__foo_hyfree`. Every name lookup then
found that single binding, which is the milder shape (the `:refer`'d call
checked against whichever signature was written last).

A module-less binding is reused only while it is a forward declaration with no
definition (`source_fn_def` unset), or for a redefinition at top level (no
current module). A module defn beside a defined top-level global of the same
name now gets its own binding, so the `:refer` import and the global are two
functions with two C names, and `(foo-free 5)` in `m/c` calls `m/b`'s.

**Pinned by** `module-export-beside-loaded-global` (the report's repro; the
milder shape -- the `load` in the main file, not the importing one -- was
checked by hand and calls `m/b`'s too).

**Spices follow-up (not in this repo):** tourist's `router-free` ->
`pattern-free` rename can stay or go; it is no longer needed.
