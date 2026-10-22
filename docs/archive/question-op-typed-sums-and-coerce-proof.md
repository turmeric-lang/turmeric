# `?` on a typed `Result` / `Option`, and `coerce` with an applied `Equal` proof

**RESOLVED 2026-10-21** (items 1 and part of 5 of type-forms-gaps, plus doc fixes
for items 3 and 8). The rest stays open in
[type-forms-gaps](type-forms-gaps.md). Pinned by
`question-op-typed-sums` (all three engines), `coerce-applied-equal-proof` and
`errors/coerce-proof-about-other-type`.

## `?`

`(? expr)` lowered through `__tur-q-is-err?` / `__tur-q-ok-val`
(`stdlib/result.tur`), which take a `ptr<void>` and read a `struct { int tag;
int64_t payload; }` -- the old pointer-carried Result. `(Result T E)` is a
by-value ADT, so a typed operand failed the argument check, and interpreted the
helper could not even be loaded.

`elab_question` (`elab_forms.c`) now reads the operand's static type when it is a
variable or a call of a named function (`question_operand_sum`; the application
`(Result int cstr)` is peeled to its ADT). A `Result` or `Option` lowers to

```
(let [q expr] (if (err? q) (return (Err (err-val q))) (ok-val q)))
(let [q expr] (if (none? q) (return (None)) (unwrap q)))
```

plain Turmeric, so every engine runs it, and the ok type may differ from the
function's. A `match` was tried first and refused: its arm-type check does not
treat a `return` arm as divergent where `if` does. An operand of any other shape
keeps the carrier path, which the existing `result-question-op*` fixtures use.

## `coerce`

A proof annotated `(Equal int int)` is a type application of the bare ADT, and
`elab_coerce` accepted only the bare spelling. It peels the application and, when
both are concrete, checks the proof's first argument against `x`'s type. The
result is still `x` at its own type; taking the result type from the proof's `b`
needs a retyping node (the proof cannot exist when `a` and `b` differ, so such a
branch is vacuous) and stays open.

## Docs

hrt-guide's `(pack int 0)` / `(open c as [s v] v)` became the accepted
`(pack 0 (exists [s] s))` / `(open c [s v] body)`;
borrow-checking-and-lifetimes-guide's `(@ r)` became `@r` and now lists what the
checker does not enforce.

## Tried and reverted

Checking that the argument of a `forall` parameter is generic rejected ten-odd
existing fixtures that pass an `int`-only function and apply it at `int`
(`hrt-rank2-annotation`, `hrt-forall-constraint-show`, `currying-rank2-partial`,
...): the corpus relies on the argument being trusted. Rejecting `&mut` of a
non-`^mut` local and `&` of a literal broke `borrow-basic`,
`errors/borrow-conflict` and `saffron-borrowed-struct-key-widened` (a literal
borrowed as a call argument is deliberate there). Both stay documented as trusted.
