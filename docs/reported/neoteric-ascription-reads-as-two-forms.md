# Neoteric `::(x T)` reads as `::` and a separate list

**Severity: low (wrong parse with an error or a silent wrong form; easy
workaround).** In a neoteric or sweet-exp file, `+(x 1)` reads as `(+ x 1)`,
but `::(x :int)` reads as two forms: the symbol `::`, then the list
`(x :int)`. In a function body that is TUR-E0003 "unbound symbol '::'"; in a
guide's sweet-exp twin it is a pair that does not parse-check. Found
2026-10-17 porting `tools/check-guide-pairs` to Turmeric, whose parse of the
guides reaches a pair inside a list item that the Python checker never saw:
`data-literals-guide.md` wrote `::(map-get(m "name") :int)`, and its
`no-check` pair above it wrote `::(...)` twice more.

## Repro

```sh
printf '(:: x :int)\n' > a.tur
printf '#lang sweet-exp\n::(x :int)\n' > a.sweet
tur parse-check a.tur a.sweet     # mismatch: "::" then "(x :int)"

printf '#lang sweet-exp\ndefn f [x : int] : int\n  ::(+(x 1) :int)\n' > f.tur.sweet
tur emit-c f.tur.sweet            # TUR-E0003: unbound symbol '::'
```

The workaround is the t-expression form, `:: map-get(m "name") :int`, or the
s-expression `(:: (map-get m "name") :int)`.

## Root cause

`read_keyword` (`src/compiler/reader.c:863`) reads `::` and returns it as a
symbol at once. Every other atom reader -- symbols, numbers, strings --
checks `peek_neoteric_bracket` before returning and, when a bracket follows,
hands the atom to `read_neoteric_bracket`. The `::` branch skips that check.

## Fix directions

In the `::` branch, before `return form_sym(...)`, do what the other readers
do: when `r->neoteric_enabled` and `peek_neoteric_bracket(r)` finds a bracket,
return `read_neoteric_bracket(r, sym_form, bracket)`. Add a parse-check case
to `tests/run-parse-check.sh` (`::(x :int)` against `(:: x :int)`). Nothing in
the guides spells `::(` any more, so no guide changes with it.
