# A carrier-represented ADT passes a `: int` or `: cstr` return check

**Severity:** medium. A type error that is not reported: the function compiles,
and its caller reads a pointer as a number or a string.

Found graduating [quote-list-data-plan](../archive/quote-list-data-plan.md),
where it matters more than before: a quoted list is a `Datum`, which has exactly
this shape, so a macro that used `(quote (form ...))` as its template -- which
used to mean the form -- now expands to a `Datum` that a `: int` function
returns without complaint (`errors/nil-tail-macro-expanded-definition` did this
until its template became a quasiquote).

## Repro

```turmeric
(load "stdlib/list-typed.tur")
(defdata T2 :copy (A int) (B (List T2)))
(defn f [] : int (A 3))     ; accepted
(defn g [] : cstr (A 3))    ; accepted
(defn main [] : int (f))    ; exits with the low byte of a pointer
```

`tur check` exits 0 on `main` (v0.70.1). `'(a b)` in place of `(A 3)` is the
same: `(defn h [] : int '(a b))` is accepted. A non-`:copy` recursive ADT, or a
by-value record, is rejected with TUR-E0709 ("an aggregate is a real C type").

## Root cause

`return_position_conflict` (`src/compiler/elab_core.c`) rejects a body type
against a scalar declaration only through `return_type_carrier_aggregate_conflict`,
which fires for an aggregate that is a real C type. A `:copy` ADT with a
`(List T)` field is represented as the int64 carrier word, so it reads as "both
sides are int64_t" -- the deliberate carrier bridge the generic and `#{Unsafe}`
classes rely on -- and passes even in a committed (monomorphic, non-unsafe)
function, where no bridge was asked for.

## Fix directions

1. In a `RET_CLASS_COMMITTED` position, treat a nominal ADT body against a
   scalar declaration (`int`, the integer family, `float`, `cstr`, `bool`) as a
   conflict whatever its representation, as the bool-vs-integer and
   reverse pointer-scalar checks already are. Run the corpus: stdlib code that
   erases an ADT through `: int` on purpose should be using an ascription.
2. The same question for a `let` binding's annotation and a call argument, if
   they share the predicate.
