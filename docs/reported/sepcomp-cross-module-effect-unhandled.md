# Separately compiled: an effect performed in one module misses a handler in another

**Severity:** high. The program builds cleanly and aborts at run time. The same
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
