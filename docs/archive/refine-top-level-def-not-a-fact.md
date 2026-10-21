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

## Resolution (2026-10-21)

Direction 1, without the whole-unit write set: a non-`^mut` global cannot be
`set!` at all (`set!: 'n' is immutable`), so immutability is the guarantee,
and the only question left is whether the name at the call IS the global.

- `elab_def` stamps `Binding.refine_def_init` with the initializer of an
  immutable, non-atomic, non-thread-local, non-deferred `def` whose value is
  an int literal or a constructor applied to int / bool literals and
  constructor terms (`rt_def_init_is_fact`). A computed value stays a free
  variable: an equation the encoder abstracts costs refutations, as the let
  path notes. A bare `bool` global does too, because a crossing reads a named
  bool argument with the Int sort -- a `let`-bound bool hits the same wall
  ("predicate does not denote a proposition"), so the equation could only be
  ill-sorted.
- `refine_note_call_site` records the immutable globals the ARGUMENTS name
  (`RefineCallSite.global_args`, `rt_collect_global_args`), looked up in the
  scope of the call while it is being elaborated -- so a parameter or local
  that shadows the name resolves to itself and contributes nothing.
- `refine_resolve_call_sites` pushes `(= g <init>)` for each with a stamped
  initializer, after the path facts and under the same rewind. The let-bound
  machinery then applies unchanged: the report's `(def prog (Push 1 (Push 2
  (Add (Halt)))))` proves `(run prog)` and `(def bad (Add (Halt)))` is refuted
  with "the predicate (safe? v 0) is false for the value given here".

Not covered: a global named only inside the callee's PREDICATE (a refinement
mentioning a `def`), and a `def` the call precedes -- `def`s are not
forward-declared, so that call is an unbound symbol anyway.

Pinned by `refine-top-level-def-fact` (a literal, arithmetic over two defs,
and a reflected measure over a def-bound constructor term, under
`--strict-refine`), `errors/refine-top-level-def-shadowed` (a shadowing
parameter gets no fact) and `errors/refine-top-level-def-mut` (a `^mut`
global gets none).
