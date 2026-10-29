# A `(Cons T)` monomorph's constructor is emitted before `T`'s struct

**Severity: medium (a valid program fails in the C compiler; the import order decides).**
Found 2026-10-29 cleaning up after module-type-names-collide-across-modules.

## Repro

In turmeric-spices, `spices/tourist/tests/markup_test.tur` imports
`tourist/markup`, `html/markup`, `html/hiccup` and `dom/holes`. Add one import
above them:

```turmeric
(import tourist/app :refer [tourist])
```

and run the file (`tur run tests/markup_test.tur` in `spices/tourist`). It
fails in cc, on `main` as well as with the collision fix:

```
error: parameter 1 ('_0') has incomplete type
error: invalid application of 'sizeof' to incomplete type 'tur_adt_Hole'
```

The same import placed after the markup imports compiles and passes.

## Root cause (partial)

The emitted C has `ctor_Cons_Cons__Hole(tur_adt_Hole _0, ...)`, which takes a
`dom/holes` `Hole` by value and `sizeof`s it, about 1200 lines before the
`#ifndef TUR_TD_tur_adt_Hole` block that defines `struct tur_adt_Hole`; only
the forward `typedef struct tur_adt_Hole tur_adt_Hole;` precedes it. A
`(Cons X)` instance tourist/app needs is already emitted when `tourist/app`
is imported first, and the `Cons__Hole` instance seems to be written at that
group's position, not after `Hole`. Not traced further: the ADT emission order
is in src/compiler/emit_module.c (Pass 0).

## A one-module repro (added 2026-10-29)

Found executing stats-formula-plan. Any struct with an `Option` field, in the
same program as a `(Cons T)` of a user `defdata` T, puts the `Cons T`
instance first -- for a `^heap` T it then has no typedef for T at all:

```turmeric
(defmodule rp/c
  (export main)
  (defstruct Unrelated [x : (Option int)])
  (defdata ^heap ^copy Foo (Foo int))
  (defn mk [n : int acc : (Cons Foo)] : (Cons Foo)
    (if (= n 0) acc (mk (- n 1) (tcons-of (Foo n) acc))))
  (defn main [] : int (println (tlength (mk 3 (tnil)))) 0))
```

```
error: unknown type name 'tur_adt_Foo'
```

Delete the `Unrelated` line (or make its field an `int`) and it prints 3. The
`Option` instance, emitted with the stdlib's list instances, seems to pull
every `Cons` instance in the program forward with it. Reproduces on `main`
(1ecdacc9) and v0.72.0. spices/stats-formula keeps every `Option` out of its
struct fields for this reason (an `ErrPos` ADT for the error position, an
`Lhs` ADT for the response).

## Fix directions

- Emit a monomorphic instance after the definitions of its type arguments
  (a topological order over the instances and the ADTs they take by value).
- At least: emit a by-value instance's constructor after every complete
  struct definition, since only the constructor needs the argument complete.
