# Specializations of same-named generics in two modules collide at link

**Severity:** medium (link failure). Two modules may each export a generic
with the same name. Separately compiled, their specializations at the same
types get the same C symbol.

## Repro

```turmeric
(defmodule ma (export ident) (defn ident [A] [x : A] : A x))
(defmodule mb (export ident) (defn ident [A] [x : A] : A x))
(defmodule user1
  (export shout)
  (import mb :as b)
  (defn shout [] : void (println (b/ident "u"))))
(defmodule app
  (import ma :as a)
  (import user1 :refer [shout])
  (defn main [] : int
    (println (a/ident "a"))
    (shout)
    0))
```

(one module per file, no `build.tur`, `tur build .`):

```
multiple definition of `ident__spec__const_char___const_char__'
```

## Root cause

`src/compiler/emit_module.c:emit_abi_clone_name` builds a spec name from the
callee's **bare** name plus the result and argument C types. The module's
mangled prefix is not included. Within one TU the `__h<n>` clash suffix keeps
names apart, but two TUs cannot see each other's names.

## Fix directions

- Include the defining module's mangled prefix (see
  [name-mangling-guide](../guides/name-mangling-guide.md)) in spec clone names.
  Update the "How to read a spec symbol" section of
  [monomorphization-abi-guide](../guides/monomorphization-abi-guide.md).

## Resolution (2026-10-21)

Fix direction 1.

- **Fix.** `emit_abi_clone_name` (`src/compiler/emit_module.c`) writes the
  module prefix the binding's own C name carries in front of the name:
  `ma__ident__spec__const_char___const_char__` and `mb__ident__spec__...`.
  The prefix comes from `binding_module_prefix` (`src/compiler/emit_core.c`),
  factored out of `raw_name_for_binding` so the two cannot drift: a global of
  a named module gets `<mangled module>__`, anything else (a top-level defn,
  `main`, a local) gets nothing, so a single-file program's spec names are
  unchanged.
- **Every consumer agrees by construction.** The caller's TU and the owning
  TU both build the name through `emit_abi_clone_name` from the same binding,
  and a forced spec carries the name it was given (`ForcedAbiSpec.clone_name`).
  The env-struct names of inner-closure specs (`__env_N__spec__...`) are
  file-local and were left alone.
- **Guides.** The "How to read a spec symbol" section of
  `monomorphization-abi-guide.md` and the clone-name, ABI-cache, failure-shape
  and worked-failure passages of `separate-compilation-internals-guide.md`.
- **Pinned** by `build-project-spec-same-name-two-modules` in
  `tests/run-build-project.sh`: the repro above as a bare-directory build,
  which links and prints `a` and `u`, and whose `ma.c` / `mb.c` hold the
  prefixed names.
