# Separately compiled: an effect performed in one module misses a handler in another (resolved)

**Resolved 2026-11-24 (fix direction 1): the effect is threaded across units.**
`tur build <dir>` of the repro below prints `99`. Four things make it work,
all under separate compilation only (a whole-program build emits the C it
always did):

- **The owner exports the `__cps` twin** of every exported function it
  CPS-emits: external linkage, hidden visibility (`xmod_exports_cps`,
  `src/compiler/emit_cps_ir.c`, "cross-module threading").
- **The importer threads into it.** The effect checker publishes the other
  modules' callees that can perform a user effect
  (`effect_check_foreign_effects`). The CPS translator treats a call to one as
  a call to a colored function, the emitter declares the twin `extern` and
  hands it the caller's chain, and the classification credits the caller with
  the callee's effects, so the taint rules keep a handler and such a call on
  the DK machine together.
- **Effect tags are derived from the effect's name** (`effect_emit_tag`:
  FNV-1a folded into `[2, INT32_MAX]`), so every unit agrees without sharing a
  table. Two names that fold to one tag are a build error, not a wrong
  handler.
- **The units share one DK machine.** This was not in the report. The machine's
  state (driver landing, resume chain, reap list, entry depth, pending-perform
  list, handler-group counter, re-opening slot) was `static` in every unit
  that included `tur_runtime.h`. With twins exported and tags agreed, a
  `perform` ran in the performer's unit and the handler's tail resume in the
  handler's, on two different driver landings: a nested-handler program built,
  printed nothing and exited with its answer as the status (`308 & 255`). The
  state is now defined once, in the `TUR_RT_OWNER` unit
  (`emit_dk_shared_state`, `src/compiler/emit_module.c`).

**What is still refused**, at the call site, with the side that failed named:

- the *caller* left the CPS backend's subset, so the call would be made on
  the direct path ("The call is not threaded here");
- the *callee* has no CPS ABI, or left the subset in its own module, so that
  module exports no twin ("Its module exports no threaded entry for it"). The
  importer cannot see the second case while it compiles: the driver keeps a
  ledger of exported and called twins, and recompiles an importer whose
  callee no unit exported with that twin denied
  (`emit_cps_xmod_deny_unsatisfied`), which turns a would-be "undefined
  reference" into the located error;
- the callee's module is not linked into the output (a `--shared` library's
  `:spices` dependency without `--bundle-deps`): it runs on another library's
  machine.

Tests: `build-project-sepcomp-*` in `tests/run-build-project.sh`,
`build-shared-cross-module-effect*` in `tests/run-build-shared.sh`.

Two whole-program defects turned up on the way and are filed separately:
[effect-through-evicted-fn-aborts-at-run-time](../reported/effect-through-evicted-fn-aborts-at-run-time.md)
and
[module-effectful-fn-value-has-no-lowering](../reported/module-effectful-fn-value-has-no-lowering.md).

**History.** Narrowed 2026-10-18 (fix direction 2): the effect checker
refused, at the call site, every call into another module whose callee could
perform a user effect, where the build used to succeed and abort at run time.
That refusal is what the three cases above are left of.

**Original severity:** high. The program builds cleanly and aborts at run time. The same
modules built as one translation unit work (that is how the
`module-cross-module-effect` fixture passes).

## Repro

The two files of `tests/fixtures/module-cross-module-effect`, laid out as a
bare directory (no `build.tur`, so `tur build .` compiles each module
separately):

```turmeric
;; effects/ask.tur
(defmodule effects/ask
  (export ask-effect)
  (defeffect Ask [] : int)
  (defn ask-effect [] : int
    (perform (Ask))))
```

```turmeric
;; myapp.tur
(defmodule myapp
  (export run)
  (import effects/ask :as ea)
  (defn run [] : int
    (handle
      (ea/ask-effect)
      (Ask [] k)
      (resume k 99)))
  (defn main [] : int
    (println (run))
    0))
```

```
$ tur build . && ./build/bin/myapp
tur: unhandled effect (tag 2)
```

Expected `99`.

## Root cause

CPS coloring and classification (`ensure_S`) run per translation unit, and a
module's `<fn>__cps` body is emitted `static`. In `myapp`'s unit,
`ea/ask-effect` is an external function. The handler calls it through its
plain C entry, which seeds a fresh DK root
([cps-dk-internals-guide](../guides/cps-dk-internals-guide.md)), so the
`perform` inside never sees `myapp`'s handler. The effect tags are also
assigned per unit (`effect_tag` starts at 2 in every TU), so even a threaded
call would need a program-wide tag agreement.

## Fix directions

- Export each module's `__cps` entry (non-`static`, in the header) and color
  imported callees by their declared effect rows, so a cross-unit call can
  thread the chain. Derive effect tags from the effect's qualified name, not
  a per-unit counter.
- Until then, make it a build-time error: a cross-unit call to a function
  whose row carries an effect that the caller handles.
- Add the bare-directory layout of this fixture as a separate-compilation
  fixture.
