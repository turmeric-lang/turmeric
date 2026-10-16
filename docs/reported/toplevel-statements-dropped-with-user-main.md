# Top-level statements are silently dropped when the file defines `main`

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
