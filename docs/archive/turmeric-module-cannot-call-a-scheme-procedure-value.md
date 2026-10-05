# Seam: a Turmeric module cannot call a Scheme procedure it was handed

**Severity:** medium (expressiveness; blocks the obvious SRFI 18 design).
A Scheme program can pass a procedure to a Turmeric function, but the
Turmeric function cannot call it. A parameter typed `any`, or left
untyped, is refused at compile time with `'f' is not a function or
continuation`. The workaround is the relay shape (the Turmeric side calls a
*named* Scheme export, which looks the procedure up), which every
`r7rs-threads-*` fixture uses. Found 2026-10-03 designing SRFI 18
(docs/upcoming/r7rs-srfi-18-216-sicp-plan.md).

## Repro

```turmeric
;; callit.tur
(defmodule callit
  (export call0)
  (defn call0 [f] : any
    (f)))
```

```scheme
#lang r7rs
(import (scheme base) (scheme write) (turmeric callit))
(write (call0 (lambda () 42)))
(newline)
```

```
$ tur run -I . input.tur
callit.tur:5:5: error: 'f' is not a function or continuation
```

The same with `[f : any]`. (Measured on origin/main b87ca3553, compiled.)

The prelude's `r7rs-apply-list__` (stdlib/r7rs/prelude.tur) calls an
untyped `f` in exactly this way and compiles, so the prelude evidently gets
a dynamic-call rule a user's Turmeric module does not. Which rule, and what
gates it, is not traced: start at the two "is not a function or
continuation" sites in src/compiler/elab_call.c (around lines 4426 and
7101).

## Why it matters

- **SRFI 18's `make-thread`** has to run an arbitrary thunk on a new
  pthread. Through the relay shape it works (measured: two thunks on real
  pthreads, joined, under `TUR_GC_TORTURE=50`, 3 of 3 runs), but only by
  routing every start through a Scheme-side registry.
- Any Turmeric library that wants to take a callback from Scheme (a sort
  comparator, an event handler) hits the same wall.

## Fix directions

- Let a Turmeric module call an `any` value with the same dynamic call the
  prelude uses (`emit_dyn_call` / `__tur_dyn_call_var`), result `any`. A
  mistyped value is then a runtime "not a procedure" error, as in Scheme.
- Or offer an explicit seam primitive, `(scheme-call f arg ...)`, exported
  from a `(turmeric stdlib/r7rs-call)`-style module, keeping the static
  refusal for plain Turmeric code.

Either way, document it in r7rs-guide.md's seam section next to "Each
argument crossing into a typed Turmeric function is checked".

## Resolution (2026-10-05)

The first direction. The elaborator made a call through an `any` value a
dynamic call (`saffron_dyn_call_on`) only in a dynamic dialect's file
(`lang_span_is_dynamic`); it now does in every dialect, both for a binding
of type `any` and for a call head of that type (src/compiler/elab_call.c).
The callee's tag is checked at run time, so a value that is not a procedure
is the same catchable "not a procedure" error Scheme gives, and the result
is `any`. A parameter left untyped in a Turmeric file is still an `int`
(the language's default), so the callback is written `[f : any]`.

A plain Turmeric program making such a call then hit a C clash: the
dynamic-call helpers forward-declared `__tur_any_type_name` `static`, and a
unit linked against the runtime archive declares it non-static. The forward
declaration was redundant (the definition or the archive's prototype always
comes first) and is gone (src/compiler/emit_module.c).

SRFI 18 (stdlib/r7rs/thread.tur) did not need this in the end -- a
prelude-shaped file calls Scheme procedures already -- but a Turmeric
library taking a callback from Scheme does. Pinned by
`tests/fixtures/r7rs-turmeric-calls-scheme-procedure` and
`tests/fixtures/any-value-call`, on both back ends.
