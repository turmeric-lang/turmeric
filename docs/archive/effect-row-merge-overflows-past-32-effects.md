# Effect-row merge overflows a 32-entry stack array

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by `effect-row-forty-effects`.

**Severity:** medium (compiler memory safety). One function that performs more
than 32 distinct effects writes past a stack array inside the compiler. Debug
builds abort under ASan; Release builds corrupt the stack.

## Repro

```python
# gen.py -- writes many.tur
n = 34
for i in range(n): print(f"(defeffect E{i} [] : int)")
body = " ".join(f"(perform (E{i}))" for i in range(n))
print(f"(defn many [] : int (+ {body}))")
print("(defn main [] : int 0)")
```

```
$ tur check many.tur
ERROR: AddressSanitizer: stack-buffer-overflow ...
    #0 ... in effect_row_merge src/passes/effect.c
```

## Root cause

`src/passes/effect.c:effect_row_merge` collects the union into
`Effect *all_effects[32]` ("Max effects in a row for now") without a bound
check. `effect_row_remove` uses another `Effect *keep[32]`. A row's own
`n_effects` is not limited to 32 anywhere upstream.

## Fix directions

- Size both arrays from the inputs' `n_effects` (heap or VLA), or check the
  bound and raise a located error.
- Add a fixture with 40 effects in one row.

## Resolution (2026-10-17)

`effect_row_merge` and `effect_row_remove` (`src/passes/effect.c`) size their
buffers from their inputs (a 32-entry stack buffer while that suffices, the
arena past it). The row counts were 8-bit as well -- `n_effects`,
`n_sym_names`, `effect_row_concrete`'s parameter and the row parsers' locals
-- so a 256th effect would have wrapped; they are 32-bit now, with every
loop over them.

The fixture performs 40 effects from one function and discards each result.
The repro's shape -- summing them -- now type-checks, but running it needs a
continuation that captures more than 16 values, which is a separate limit:
[cps-continuation-past-16-captures-loses-its-env](cps-continuation-past-16-captures-loses-its-env.md) (since fixed).
