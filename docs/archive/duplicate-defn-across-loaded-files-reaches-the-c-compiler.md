# A `defn` defined again in another `load`ed file reaches the C compiler

**Severity:** low (a C compile error naming the clash, instead of a
diagnostic).

Two files pulled in with `load` that each define the same function name
compile to C with two definitions of it. cc then fails with "conflicting
types". Within one file the same mistake is a diagnostic. Found 2026-10-16,
when a new markdown helper reused the name of one in `stdlib/markdown/scan.tur`.

## Repro

```turmeric
;; a.tur
(defn helper [x : int y : int] : int (+ x y))
(defn use-a [] : int (helper 1 2))

;; b.tur
(defn helper [x : int] : int (* x 10))
(defn use-b [] : int (helper 4))

;; main.tur
(load "a.tur")
(load "b.tur")
(defn main [] : int (do (println (use-a)) (println (use-b)) 0))
```

`tur run main.tur`:

```
/tmp/tur-build/main_tur.c:3114:16: error: conflicting types for 'helper'; have 'int64_t(int64_t)'
/tmp/tur-build/main_tur.c:3112:16: note: previous declaration of 'helper' with type 'int64_t(int64_t,  int64_t)'
```

With both `defn`s in one file, the elaborator reports
`defn: 'helper' is already defined` and points at the first.

## Root cause

`elab_fns.c`, the redefinition check in the `defn` elaborator (around line
8855, the `duplicate-defn-in-one-file-reaches-the-c-compiler` comment). Pass 1
declares each name once, so a second `defn` finds the binding the first one
filled. It is told apart from a forward declaration by `defn_claim`, but only
when the claim is in the same file (`claim->file_id == call->span.file_id`).
That scope is deliberate: a name from an import, an `extern-c` or a native
stub has no claim. A second `load`ed file is none of those, though, so its
`defn` is taken for the real definition of a forward declaration.

## Fix directions

1. Widen the check from "the same file" to "any file this compilation
   `load`ed", keeping imports, `extern-c` and native stubs out as now. The
   claim already records the span, so the note can say which file defined
   the name first.
2. Keep the stdlib's own pass-1 forward declarations working: the
   `in_stdlib_load` exemption stays, but a stdlib file `load`ed by the user
   (`(load "stdlib/markdown.tur")`) should still be checked against itself.

## Resolution (2026-10-18)

Fix direction 1. The redefinition check in `elab_defn` (`src/compiler/elab_fns.c`)
no longer requires the claim to be in the same file. It requires the claim
to be in the same module namespace instead (`existing->defining_module_name ==
e->current_module_name`): the top level of the program, entry and loaded
files together, is one namespace, and so is one module's body. "The same
form", which a speculative defn re-elaborated must still pass, is now its
whole span, file id included. A defn of a name that another module exports
keeps the old behaviour, as do extern-c and native stubs (no claim) and the
auto-loaded stdlib (`in_stdlib_load`). A stdlib file the user loads is checked
like any other loaded file, which is the second direction.

One cross-file shape is still allowed as a replacement: a stdlib file
defining a name that was first claimed outside the stdlib
(`stdlib_replaces_stub`). The interpreter's preload stubs
(`src/turi/preload.c`, for example `(defn none? [r :int] :bool false)`) work
this way, and the REPL's `:type` re-elaborates the whole session source,
stubs first and then the stdlib that replaces them, as one unit. Without the
exemption, `:type 42` failed with "'none?' is already defined"
(`tur_repl_smoke` and the browser meta-commands test caught it on #1286).

The two-file repro is now `defn: 'helper' is already defined` at `b.tur`,
with a note at `a.tur`'s definition. Pinned by
`errors/defn-duplicate-across-loaded-files`.
