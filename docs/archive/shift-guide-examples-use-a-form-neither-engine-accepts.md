# The shift/reset guide documents a `shift` form neither engine accepts

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `delimited-control-guide-examples`.

**Severity:** medium (documentation). The main user guide for delimited
control teaches a syntax and semantics that do not exist, and the behavior
that does exist is described only in a later "abortive shift" subsection.

## Repro

`docs/guides/delimited-control-operators-guide.md`, section "`shift` / `reset`
-- static, re-delimiting":

```turmeric
(reset
  (+ 1 (shift k (k (k 10)))))
;; => 12
```

Both `tur run` and `tur --interpret` reject it:

```
error: unbound symbol 'k'
```

What the language implements is `(shift receiver value)`. The shift is
**abortive**: it delivers `receiver(value)` to the enclosing `reset` and
discards the context in between.

```turmeric
(reset (+ 1 (shift (fn [v : int] (* 2 v)) 10)))   ;; => 20, not 21
```

The same guide's `shift0` example already uses the receiver form, so the
guide contradicts itself. `async-await-guide.md` "Desugaring" (marked
"conceptually") also uses the `(shift k body)` spelling.

## Root cause

The guide was written against Danvy-Filinski `shift` (which binds `k` and
lets the body call it). Base `shift` was implemented as the abortive
receiver form (`elab_effects.c`; `CT_SHIFT` in `src/passes/cps_ir.c`, whose
body is "apply the receiver to the value"). The guide's own "Abortive shift"
subsection, under "How it lowers", says so. The capturing behavior the
earlier sections describe is what `cloneable-shift` / `call/cc*` provide.

## Fix directions

Either:

- **Fix the guide:** rewrite the `shift`/`shift0` sections around the
  receiver form, show the abortive semantics first, and point to
  `cloneable-shift` for a `k` the body can call (with a runnable example of
  each); or
- **Fix the language:** add the `(shift k body)` binding form, if it is meant
  to exist.

In either case, make every example in the guide a fixture, or at least
compile-check it, so the guide cannot drift from the language again.

## Resolution (2026-10-17)

The guide was fixed, not the language. "`shift` / `reset`" now opens with the
abortive `(shift receiver value)` form, says plainly that
`(shift k (k (k 10)))` is not a Turmeric form, and shows `cloneable-shift` for
a `k` the body can call (`(k (k 10))` under `(+ 1 _)` gives 12). The overview
says the same, and `async-await-guide.md`'s "Desugaring" notes that its
`(shift k body)` is notation. Every example in the guide is in the
`delimited-control-guide-examples` fixture, so an engine change that breaks
one fails `run.sh` and `run-turi.sh`.
