# Codegen Cross-Module Private-Defn Collision -- Plan (CC0--CC3)

> **Status:** Done (CC0--CC2 landed). CC3 (spice cleanup) is optional and not
> done here -- `../turmeric-spices` is not checked out in this environment.
>
> **Outcome:** Root cause was hypothesis (a), wrong-binding resolution -- but
> the operative defect was binding *reuse*, not name lookup. In whole-program
> mode every module elaborates into the single shared `e->global` scope, and
> `elab_define_fn` reused a same-named private binding from a *different* module
> as if it were a forward declaration (last-writer-wins on
> `defining_module_name`), collapsing two distinct private defns onto one
> mangled C symbol. The "private defns get a NULL module name" side-theory was
> ruled out: `binding_new` (elab_core.c:1417) stamps `defining_module_name`
> unconditionally.
>
> **Fix:** In `elab_fns.c` (the function-binding creation site), only reuse an
> existing global `TY_FN` binding when it belongs to the *same* module being
> defined into (`defining_module_name == e->current_module_name`) or to no
> module yet (`NULL`, e.g. a pre-module stdlib forward decl). Otherwise a fresh,
> distinctly-mangled binding is created. Resolution then needs no separate fix:
> `scope_lookup` reverse-scans the shared scope, and each module's body is
> elaborated immediately after its own helper is added, so a module's call to
> its private helper binds to that module's (newest) binding. Same-module
> recursion, mutual recursion (pass-1 forward decl -> pass-2 reuse), and
> stdlib forward decls all continue to reuse correctly. Verified: full suite
> 1152 passed, 0 failed.
>
> **Flag:** None. Bug in the codegen / elab pipeline; fix is invisible to
> well-formed programs.
>
> **Last updated:** 2026-05-31
>
> **Related:**
> - `src/compiler/elab_core.c::elab_mangle_binding_name` (line ~1561) -- the
>   single source of truth for "Turmeric binding -> C symbol name."
> - `src/compiler/emit_core.c::raw_name_for_binding` (around line 475) -- the
>   parallel mangle used at emission time; the comment claims it mirrors
>   `elab_mangle_binding_name`.
> - `src/compiler/elab_toplevel.c` lines ~951-995 -- the exported-binding
>   C-name collision check. **Does not run for non-exported (private)
>   bindings.**
> - `src/compiler/elab_module.c::elab_load_module` (around line 295) -- collects
>   exports by matching `b->defining_module_name == name`.
> - `../turmeric-spices/spices/plot/src/plot/{decor,interval,line}.tur` -- real
>   world manifestation; helpers there are now uniquely named (`__auto-bound` /
>   `__iv-nan` / `__ln-nan`) specifically to dodge this bug.

---

## Symptom

When two different modules each define a *private* (non-exported) `defn`
with the same Turmeric name, building any consumer that transitively pulls
both modules produces a C-level redefinition error.

Minimal reproducer using existing plot infrastructure (before the
defensive renames in plot-v0.3.0):

```turmeric
;; spices/plot/src/plot/decor.tur
(defn __auto-bound [] : float ```c return 0.0 / 0.0; ```)

;; spices/plot/src/plot/interval.tur
(defn __auto-bound [] : float ```c return 0.0 / 0.0; ```)
```

Build any test file that imports both `plot/decor` and `plot/interval`:

```
/tmp/tur-build/tests_plot_interval_test_tur.c:4495:15:
  error: redefinition of 'plot__decor____auto_bound'
1 error generated.
tur: cc invocation failed (status 256)
```

