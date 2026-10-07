# No documented way for Turmeric code to call a procedure it holds as `any`

**RESOLVED 2026-10-07: the gap was already closed, and was undocumented.**
The report says calling an `any` as `(f x)` is a compile error in `#lang
turmeric` and that the language change lived on an unmerged branch
(`868fea84d`). That commit is on `main` ("Calling an any value is a dynamic
call in every dialect", 2026-10-05), and it archived the parent report
([turmeric-module-cannot-call-a-scheme-procedure-value](turmeric-module-cannot-call-a-scheme-procedure-value.md))
with fixtures `r7rs-turmeric-calls-scheme-procedure` and `any-value-call`.
Verified 2026-10-07 on both back ends: a Turmeric module's `(f)` and `(f a b)`
on `any` call the Scheme procedure, and the failure side matches the
proposal's wish list -- a wrong argument count and a non-procedure raise
Scheme error objects (`wrong number of arguments (2 given)`, `not a
procedure`) that the Scheme caller can `guard`.

So the proposed `dyn-call` / `dyn-apply` / `dyn-procedure?` library is not
needed and was not added (a prototype as a Scheme `define-library` worked, but
would only duplicate the call). What was missing is what the title says:
documentation. `docs/guides/r7rs-guide.md` ("Libraries and Turmeric") now
shows the call and its errors, and `tests/run-r7rs-import.sh` case
`turmeric-calls-scheme-procedure-errors` pins the error side on both back ends.

**RESOLVED 2026-10-07** with the proposal's shape, written as a Scheme
library rather than a Turmeric file: `stdlib/dyn.scm` is `(define-library
(dyn) (export dyn-call dyn-apply dyn-procedure?) ...)`, and each export is
Scheme's own `apply` / `procedure?`. A Turmeric module imports it like any
Scheme library, `(import dyn :refer [dyn-call])`, so no compiler change and no
experiment row. Being Scheme, it gets the error behaviour the proposal asked
for for free: a wrong argument count raises an error object that a Scheme
caller can `guard`, on both back ends, and the arity limit is `apply`'s (eight
for a fixed-arity procedure, unbounded for a variadic one). `dyn-call` is
variadic; its fixed-arity cost is one rest list, the same `apply` pays.

Answers to the open questions: **r7rs only.** Under a plain Turmeric program
a Turmeric closure boxed as `any` reaches the same dynamic call but is
refused ("wrong number of arguments"), so the library documents itself as for
an `#lang r7rs` program and the Turmeric modules it imports. **The
interpreter** gives the same results and errors. Pinned by
`tests/run-r7rs-import.sh` case `turmeric-calls-scheme-procedure-via-dyn`
(compiled and `--interpret`): a thunk, a two-argument lambda, `+`,
`dyn-apply` over `max`, `dyn-procedure?` both ways, and a guarded arity
error. Documented in `docs/guides/r7rs-guide.md`, "Libraries and Turmeric".

Placement note: the file is `stdlib/dyn.scm`, not under `stdlib/r7rs/`. A
library there named `(r7rs dyn)` resolved, but its body did not get the
Scheme rename table (`apply` was unknown) -- that directory is the prelude's
own, spelled in `r7rs-` names.

**Severity:** low-medium (expressiveness). This is the fix half of
`turmeric-module-cannot-call-a-scheme-procedure-value.md` (fix direction 2
there), filed separately so that it can be picked up without the language
question. Filed 2026-10-05.

## The gap

A Turmeric module that receives a Scheme procedure (a thunk to run on a
thread, a comparator, an event handler) holds it as `any`. Calling it as
`(f x)` is a compile error in `#lang turmeric`: `'f' is not a function or
continuation`. Only a `#lang saffron` or `#lang r7rs` file gets the dynamic
call (`elab_call.c`, the `lang_span_is_dynamic` test in the "saffron-lang-plan
S4/D4 (G5)" block).

What works today, inside an r7rs build, is the prelude's own `apply`:

```turmeric
(defn call2 [f : any a : any b : any] : any
  (r7rs-apply f (r7rs-list a b)))
```

PR #1082 measured it on both back ends: `(call2 (lambda (x y) (list y x)) 1
"two")` gives `("two" 1)`. But `r7rs-apply` and `r7rs-list` are internal
names of `stdlib/r7rs/prelude.tur`, not a documented seam, and nothing
outside an r7rs build defines them.

## Why a function, not a language change

The alternative, letting `(f x)` on any `any` value compile everywhere, was
tried on an unmerged branch (`claude/r7rs-srfi-plan-execution-n85j1j`,
commit 868fea84d). It turns a compile-time error into a run-time check in all
typed Turmeric code, which is a language change and would need an
`--enable` experiment (CLAUDE.md, "Experimental Compiler Features").

An explicit function gets the same capability with none of that:

- The dynamic call is visible at the call site, so a reader knows that this
  one call is checked at run time.
- Calling an `any` by accident stays a compile error.
- No compiler change, so no experiment row.

## Proposal

A small module, for example `stdlib/dyn.tur` (all names are placeholders):

```turmeric
;;; dyn-call -- call a procedure held as `any`, checked at run time.
(defn dyn-call [f : any & args : any] : any ...)

;;; dyn-apply -- the same, with the arguments in a Scheme list.
(defn dyn-apply [f : any args : any] : any ...)

;;; procedure? -- whether an `any` holds something callable.
(defn dyn-procedure? [v : any] : bool ...)
```

- Inside an r7rs build these wrap `r7rs-apply` and `r7rs-list`, so they get
  Scheme's error objects ("f: too many arguments (expects 1, got 2)", "not a
  procedure") and its `raise`, which a Scheme caller can `guard`.
- `dyn-call` with zero or one argument is the common case (SRFI 18's thunk,
  a predicate). Check that the fixed-arity path does not pay for a rest list.
- Document the module in `docs/guides/r7rs-guide.md` next to "Each argument
  crossing into a typed Turmeric function is checked", and point the open
  report at it.

## Open questions

- **Outside an r7rs build.** A `#lang saffron` lambda is also an `any` that is
  callable. Should `stdlib/dyn.tur` work in a plain Turmeric program over the
  Saffron dynamic-call runtime (`ensure_saffron_dyn_runtime`,
  `src/compiler/emit_module.c`), or be r7rs-only at first? r7rs-only is enough
  for the use case that filed the original report.
- **Arity.** `r7rs-apply-list__` calls a procedure with up to 8 arguments.
  Past 8, `r7rs-apply-long__` spreads a variadic procedure's fixed arguments
  and hands it the rest as its rest list, so any variadic procedure takes a
  list of any length; a fixed-arity procedure with more than 8 parameters is
  refused. The wrapper inherits that limit; say so in its docstring.
- **The interpreter.** `tur --interpret` must give the same results and
  errors. Pin both back ends in `tests/run-r7rs-import.sh`.

Resolving this would narrow the original report to the language question
alone, which can then stay open with no pressure on it.
