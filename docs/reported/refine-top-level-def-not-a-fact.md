# A top-level `def`'s value is not a refinement fact

**Severity:** low (completeness). The check is kept, so programs are correct.
But constant tables are the natural home for compile-time-checked data (a
literal program for a stack machine, a lookup table), and a `^reflect`
measure cannot see through one.

## Repro

```turmeric
(def n 5)
(defn needs-pos [x : #refine{ v : int | (> v 0) }] : int x)
(defn main [] : int (println (needs-pos n)) 0)
```

`(> n 0)` is unknown; the VC is just `(declare-const n Int) (assert (not (< 0
n)))`. The same holds for a reflected measure over a `def`-bound constructor
term:

```turmeric
(def prog (Push 1 (Push 2 (Add (Halt)))))
(run prog)   ; `run` requires (safe? v 0) -- unknown
```

The same term inline, or bound by a `let`, proves (`reflect-stack-machine`,
`reflect-let-bound`).

## Root cause

Call-site path facts come from the enclosing function body
(`rt_collect_path_conds` in `src/compiler/elab_fns.c`). A global is not on that
path, and nothing else asserts `n = 5`. That is correct for a global something
can `set!`; nothing checks that this one never is.

## Fix directions

- Assert `g = <init>` for a global whose initializer is a literal or a
  constructor term, when no `set!` of `g` exists anywhere in the program. The
  write-set walk `rt_body_writes` is per-body; this needs a whole-unit
  "assigned globals" set.
- The let-bound machinery then applies unchanged: `rf_canon` selects
  through the equation and `model_ctor_defns` refutes through it.
