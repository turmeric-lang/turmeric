# Top-level statements are silently dropped when the file defines `main`

**RESOLVED 2026-10-17** -- see [Resolution](#resolution-2026-10-17). Pinned by
`toplevel-statements-run-before-user-main` and `toplevel-main-call-dropped`.

**Severity:** low-medium. The compiled program skips code the interpreter
runs, with no diagnostic.

## Repro

```turmeric
(println 5)

(defn main [] : int
  (println 7)
  0)
```

- `tur --interpret`: `5`, `7`.
- `tur run`: `7` only.

## Root cause

The main fold (`src/compiler/elab_toplevel.c:elaborate_program`) turns trailing
top-level statements into a synthesized `main` only when the file has no user
`main`. When it does, the emitter writes the user's `main`, and the top-level
statements are not emitted anywhere: not as static initializers, and not
before `main`'s body. The archive mentions the behavior as long-standing, but
no report tracked it.

## Fix directions

Pick one and make both engines agree:

- error (or at least warn) on top-level statements in a file that defines
  `main`; or
- run them before the user's `main`, in source order, as the interpreter
  does.

## Resolution (2026-10-17)

The compiled program now runs them, before the user's `main`, in source order
-- what the interpreter already did. Both emitters (`src/compiler/emit_module.c`,
`emit_toplevel_stmts_for_user_main`) put the top-level statements in a
`__tur_toplevel_stmts` function and call it from C `main` at the point the
`TUR_TOPLEVEL_STMTS_SENTINEL` marks: after `*args*` is bound and before the
user's body, on the direct path (`emit_fns.c`) and in the CPS backend's D2b
`main` wrapper (`emit_cps_ir.c`). A file with no top-level statements emits
nothing new.

Running them changed the meaning of a script-style trailing `(main)`: it would
now run `main` twice compiled, as it already did interpreted, and the compiled
call reached C's own `int main(int, char **)` with no arguments. `main` is the
entry point, so when the same form list defines it, a bare `(main)` form is
dropped in both engines with the new warning TUR-W0625 (`tur explain`
TUR-W0625). A REPL turn that calls `main` still runs it. Three fixtures that
ended in `(main)` lost the call.

Pinned by `toplevel-statements-run-before-user-main` and
`toplevel-main-call-dropped`.
