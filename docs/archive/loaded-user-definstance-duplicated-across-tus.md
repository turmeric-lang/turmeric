# A `definstance` in a file `load`-ed by two modules is defined twice

**Severity:** medium (link failure). This is the leftover of the archived
definstance cross-module report. It was noted there but never filed.

## Repro

```turmeric
;; shared/desc.tur
(defclass Describe [a]
  (describe [x : a] : int))
(definstance Describe [int]
  (describe [x] (* x 10)))
```

```turmeric
;; ma.tur                              ;; mb.tur
(load "../shared/desc.tur")            (load "../shared/desc.tur")
(defmodule ma                          (defmodule mb
  (export fa)                            (export fb)
  (defn fa [] : int (describe 1)))       (defn fb [] : int (describe 2)))
```

plus an `app.tur` that imports both. Separately compiled (`tur build .` with
no `build.tur`):

```
multiple definition of `__inst_Describe_describe_int'
```

## Root cause

`src/compiler/emit_module.c:emit_inst_method_wants_external` decides whether
a unit references an instance method or defines it. It only recognizes
stdlib paths as "defined elsewhere". A user file loaded into two modules is
spliced into both, and both units define the method.

## Fix directions

- Treat an instance as owned only by the unit whose own file defines it
  (`origin_file_id == 0`); every other unit declares it `extern`. One unit
  must still be chosen to own instances from a file no module owns, e.g. the
  first module (in build order) that loads it.

## Resolution (2026-10-21)

Not the fix direction above: every loading unit still defines the methods,
and the copies are WEAK.

- **Why not "one owner, the rest extern".** Choosing the owner needs every
  unit's load set before any unit is emitted; the separate-compilation driver
  compiles modules one at a time, and the only cross-unit channel is the
  pass-2 forced-spec list. Weak definitions need no coordination.
- **Why not `static`.** It would link the two-loader repro, and break a shape
  that works today: an importer of a module that loaded the file dispatches on
  the instance through that module's header (`app` imports `ma`, which loads
  `desc.tur`, and calls `(describe 5)`). The method has to stay
  linker-visible.
- **Fix.** `emit_inst_method_linkage_attr` (`src/compiler/emit_module.c`)
  returns `__attribute__((weak)) ` for an instance method that
  `emit_inst_method_wants_external` gives external linkage and whose instance
  did not come from the unit's own file. A separately compiled module's source
  is registered as file 0 of its unit (`src/main.c`), so `origin_file_id != 0`
  is a loaded copy. The attribute is written at the three places that must
  agree on linkage: the definition (`emit_fns.c`), the forward declaration
  (`emit_fn_forward_decls`) and the CPS entry wrapper (`emit_cps_ir.c`). The
  header keeps its plain prototype. An instance in a module's own file keeps
  a strong definition, so two modules that each define the same instance in
  their own source still fail to link, as they should.
- **Precedent.** The interned symbol records of separately compiled modules
  are weak for the same reason (SYM2, `sym_codegen_emit`). The PE/COFF
  problem recorded there is a reference at an offset into weak DATA; a
  function is only ever referenced by its address.
- **Pinned** by `build-project-loaded-definstance-two-modules` (the repro:
  links, prints 10 and 20) and `build-project-loaded-definstance-reached-by-importer`
  (one loader, an importer calling the instance directly: 10 and 50) in
  `tests/run-build-project.sh`. `tests/fixtures/spices/cross-module-instance`
  still checks that a module's own instance is declared in its header and
  defined non-static.