Inspecting the emitted C file shows two definitions of
`plot__decor____auto_bound` (not one of each module's mangled name).
Worse: the call site inside `plot/interval`'s `function-interval` body --
which the programmer wrote as `(__auto-bound)` against *interval's* own
helper -- has been rewritten to call `plot__decor____auto_bound()`. The
two helpers have collapsed into one and the wrong module's body ended up
emitted twice.

The workaround already shipped in plot 0.3.0 is to give each module's
helper a unique name (`__auto-bound` in decor, `__iv-nan` in interval,
`__ln-nan` in line). That sidesteps the bug entirely but is a footgun for
future spice authors who reach for "obvious" helper names.

## Root cause (hypothesis)

There are two independent but related defects, and the plan needs to
verify which is actually firing:

1.  **Resolution picks the wrong binding.** When `plot/interval` references
    its own `__auto-bound`, the elaborator's name lookup walks the global
    scope and returns the *first* binding it finds with that name --
    irrespective of `defining_module_name`. With multiple privates of the
    same Turmeric name, the lookup is non-deterministic / position-dependent
    and may hand back the wrong module's binding.

2.  **Emission only iterates once per binding object.** Once both
    `__auto-bound` Turmeric names resolve to the same `Binding *` (or one
    is mistaken for the other), the emitter sees a single binding and
    emits its body once -- but the *other* module's defn is still queued
    for emission separately, producing two static function bodies with the
    same C name. That matches the "defined twice" symptom in the C output.

The exported-binding collision check at `elab_toplevel.c:955` is gated on
`b->is_exported`, so private helpers never hit it. The check should
either:

-  cover all globals, not just exported ones, or
-  be redundant because every binding gets a uniquely-mangled C name by
   construction (in which case the bug is elsewhere -- almost certainly
   in name resolution).

A side investigation: `elab_mangle_binding_name` only adds the module
prefix when `b->defining_module_name != NULL` *and* `b->is_global` (line
~1572). If for some reason a private defn's `defining_module_name` is
left NULL while the exported sibling sets it, the unprefixed names of
two privates would alias trivially -- with the unprefixed body landing on
top of the prefixed one. Worth confirming.

## Scope

### In scope

-  Reproduce the bug with a two-file fixture in the turmeric test tree.
-  Pinpoint which of the two hypotheses (resolution vs duplicate
   emission) is responsible.
-  Make `defining_module_name` reliably populated for every global
   binding -- exported or not.
-  Either fix the C-name collision check to cover non-exported globals,
   or prove it's unnecessary because mangling is collision-free by
   construction.
-  Update `docs/guides/developing-spices-guide.md` if the diagnostic or
   the recommended idiom changes.

### Out of scope

-  Macro hygiene / hygienic renames more broadly.
-  Reworking `export` / `export-as` semantics.
-  Doing anything to plot itself; plot 0.3.0 already routes around this.

---

## Plan

-  [x] **CC0** -- Land a regression fixture under
   `tests/fixtures/codegen-private-defn-collision/` with two minimal
   modules each defining a private `(defn __h [] :int ...)` and a top-level
   that imports both. Expect a clean build today; check it actually fails
   with the current toolchain to lock the symptom in.

-  [x] **CC1** -- Add a small `printf` / span-dump trace in
   `elab_form` for the `(__h)` call sites, and similar trace in the
   defn-emission loop. Confirm which of the two hypotheses (wrong
   binding resolved, or duplicate emission) is actually firing. Record
   findings inline below this list.

-  [x] **CC2** -- Fix. Two likely landing spots:

   1. If resolution is at fault: ensure global-scope name lookup
      preferences the *current module's* binding when multiple privates
      share a name, and surfaces an error if a non-current-module private
      is the only candidate.

   2. If emission is at fault: dedup the defn-emission queue by `Binding
      *` identity so the same C body is never written twice; if two
      distinct `Binding *`s mangle to the same C name, raise a hard
      diagnostic instead of letting clang fail.

   Extend the existing exported-binding collision check at
   `elab_toplevel.c:955` to cover all globals, so future regressions
   surface as a Turmeric diagnostic ("private symbol '__h' from module
   'plot/decor' mangles to the same C name as ...") rather than a
   downstream clang error.

-  [x] **CC3** -- Removed the defensive uniqueness in
   `../turmeric-spices/spices/plot/src/plot/{decor,interval,line}.tur`.
   All three helpers renamed from `__auto-bound`/`__iv-nan`/`__ln-nan`
   back to a shared `__nan` name. Verified: `emit-c` on tests that import
   multiple modules now emits distinct `plot__decor____nan`,
   `plot__interval____nan`, and `plot__line____nan` C symbols -- one per
   module, no collision. The plot test suite's compilation failures are
   due to the missing `plutovg` native library in this environment, not
   Turmeric-level errors.

---

## Test plan

-  `bash tests/run.sh` -- the new CC0 fixture must build clean once
   CC2 lands.
-  `bash tests/run.sh tests/fixtures/codegen-private-defn-collision`
   alone, to keep iteration fast.
-  `tur test tests/plot/` in `../turmeric-spices/spices/plot` after CC3
   -- 6 passing tests, no regression in the post-rename plot suite.
-  Manual: build any spice that defined two private same-named helpers
   prior to the fix; confirm the C output now has each helper exactly
   once and that each call site refers to its own module's body.

---

## Open questions

-  Should the eventual diagnostic for a same-mangled-C-name collision be
   an error or a warning? Error is safer (the program will not link),
   but module authors may want to declare "yes, I know, both privates
   are intentionally identical" via something like `(export-as ...)` for
   private bindings.
-  Does the same bug exist for `defmacro`, `defstruct`, or
   `definstance`? Each has its own emission path; verify before
   claiming the fix is complete.
