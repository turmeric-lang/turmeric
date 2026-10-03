# A struct temporary with a fn field leaks its fn-field box

**Narrowed 2026-10-03 (same day): the direct emitter's half is fixed; a
CPS-lowered function still leaks, and so does a `let`-bound struct there --
see "What is left".**

**Severity: low (leak; 24 bytes per evaluation).**  Filed 2026-10-03, found
working [cps-evicts-handle-in-operand-positions](cps-evicts-handle-in-operand-positions.md)
item 4.  Pre-existing; the direct emitter and the CPS backend both show it.

## Repro

```turmeric
(defstruct PE :copy [run : (fn [int] int)])
(defn inc [v : int] : int (+ 1 v))
(defn main [] : int
  (println (.run (make-struct PE inc) 1))          ; leaks 24 bytes
  (let [p (make-struct PE inc)] (println (.run p 2)))  ; freed
  0)
```

Built the way `tests/run-leak-check.sh` builds: `24 byte(s) leaked in 1
allocation(s)` -- the `malloc(sizeof(void *) + 2 * sizeof(int64_t))` fat box
the constructor wraps `inc` in for the fn field.

## Root cause

The fn-field box is released only by `drop_fnfields_<T>`, which
`emit_let_value` runs for a LET-BOUND by-value struct the elaborator flagged
`drops_fn_fields` (local-struct-drop).  A struct that is never bound -- a
constructor used directly as a field-access receiver or an argument -- has no
binding to flag, so nothing drops its fields.

## Fix directions

- Treat a by-value struct temporary with fn fields like the other pending
  temporaries the emitter already drains after the consuming call (the
  `any` / sum-box pending-drop queues in `emit_expr.c`): push
  `drop_fnfields_<T>(&tmp)` when the constructor is spilled to a temp, drain
  after the field read or call that consumes it.
- A fixture with `requires.leak-check` belongs with the fix.

## Fixed (2026-10-03): an unbound receiver gets a binding

The capability-field call path (`elab_typeclasses.c`, `(.field obj args)` on
a record ADT) binds an owning receiver -- a constructor or a call result whose
type owns a boxed fn field (`elab_type_owns_boxed_fnfield`) -- in a
synthesized `let` flagged `drops_fn_fields`: `(let [t <recv>] (.run t x))` is
the same program, and the `let` is what local-struct-drop releases.  Pinned
by `tests/fixtures/struct-temporary-fn-field-released` (leak-checked: a
constructor receiver, a call-result receiver, and the `let`-bound control).

## What is left: CPS-lowered functions

A function that calls through a field is usually CPS-lowered -- the indirect
call colors it -- and the CPS backend emits a `let` with no
`drops_fn_fields` release at all, bound or synthesized (`emit_cps_ir.c` never
reads the flag).  So `(defn twice [n : int] : int (.run (make-struct PE inc
n) n))` still leaks its box from `twice__cps`, as does the same body with the
struct `let`-bound.  The release cannot simply be emitted at the end of the
`let` there: when the field call is the tail, the box is the callee, alive
until the call returns, and nothing runs after a CPS tail call.  A fix needs
the release threaded as a continuation frame (or the drop deferred to the
entry boundary's reap list, the way DK nodes are reaped).
