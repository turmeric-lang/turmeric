# The `extern-c` caret shorthand mis-types floats and zero-argument functions, and an unknown C symbol returns 0 interpreted

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`extern-c-caret-shorthand` and `load-relative-to-loading-file` (both engines),
`errors/extern-c-caret-unknown-type`, `errors/extern-c-lone-caret-with-params`
and `errors/extern-c-unknown-symbol-interp` (interpreter).

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

## Resolution (2026-10-21)

1. **`^float` and the sized names** are read as the `:type` keyword form reads
   them (`typekind_from_symbol` plus its legacy `void`), in `elab_extern_c`
   (`src/compiler/elab_fns.c`). `^ptr<T>` is a pointer of any pointee, as
   `:ptr` is. Any other `^`-token is an error naming it (`extern-c: unknown
   parameter type ^zork`) instead of defaulting to `int`. Fixing this turned
   up a second fault in the same loop: the token after a `^type` was always
   taken as the parameter's name, so `[^float ^float]` -- and stdlib
   `io.tur`'s `(extern-c fopen [^cstr ^cstr] :ptr)` -- declared ONE parameter
   named `^float`. A `^type` followed by another `^type` is now an anonymous
   parameter, and the second starts the next one.
2. **`[^]`** is an empty parameter list, as c-integration-guide (and stdlib
   `random.tur`'s `rand`) already used it. A lone `^` beside other parameters
   is an error.
3. **An unknown C symbol under `--interpret`** is bound to a native that fails
   when called (`extern-c 'nosuchfn_xyz': no C symbol of that name in this
   process ...`, `register_extern_c_unavailable`, `src/turi/eval.c`), rather
   than to the nil stub. The declaration alone stays legal, as an unused one
   links compiled. A by-value aggregate signature the FFI could not bind gets
   the same treatment; the no-JIT build and the signatures the thunk
   vocabulary cannot express keep the nil stub.
4. **Smaller things.**
   - A relative `load` path other than `stdlib/...` is looked for beside the
     file holding the `load` first (`load_expand_forms`,
     `src/compiler/elab_toplevel.c`), then cwd-relative, so the corpus's
     root-relative paths are unchanged. module-system-guide and the `load`
     reference page say so.
   - The prototype clash (`(extern-c getpid [] : int)` beside `<unistd.h>`'s
     `int getpid(void)`) stays a C compiler error by design: `tur` has no copy
     of the system headers to check a declaration against, and the C rule is
     the right one. c-integration-guide and the `extern-c` page say to declare
     the real width (`: int32`).

