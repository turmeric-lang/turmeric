# In a `defmodule`, an effectful function value that works at top level has no lowering

**Severity:** medium (a correct program is refused; the same code outside
`defmodule` builds and runs). Found 2026-11-24 executing
[sepcomp-cross-module-effect-unhandled](../archive/sepcomp-cross-module-effect-unhandled.md).
It is a whole-program defect (`tur run`, `tur build <main.tur>`): one file is
enough.

## Repro

```turmeric
(defmodule m3
  (defeffect Ask [] : int)
  (defn ask [] : int (perform (Ask)))
  (defn app0 [h : (fn [] int)] : int (h))
  (defn main [] : int
    (println (handle (app0 (fn [] : int (* 2 (ask))))
               (Ask [] k) (resume k 10)))
    0))
```

```
$ tur run m3.tur
m3.tur:3:22: error: this effect operation has no lowering here: ...
m3.tur:6:41: note: `Ask` escapes this function value: it is returned, stored,
or passed where a call through it does not carry the caller's handler ...
```

Expected `20`. Remove the `(defmodule m3` line and its closing paren and it
prints `20`: that is the second line of
`tests/fixtures/cps-effectful-callback-multi-arg`.

A second shape, the same cause:

```turmeric
(defmodule m2
  (defeffect Ask [] : int)
  (defn ask-plus [n : int] : int
    (+ n (perform (Ask))))
  (defn main [] : int
    (println (handle (let [f (fn [x : int] : int (ask-plus x))]
                       (+ (f 1) (f 2)))
               (Ask [] k) (resume k 5)))
    0))
```

Refused the same way; `13` at top level.

## Root cause

`TUR_TRACE_EVICT=1` prints the threadability line for the lambda:

```
[E2-COLOR] __fn_6    thr=N tier=-    uses=0 ok=0 lambda     (in the defmodule)
[E2-COLOR] __fn_6    thr=Y tier=now  uses=1 ok=1 lambda     (at top level)
```

`uses=0`: the tally never saw the lambda passed to `app0`.
`fv_multi_tally` (`src/compiler/emit_cps_ir.c`) walks
`program->as.program.items`, where a module is one `EX_DEFMODULE` item, and
the effect walk it runs on each item does not descend into a module's member
bodies. `ensure_S` flattens the program for its own loops
(`flatten_program_items`, whose comment describes this same mistake for the
classification loop) but hands `fv_multi_tally` the unflattened program. With
no counted use the lambda is not threadable, it is an effectful fn value with
no threaded call, and its effect is tainted.

## What the separate build does

The `m2` shape, with `ask-plus` in another module, **builds and prints `13`**
under separate compilation (`tur build <dir>`) since 2026-11-24. So the two
build shapes disagree, and the whole-program one is the wrong one.

The `m3` shape, with `app0` in another module, is refused by both. Under
separate compilation that is expected: the E2 twin registry is per unit (see
the
[separate compilation guide](../guides/separate-compilation-internals-guide.md#cps-functions-across-tus)).

## Fix directions

- Walk the flattened items in `fv_multi_tally`, and audit the other walks in
  `ensure_S` that take `program` rather than `items` (`thin_alias_collect`,
  `fn_unreached`'s `reach_refs_visit`, `nesc_fixpoint`).
- Add both repros as fixtures; expect `module-*` snapshots to move where a
  module member's lambda becomes threadable.
