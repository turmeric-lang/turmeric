# A `set!` whose value performs evicts its function, not only the conditional in-loop case the docs named

**RESOLVED 2026-11-05.** A `set!` whose value has control lowers as the `let`
rewrite (fix direction 1): `set_value_hoisted` in `src/passes/cps_ir.c` binds
the value to a fresh binder through the CPS path and delegates the assignment
of that atom. That exposed a wrong value the hand-written rewrite already
had: in `(if c (let [v (perform (E))] (set! s (* s v))) nil) s` the lifted
join read `s` from an env snapshotted at the split (5 where the interpreter
prints 15). `byref_scan` now promotes every enclosing `^mut` a suspending
branch of a join assigns to the shared cell (`term_may_split`, reusing
`loop_cell_scan`'s rule). The guide bullet and the eviction text no longer
list the shape. Pinned by `tests/fixtures/cps-set-value-performs`
(straight-line, bare, `if` and `match` arms, a conditional and a doubled
in-loop update; every line equals `tur --interpret` and `tur jit`). The shared
cell is shared across `^multishot` resumes too, which the loop-cell promotion
already did: [cps-cell-promoted-mut-shared-across-multishot-resumes](../reported/cps-cell-promoted-mut-shared-across-multishot-resumes.md).

**Severity:** low (compile-time limitation with a located error and an easy
rewrite). Until 2026-10-25 both the effects guide's "Performs inside loops and
conditionals" list and the eviction error itself said that only a
*conditional* assignment whose value performs is unsupported. In fact, any
`set!` whose value performs is refused unless it is an unconditional
assignment to a loop's carried variable. That includes straight-line code
with no loop at all.

Filed 2026-10-25, found while writing `^multishot` examples for the effects
guide. The guide bullet and the error text now describe the real limit; the
limit itself is still open.

## Repro

```turmeric
(defeffect Choose [] : int)

(defn f [] : int
  (let [^mut s 1]
    (set! s (+ s (perform (Choose))))   ; also: (set! s (perform (Choose)))
    s))

(defn main [] : int
  (println (handle (f) (Choose [] k) (resume k 10)))
  0)
```

```
error: this effect operation has no lowering here: the enclosing function left
the CPS backend's supported subset ...
```

`TUR_TRACE_EVICT=1` reports `BODY-UNSUPPORTED eff=1 f unsupported form:
EX_SET (mutation)`, and the eviction spreads to every caller that installs
the handler (`BODY-TAINT`). `tur --interpret` runs it and prints `11`.

This works:

```turmeric
(let [^mut s 1]
  (let [v (perform (Choose))] (set! s (+ s v)))
  s)
```

So does `(set! s (+ s (perform (Choose))))` as the unconditional update of a
`while` loop's carried variable.

## Root cause

`cps_tail`'s `EX_SET` arm (`src/passes/cps_ir.c:5808`) handles only two
cases. One is a loop-carried target (`loop_var_index`), whose value is bound
with `cps_bind`. The other is a `set!` with no control in its value
(`safe_to_delegate`, delegated raw). Any other `set!` reaches
`unsupported_form` (`src/passes/cps_ir.c:5829`).

## Fix directions

1. Lower a non-loop `set!` the way the loop-carried one is lowered: `cps_bind`
   the value to a fresh `CVar`, then emit the assignment of that atom (a
   `CT_LETPRIM`/raw store, or a cell store when the target was promoted by
   B7b). This is the `let` rewrite above, done by the compiler.
2. If the in-loop conditional case stays unsupported, keep it in the guide
   and the error text.
