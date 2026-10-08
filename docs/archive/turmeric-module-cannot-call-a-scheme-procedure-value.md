# Seam: a Turmeric module cannot call a Scheme procedure it was handed

**Workaround found 2026-10-04 (still open).** Inside an r7rs build, a
Turmeric module can already call a Scheme procedure through the prelude's
own `apply`: `(defn call2 [f : any a : any b : any] : any (r7rs-apply f
(r7rs-list a b)))`. `(call2 (lambda (x y) (list y x)) 1 "two")` gives
`("two" 1)`, and `(call2 + 3 4)` gives `7`, on both back ends (origin/main
ab9be034). That covers the SRFI 18 thunk case without the relay. It is not a
fix: `r7rs-apply` and `r7rs-list` are the prelude's internal names, not a
documented seam, and a plain `(f)` is still refused.

Why it is refused: `elab_call.c` (the "saffron-lang-plan S4/D4 (G5)" block,
just above the second "is not a function or continuation" site) turns a call
through an `any` binding into a dynamic call only when
`lang_span_is_dynamic(call->span)`. That is true for a `#lang saffron` or
`#lang r7rs` file, and so for the prelude; it is never true for a `#lang
turmeric` module. Fix direction 1 is therefore a change to the Turmeric
language, which belongs behind an `--enable` experiment with a plan
(CLAUDE.md, "Experimental Compiler Features"). Direction 2 could ship as a
documented, exported wrapper over `r7rs-apply`.

**Severity:** medium (expressiveness; blocks the obvious SRFI 18 design).
A Scheme program can pass a procedure to a Turmeric function, but the
Turmeric function cannot call it. A parameter typed `any`, or left
untyped, is refused at compile time with `'f' is not a function or
continuation`. The workaround is the relay shape (the Turmeric side calls a
*named* Scheme export, which looks the procedure up), which every
`r7rs-threads-*` fixture uses. Found 2026-10-03 designing SRFI 18
(docs/archive/r7rs-srfi-18-216-sicp-plan.md).

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
