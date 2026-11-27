# A panic that is caught skips the scope-exit drop of every local it unwinds past

**Severity:** medium (a leak per caught panic; `catch-unwind` is what keeps a
server alive after a handler panics, so the leak is per failed request).

`catch-unwind` and `catch-panic-of` let a program continue after a panic. The
frames the panic unwinds run their explicit `defer`s. They do not run the
drops the compiler injects for owning locals -- a move-only Drop opaque, a
`ref<T>` -- because those are registered only when the `let` body finishes.

Predates #1486, which noted the one-arm case of it. Verified 2026-11-27 on
`main` at `853f866`.

## Repro

`r/main.tur`:

```turmeric
(defopaque ^affine Widget :int)
(defn mk [] : Widget (println "  mk") (:: 1 Widget))
(defn rel [w : Widget] : void (println "  rel") (:: w int) nil)
(definstance Drop [Widget] (drop [x] : void (rel x)))
(defn peek [^borrow w : Widget] : int 1)

(defn explicit [] : int
  (let [x 3]
    (defer (println "  deferred"))
    (panic "boom")
    x))

(defn callee-panics [] : int (panic "boom"))

(defn injected [] : int
  (let [h (mk)]
    (callee-panics)
    (peek h)))

(defn main [] : int
  (println "an explicit defer") (catch-unwind (fn [] : int (explicit)))
  (println "an injected drop") (catch-unwind (fn [] : int (injected)))
  0)
```

```
an explicit defer
  deferred
an injected drop
  mk
```

The widget is never released. A `ref<T>` behaves the same:

```turmeric
(defn boom [] : int
  (let [r (ref 41)]
    (panic "boom")
    @r))

(defn main [] : int
  (println (err? (catch-unwind (fn [] : int (boom)))))
  0)
```

Under `TUR_SANITIZE_PROGRAMS=1` LeakSanitizer reports the 8-byte cell.

## Root cause

`elab_let` decides each binding's scope-exit drop after elaborating the body,
because the decision depends on whether the body moved or consumed the
binding. It then appends the drop defers after the body's items
(`src/compiler/elab_forms.c:2739`, "Add defer expressions ... at the end").
A `defer` is registered on the frame when control reaches it, so one that
sits after the body is not registered while the body runs. An early `return`
is handled statically -- the emitter runs the not-yet-registered drops inline
(`emit_frame_run_pending_drops`, `src/compiler/emit_core.c:3522`). A panic has
no such step: unwinding fires what the frame holds
(`tur_frame_fire_lifo`), and the drop is not in it.

The arm-level drops #1486 added have the same shape: an arm that panics is
left unchanged by `elab_branch_with_scope_drop`
(`src/compiler/elab_forms.c:818`).

## Fix directions

- Run the pending drops on the panic path too. Every call site already emits
  `if (tur_panicking) { ... tur_frame_fire_lifo(&frame); ... return; }`; the
  pending drops the emitter knows at that point could be emitted there, as
  they are for `return`. A binding the body has moved by then must be
  skipped, which the emitter can only know for moves that precede the call
  on every path.
- Or register the drop at the top of the body, guarded by a per-binding
  "still owned" flag the move sites clear. That is correct on every path,
  panic included, at the cost of a flag per owning local.
- A fixture under `tests/run-leak-check.sh`: the two programs above.
