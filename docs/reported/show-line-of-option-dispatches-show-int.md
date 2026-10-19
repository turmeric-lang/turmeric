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
