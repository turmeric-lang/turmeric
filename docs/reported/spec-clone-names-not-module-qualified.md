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
