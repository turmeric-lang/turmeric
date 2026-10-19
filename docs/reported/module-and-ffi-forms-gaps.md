# The `extern-c` caret shorthand mis-types floats and zero-argument functions, and an unknown C symbol returns 0 interpreted

**Severity: medium.** Found 2026-10-19 checking the examples for the module and FFI
forms' API reference (`stdlib/forms/modules.tur`, `stdlib/forms/ffi.tur`). Each
repro was run on `tur run` and `tur --interpret`.

## 1. `(extern-c sqrt [^float] :float)` declares an integer parameter

```turmeric
(extern-c sqrt [^float] :float)
(defn main [] : int (println (sqrt 4)) 0)
```

Compiles with only a C warning and prints `9.08662e-159` (compiled) or
`4.44552e-162` (interpreted): `sqrt` is called with the integer 4 in an integer
register where the C function reads a double. `(sqrt 2.25)` is
`TUR-E0001: function 'sqrt' arg 1: expected int, got float`, which shows the
parameter was taken as `int`. The same declaration in the named form,
`(extern-c sqrt [x : float] : float)`, is right (`(sqrt 2.25)` is 1.5 on both
engines). `^cstr`, `^ptr` and `^int` work, which is all the stdlib uses
(`stdlib/io.tur`), but c-integration-guide's table presents the shorthand as taking
any of the types. Fix: parse `^float` (and the sized names) as types, or reject a
`^`-token it does not know.

## 2. `[^]` is a function of one argument

c-integration-guide declares globals and nullary functions with an empty
parameter list written `[^]`: `(extern-c rand [^] :int)`, accessed as `(rand)`.

```turmeric
(extern-c getpid [^] :int32)
(defn main [] : int (println (>= (as int (getpid)) 1)) 0)
```

`tur check`: `cannot cast non-numeric type '(fn [int] : int32)' with 'as'` --
`(getpid)` is a function awaiting one `int`, not the pid. `[]` is the nullary form
(`(extern-c getpid [] : int32)` works), and the stdlib writes `[]`
(`tur_hamt_new [] :ptr`).

## 3. An unknown C symbol is called as 0 when interpreted

```turmeric
(extern-c nosuchfn_xyz [^int] :int)
(defn main [] : int (println (nosuchfn_xyz 1)) 0)
```

`tur run`: linker error `undefined reference to 'nosuchfn_xyz'`. `tur --interpret`
prints `0` and exits 0. A misspelt C name in a program that is only ever run
interpreted returns a plausible value. It should say `unknown symbol` (the
sandbox path already reports missing natives).

## 4. Smaller things

- `(load "helper.tur")` resolves a non-stdlib path against the directory `tur` is
  run from, not the directory of the file with the `load`; `import` resolves
  against the file. From another directory the same program fails with
  `load: cannot open 'helper.tur'`. The module guide describes `load` as splicing
  "in place" without saying which directory.
- An `extern-c` prototype is emitted beside the system headers', so a declaration
  that differs from the real one in width (`(extern-c getpid [] : int)`, C's
  `int`) is a C error (`conflicting types for 'getpid'`) and not a diagnostic from
  `tur`. Declaring `: int32` works.
