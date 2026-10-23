# A top-level `define` of `(command-line)` sees `("")`, not the arguments

> Severity: medium -- a plain R7RS program reads the wrong command line,
> silently, in the most natural place to read it.
> Found executing docs/upcoming/r7rs-heap-frames-plan.md (2026-10-23).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write) (scheme process-context))
(define n (command-line))
(write n)
(newline)
(write (command-line))
(newline)
```

```sh
tur build cl.scm -o cl && ./cl a b
("")
("./cl" "a" "b")
```

The same `(command-line)` gives the right list inside a procedure or at a
top-level expression, and the empty one in a `define`'s initializer, so
`(define n (string->number (cadr (command-line))))` fails with
`car: not a pair ()`. `tur run cl.scm a b` behaves the same.

## Root cause

A top-level `define` whose initializer the emitter hoists into
`__tur_module_def_init` runs there, and that runs from `__tur_static_init`,
which a constructor (`__tur_static_init_ctor`) calls before `main`. `main`
builds `g_tur_args` and sets `g_tur_argv0` from `argv` only after that (the
`*args*` block at the top of the emitted `main`), so the initializer reads
the empty list and the `""` program name. In the emitted C of the repro,
`r7rs_hycommand_hyline()` is called inside `__tur_module_def_init`, ahead of
`g_tur_args = 0;` and the loop in `main`.

## Fix directions

- Fill `g_tur_args` / `g_tur_argv0` before the module initializers run: the
  constructor cannot see `argv` portably, but `main` could run the def-init
  itself after the `*args*` block when it is the first caller (the
  constructor exists for the no-`main` cases: `--shared`, separate
  compilation).
- Or keep initializers that call a procedure out of
  `__tur_module_def_init` in a `#lang r7rs` program, evaluating them in
  program order from `main` as the other top-level forms are -- which is also
  what R7RS's program semantics asks for (a `define` is evaluated when the
  program reaches it).
