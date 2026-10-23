# Generators: no parameter type

**ARCHIVED 2026-10-21** -- split into reports and plans; see the disposition below.

## Disposition (2026-10-21) -- archived

- Item 1 (no generator parameter type; `gen-unwrap` typed `int`): decision -- add
  `(Generator T)` and a typed `gen-unwrap`. Plan:
  [generator-type-plan](generator-type-plan.md) (done 2026-10-23).
- The rest is resolved: see
  [generator-capture-yield-and-exhausted-reads](generator-capture-yield-and-exhausted-reads.md).

**Severity: medium.** Found 2026-10-19 checking the examples for the generator
forms' API reference (`stdlib/forms/generators.tur`).

**Narrowed 2026-10-21.** Shared captures of a scalar `^mut`, `yield` type
checks, `yield` in a nested `fn`, and `gen-unwrap` of an exhausted generator
are resolved, and so (same day, second pass) is a generator captured by a
`fn` literal: see
[generator-capture-yield-and-exhausted-reads](generator-capture-yield-and-exhausted-reads.md).

## 1. No way to type a generator parameter

`(defn g [x : generator] : int (gen-unwrap (gen-next x)))` is
`gen-next expects a generator value, got tyvar`; with no annotation the parameter
defaults to `int` and fails the same way. `(Generator int)` is a kind error
(TUR-E0012). So a generator can be consumed only where it is created or through
the `gen-for-each` / `gen-collect` / `gen-nth` macros. A struct yield is not
usable either: `(.a (gen-unwrap (gen-next g)))` is `no typeclass method found
for 'a'`, because `gen-unwrap` is typed `int`.

## Fix directions

1. Give generators a parameter type (the `Seq` combinators already pass them
   through generics) and a typed `gen-unwrap`.
