# `(show-line (some 1))` prints a pointer: an `Option` argument dispatches to `Show[int]`

**Severity: low-medium (silently wrong output).** Found 2026-10-19 while
checking where `stdlib/forms/core.tur` should send a reader whose value
`println` cannot print.

There is no `Show` instance for `Option` (or `Result`), so a constrained call
on one should be TUR-E0001 -- as it is for a struct with no instance (`no
'Show' instance for 'P' in constrained call to 'show-line'`). Instead the
compiled program binds the class variable to the carrier `int` and prints the
box's address; the interpreter fails at run time with an unrelated message.

## Repro

```turmeric
(load "stdlib/typeclass-show.tur")
(show-line (some 1))
```

- `tur run`: prints `140733233789344` (an address). The emitted
  `show_hyline(int64_t x)` calls `__inst_Show_show_int(x)`.
- `tur --interpret`: `tur: eval: unknown infix builtin '>'`.

## Root cause (not traced)

The instance lookup for the constrained call resolves `(Option int)` to its
int64 carrier before matching, so `Show[int]` (`stdlib/typeclass-show.tur:36`)
matches. A `defstruct` type keeps its identity and is refused correctly.

## Fix directions

Match instances on the declared type, not the carrier, so a missing
`Show[(Option A)]` is TUR-E0001; then decide whether to add `Show` instances
for `Option` / `Result` (Show[Vec], Show[Map] already exist).

## Resolution (2026-10-20)

Fix direction 1, at the place the "root cause (not traced)" paragraph guessed
wrong. The instance lookup was not resolving `(Option int)` to its carrier:
`typeclass_env_lookup_instance` already matches an ADT-headed applied type by
its constructor (CONV-S1). The call never reached it.

- **Root cause.** The call-site discharge for a constrained defn
  (`elab_call.c`, "class-defn-constraint-not-discharged-at-call-site") skipped
  every applied type, `TY_APP`, on the ground that a parametric instance head
  needs the full PTC3/PTC4 machinery. With no error raised, the
  specialization `show_line__spec__void_tur_adt_Option__int` was emitted, and
  re-resolved the body's `(show x)` against the representative instance the
  elaborator had baked in, `Show[int]`, passing it the aggregate's address:
  `__inst_Show_show_int((int64_t)(intptr_t)(&__t474))`. A `defstruct` is not
  `TY_APP`, so it was already refused.
- **Fix.** The discharge now also runs for an applied type whose head is a
  lowered ADT (`(Option int)`, `(Result T E)`), the case the lookup can
  discriminate. `(show-line (some 1))` is TUR-E0001 on both engines:
  `no 'Show' instance for '(Option int)' in constrained call to 'show-line'`.
  An applied type with a struct head (`(Vec int)`) is still left to the
  existing machinery.
- **Pinned** by `errors/show-line-of-option-no-instance` and
  `errors/show-line-of-result-no-instance`, and by
  `constrained-call-adt-instance-ok` as the negative control: a user enum with
  a `Show` instance goes through `show-line` as before, and `Eq[Option]` still
  answers `eq?` on `(Option int)`.

Fix direction 2 -- whether `Option` and `Result` should have `Show` instances
-- is a library decision and not taken here. With them, `(show-line (some 1))`
would print instead of being refused; without them it is a located error.
