# Generators: no parameter type, and a generator captured by a closure does not build

**Severity: medium.** Found 2026-10-19 checking the examples for the generator
forms' API reference (`stdlib/forms/generators.tur`).

**Narrowed 2026-10-21.** Shared captures of a scalar `^mut`, `yield` type
checks, `yield` in a nested `fn`, and `gen-unwrap` of an exhausted generator
are resolved: see
[generator-capture-yield-and-exhausted-reads](../archive/generator-capture-yield-and-exhausted-reads.md).

## 1. No way to type a generator parameter

`(defn g [x : generator] : int (gen-unwrap (gen-next x)))` is
`gen-next expects a generator value, got tyvar`; with no annotation the parameter
defaults to `int` and fails the same way. `(Generator int)` is a kind error
(TUR-E0012). So a generator can be consumed only where it is created or through
the `gen-for-each` / `gen-collect` / `gen-nth` macros. A struct yield is not
usable either: `(.a (gen-unwrap (gen-next g)))` is `no typeclass method found
for 'a'`, because `gen-unwrap` is typed `int`.

## 2. A generator captured by a closure does not build

```turmeric
(let [g (gen [] (yield 1))
      r (catch-unwind (fn [] : int (gen-unwrap (gen-next g))))]
  ...)
```

Compiled: `'__tur_gen_hdr_t' undeclared` in the lifted `fn` (the interpreter and
`tur jit`'s front end accept it; the jit's C step fails the same way). The
generator header typedef is emitted with the generator's own functions, after
the lambda that reads it. Same root as item 1: a generator is only usable where
it is made.

## Fix directions

1. Give generators a parameter type (the `Seq` combinators already pass them
   through generics) and a typed `gen-unwrap`.
2. Emit `__tur_gen_hdr_t` in the preamble (or ahead of any lifted function that
   names it) instead of with each generator.
