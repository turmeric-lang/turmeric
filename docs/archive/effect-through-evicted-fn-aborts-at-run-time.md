# In a `defmodule`, a handler reaching its performer through an evicted function builds and aborts at run time (resolved)

**Resolved 2026-11-27.** The repro is now the build error it is at top level
("this effect operation has no lowering here" on the `perform`, "this
`handle` has no lowering here" on the handler), in one file and with the
performer in an imported module.

The suspect below was wrong: the call graph was complete, and every callee
`Binding` matched its entry by pointer. What was missing was the effects.
`ensure_S` asks `fn_unreached` whether a function can run at all, and zeroes
the performed and handled sets of one that cannot. `fn_unreached` collected
the program's calls by walking `program->as.program.items`, where a module is
one `EX_DEFMODULE` node that `cps_visit_children` does not enter. So no call
made inside a module was recorded, and every member a module did not export
read as never called. In the repro that zeroed `ask-plus`'s `Ask` and
`run-c`'s handler: Rule C needs a handler and a performer of the effect to
build a path, found neither, and tainted nothing. In the imported shape the
exported `ask-plus` and `nested` kept their effects and the unexported
`run-c` lost its handler, with the same result.

`fn_unreached` now reads the flattened items `ensure_S` already holds, as do
the three fn-value walks that had the same blind spot (see
[module-effectful-fn-value-has-no-lowering](module-effectful-fn-value-has-no-lowering.md),
resolved by the same change). `src/compiler/emit_cps_ir.c`.

Tests: `tests/fixtures/errors/effect-through-evicted-fn-in-module` and
`tests/fixtures/errors/effect-through-evicted-fn-imported-module`.

The report as filed follows.

---

**Severity:** medium-high (a program that compiles aborts at run time with no
diagnostic; the same code outside `defmodule` is a build error). Found
2026-11-24 executing
[sepcomp-cross-module-effect-unhandled](sepcomp-cross-module-effect-unhandled.md).
It is a whole-program defect (`tur run`, `tur build <main.tur>`): one file is
enough.

## Repro

```turmeric
(defmodule one
  (defeffect Ask [] : int)
  (defn ask-plus [n : int] : int
    (+ n (perform (Ask))))
  ;; A performing call inside nested loops has no CPS lowering, so `nested`
  ;; is evicted (BODY-UNSUPPORTED) and calls `ask-plus` through its plain
  ;; entry: a fresh root, with no handler on it.
  (defn nested [n : int] : int
    (let [^mut i   0
          ^mut acc 0]
      (while (< i n)
        (let [^mut j 0]
          (while (< j n)
            (set! acc (+ acc (ask-plus 1)))
            (set! j (+ j 1))))
        (set! i (+ i 1)))
      acc))
  (defn run-c [] : int
    (handle (nested 2)
      (Ask [] k) (resume k 10)))
  (defn main [] : int
    (println (run-c))
    0))
```

```
$ tur run one.tur
tur: unhandled effect: Ask (tag 2)
```

Remove the `(defmodule one` line and its closing paren and the build fails
where it should:

```
top.tur:4:10: error: this effect operation has no lowering here: ...
```

It also reproduces with `ask-plus` and `nested` in an imported module, with
`:as` or `:refer`.

## Root cause (as filed -- the suspect was not the cause)

Rule C of `ensure_S`'s taint fixpoint (`src/compiler/emit_cps_ir.c`) exists
for this: taint an effect whose handler-to-performer path runs through a
function outside `S`, so the handler and the performer fall back together and
the `perform` gets its located error. `TUR_TRACE_EVICT=1` shows it does not
fire for module members: `nested` is evicted, and `run-c` and `ask-plus` stay
in `S`. At top level the same trace shows both `BODY-TAINT`.

Not narrowed further. The suspect is the call graph the rule walks. `edges`
are built by matching each callee `Binding` against the entries by pointer
(`g_ents[j].bind == cbuf[k]`). A module's members get a forward-declaration
pass, so a call's `fn_binding` and the member's own `FnDef.binding` may be two
objects. The coloring pass met the same thing and resolves by C symbol as a
fallback (`cps_find_node`); this loop has no fallback. With no edge from
`run-c` to `nested`, there is no path to taint.

## What the separate build does

When `nested` is in another module of a separately compiled build, the call
is refused at build time ("Its module exports no threaded entry for it"). That
check is keyed on the callee being in another unit, so it does not cover a
whole-program build.

## Fix directions

- Give the `edges` loop the binding-or-C-symbol lookup the coloring pass uses,
  and check the other pointer compares in the classification
  (`ent_of_binding`, `binding_in_s`).
- Add the repro as a negative fixture under `tests/fixtures/errors/`.
