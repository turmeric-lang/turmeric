# A `^mut` the CPS backend keeps in a shared cell is shared by every `^multishot` run

**Narrowed 2026-11-06: both repro shapes agree now (`12022`, `16026` on all
three engines).** Fix direction 2, at the DK runtime. A frame whose env holds
a one-word by-reference cell (an `int64_t`, a `double`, a pointer) records
which env words those are (`dk_env_cells`, a `cell_mask` beside `env_size`;
the `<name>_env_CELLS` word mask the emitter writes after the env typedef,
`src/compiler/emit_cps_ir.c`). The copy a resume runs -- `dk_resume_into`'s
and `dk_invoke`'s -- then gets cells of its own (`__dk_cells_fresh`,
`src/compiler/emit_dk_runtime.c`): each cell the copy names is replaced, once
per copy, by a fresh cell holding the value the `perform` left, so frames that
shared a cell still share it within a run. The env is duplicated first
(`env_owned`), and the new cells are reaped like the originals.

Only where every frame naming the cell is in the copy: the emitter masks
cells of a function with no `handle` / reset / call/cc of its own and no
closure literal (`fn_cells_fresh_ok`). A `^mut` bound in the function that
installs the handler stays shared by every run, as it should -- the clause and
the code after the `handle` read it. Pinned by
`tests/fixtures/cps-multishot-cell-per-run` (leak-checked; `lp`, `br`, a loop
with two performs and four runs, the cell's function as a callee, a one-shot
handler, and `outer`, whose shared `^mut` must stay shared; every line equals
`tur --interpret` and `tur jit`).

**Still open:** a cell that is not one word (a `bool` or narrow-int `^mut`, a
by-value struct), and a cell in a function that also builds a closure literal
(the closure's env is not a DK env and cannot be re-celled), keep sharing one
cell across runs.

**Severity:** low (engine disagreement, `^multishot` only; a silent wrong
value in the shapes below). One-shot handlers are unaffected.

Filed 2026-11-05 while resolving
[cps-set-whose-value-performs-always-evicts](../archive/cps-set-whose-value-performs-always-evicts.md)
and
[turi-multishot-tail-let-frame-shared-across-resumes](../archive/turi-multishot-tail-let-frame-shared-across-resumes.md).
Until the second of those, the interpreter shared the same locals for a
different reason, so both engines printed the same wrong number and nothing
noticed.

## The rule it breaks

The effects guide's "What each run sees": every resume of a `^multishot` `k`
runs its own copy of the rest of the handled body, and a local bound *inside*
the body starts from its value at the `perform` in every run.

## Repro

```turmeric
(defeffect Choose [] : int)

(defn lp [] : int                       ; loop cell (on main before 2026-11-05)
  (let [^mut i 0
        ^mut x 1]
    (while (< i 1)
      (when (= i 0) (set! x (+ x 1)))   ; conditional: x is cell-carried
      (let [v (perform (Choose))] (set! x (+ x v)))
      (set! i (+ i 1)))
    x))

(defn br [c : bool] : int               ; join cell (since 2026-11-05)
  (let [^mut s 5]
    (if c
      (do (set! s (+ s 1))
          (let [v (perform (Choose))] (set! s (+ s v))))
      nil)
    s))

(defn main [] : int
  (println (handle (lp) (Choose [] ^multishot k) (+ (* 1000 (resume k 10)) (resume k 20))))
  (println (handle (br true) (Choose [] ^multishot k) (+ (* 1000 (resume k 10)) (resume k 20))))
  0)
```

| Engine | `lp` | `br` |
| --- | --- | --- |
| `tur --interpret` (and the guide) | `12022` | `16026` |
| compiled / `tur jit` | `12032` | `16036` |

The second run starts from the first run's `x = 12` / `s = 16` instead of the
`2` / `6` the `perform` saw.

## Root cause

`src/compiler/emit_cps_ir.c` promotes a `^mut` to a shared heap cell
(`g_byref_muts`) where a by-value copy would lose a write: a loop's
cell-carried variable (`loop_cell_scan`), one a handled body or a clause
touches (`case_mut_scan`), and since 2026-11-05 one a suspending branch of a
join assigns (`term_may_split` in `byref_scan`). The cell is allocated once,
where the binding is introduced. A `^multishot` resume clones the captured
frames, not the cell, so every run reads and writes the same storage.

Before 2026-11-05 the `br` shape did not compile at all (the function was
evicted), and the hand-written `let` form of it compiled to a wrong one-shot
value. The join promotion made the one-shot answer right and moved this
shape into the `lp` class.

## Fix directions

- Thread the variable instead of sharing it: a join that a suspending branch
  reaches takes the assigned mutables as extra parameters (each branch
  delivers its own value), and a loop helper carries a conditionally assigned
  variable as a parameter whose value each path passes on. A per-run copy then
  falls out of the frame copy, as it does for straight-line code.
- Or clone the cells a captured slice reaches when a `^multishot` resume
  copies it, as `clone_ws_slice` does for the interpreter's frames.
