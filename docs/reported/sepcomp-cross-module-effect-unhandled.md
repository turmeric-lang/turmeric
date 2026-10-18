# Separately compiled: an effect performed in one module misses a handler in another

**Narrowed 2026-10-18 (fix direction 2): refused at build time, no longer a
run-time abort.** Under separate compilation the effect checker
(`src/passes/effect_check.c`, "sepcomp-cross-module-effect-unhandled")
indexes the other modules' callees this unit reaches. It infers their rows in
the same fixpoint, registering an effect declared over there so its `perform`
counts. It then rejects at the call site a call whose callee can perform a
user effect:

```
./myapp.tur:6:7: error: `ask-effect` (module effects/ask) can perform the
effect `Ask`, and this call crosses into it from another module of a
separately compiled build. ... Handle `Ask` inside effects/ask, or build the
program as one unit (`tur run` or `tur build <main.tur>`), which threads it.
```

Capability tags and Unsafe cross freely. A module that handles its own effect
builds and runs. Both cases are in `tests/run-build-project.sh`
(`build-project-sepcomp-*`). Found on the way: a module that failed to compile
after another had compiled leaked that module's borrow specs, and the Debug
`tur`'s LeakSanitizer turned the compile error's exit 1 into 141
(`free_borrow_specs`, `src/main.c`).

**Severity is now medium** (a correct program is refused, with a remedy).
What is left is fix direction 1: thread the effect across units. That means
exported `__cps` entries, an importer that colors imported callees by their
rows, and effect tags agreed program-wide.

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
  (defeffect Ask [] :int)
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
