# Generators: captured variables are copied (and diverge from the interpreter), `yield` types are unchecked, `yield` in a nested `fn` does not compile

**Severity: medium.** Found 2026-10-19 checking the examples for the generator
forms' API reference (`stdlib/forms/generators.tur`). Each repro below was run
on both engines.

## 1. A compiled generator copies the variables it captures

```turmeric
(defn main [] : int
  (let [^mut n 1
        g (gen [] (yield n))]
    (set! n 2)
    (println (gen-unwrap (gen-next g))))   ; compiled: 1   interpreted: 2
  0)
```

The compiled generator takes its own copy of `n` when it is created. A `set!`
of a captured variable *inside* the body changes that copy only:

```turmeric
(let [^mut n 0
      g (gen [] (set! n (+ n 5)) (yield n))]
  (println (gen-unwrap (gen-next g)))      ; 5 on both
  (println n))                             ; compiled: 0   interpreted: 5
```

and when the body only sets the variable and never reads it, the compiled
program does not build:

```turmeric
(let [^mut n 0
      g (gen [] (set! n 5) (yield 1))]
  (gen-next g)
  (println n))     ; compiled: C error  'n_3' undeclared
```

Likely cause: `collect_free_vars` records the *value* of an `EX_SET` but not its
target binding (`src/compiler/elab_core.c`, `case EX_SET`, near line 983), so a
variable that is only assigned is never captured into the state struct.

## 2. `yield` payload types are not checked

`(gen [] (yield 1) (yield 2.5))`: the second `gen-unwrap` yields
`4612811918334230528`, the bits of 2.5 read as an int, on both engines. The first
`yield` fixes the element type and later ones are not compared with it.

## 3. `yield` inside a nested `fn` literal does not compile

```turmeric
(let [g (gen [] (let [f (fn [x : int] : nil (yield x))] (f 1) (yield 2)))]
  (gen-unwrap (gen-next g)))
```

Compiled: `'null' undeclared` and `label '__yield_0' used but not defined` from
the C compiler. Interpreted: it runs and yields 1. It should be a diagnostic like
TUR-E0702/E0703.

## 4. Reading an exhausted generator crashes

`(let [g (gen [] (yield 1))] (gen-next g) (gen-unwrap (gen-next g)))`: segfault
compiled, `0` interpreted. `gen-next` returns null once the body ends and
`gen-unwrap` dereferences it; callers must test `gen-some?` first.

## 5. No way to type a generator parameter

`(defn g [x : generator] : int (gen-unwrap (gen-next x)))` is
`gen-next expects a generator value, got tyvar`; with no annotation the parameter
defaults to `int` and fails the same way. `(Generator int)` is a kind error
(TUR-E0012). So a generator can be consumed only where it is created or through
the `gen-for-each` / `gen-collect` / `gen-nth` macros. A struct yield is not
usable either: `(.a (gen-unwrap (gen-next g)))` is `no typeclass method found
for 'a'`, because `gen-unwrap` is typed `int`.

## Fix directions

1. Record an `EX_SET` target as a capture, and decide one rule for captured
   mutable state (share it by reference, as the interpreter does, or reject a
   `set!`/capture of a `^mut` variable); pin it with a fixture on both engines.
2. Check each `yield` against the first.
3. Diagnose `yield` under a nested `fn`.
4. Make `gen-unwrap` of null a defined result, or a `Result`-style reader.
5. Give generators a parameter type (the `Seq` combinators already pass them
   through generics) and a typed `gen-unwrap`.
