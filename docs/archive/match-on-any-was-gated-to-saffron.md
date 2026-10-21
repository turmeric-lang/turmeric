# `match` on an `any` scrutinee was gated to the dynamic dialects, so plain Turmeric emitted uncompilable C

**Severity: medium (a hard `cc` error with no Turmeric-level diagnostic).**
**RESOLVED 2026-10-21.** First found and fixed 2026-09-18 on the branch
`claude/non-macos-windows-reports-6pz2pr`, which never merged; ported to `main`
(where the gate had become `lang_span_is_dynamic`) alongside
[any-widen-deep-drop-frees-borrowed-children](../reported/any-widen-deep-drop-frees-borrowed-children.md).

## Repro

```turmeric
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn peek [l : any] : int (match l (Cons h t) 1 (Nil) 0))
(defn main [] : int (println (peek (:: (Nil) any))) 0)
```

```
error: invalid initializer
  tur_adt_Lst __scrut_v = (l);
```

Adding `#lang saffron` as the first line -- changing nothing else -- compiled
and ran the same program.

## Root cause

`src/compiler/elab_structs.c`, the `any`-scrutinee narrow in `elab_match`:

```c
if (scrutinee->type.kind == TY_ANY && lang_span_is_dynamic(call->span)) {
```

The narrow reads the arms, finds the one ADT they name, and inserts the checked
unbox that `cast` would. Without it the scrutinee stays `any` and the emitter
takes its by-value-sum arm, which spells `T __scrut_v = (<tur_tagged_t>)` --
exactly the C the narrow's own comment predicts. The gate described where the
shape ARRIVES by default (an unannotated parameter is `any` in a dynamic
dialect), not where it applies: `[l : any]` is a Turmeric signature too.

## Fix

The narrow is keyed on the scrutinee's type for a non-parametric ADT. It stays
CHECKED: an `any` holding something else panics with the standard cast message
rather than reinterpreting the payload.

The one part that IS dialect-specific is a parametric ADT. Its narrow to
`(Shape any)` is sound only because a dynamic dialect's constructor builds at
`any`; a plain-Turmeric `(Sq 6)` builds a `(Shape int)`, so the same narrow
would compile and then panic "different instantiation". Outside a dynamic
dialect that case is now a diagnostic naming the fix --
`(match (cast s (Shape int)) ...)` -- instead of a C error about aggregates.

Pinned by `tests/fixtures/match-on-any-plain-turmeric` (the tag read, an arm
binder read through the narrow, and an already-typed scrutinee that must keep
the ordinary path) and `tests/fixtures/errors/match-on-any-parametric-plain`.

## Note for whoever removes the next dialect gate

On the original branch, removing this gate immediately exposed a latent double
free in plain Turmeric (`any_expr_is_owned_temp`) -- the shape had been
unreachable there because it did not compile. A dialect gate can be hiding a
bug rather than scoping a feature; run `tests/run-leak-check.sh` over the newly
reachable shapes before assuming otherwise.
