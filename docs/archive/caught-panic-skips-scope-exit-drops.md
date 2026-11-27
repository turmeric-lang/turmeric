# A panic that is caught skips the scope-exit drop of every local it unwinds past

**Severity:** medium (a leak per caught panic; `catch-unwind` is what keeps a
server alive after a handler panics, so the leak is per failed request).

`catch-unwind` and `catch-panic-of` let a program continue after a panic. The
frames the panic unwinds run their explicit `defer`s. They do not run the
drops the compiler injects for owning locals -- a move-only Drop opaque, a
`ref<T>` -- because those are registered only when the `let` body finishes.

Predates #1486, which noted the one-arm case of it. Verified 2026-11-27 on
`main` at `853f866`.

**RESOLVED 2026-11-27 (compiled path).** Fix direction 1. The panic-signal
return after a call (`emit_panic_signal_return`, `src/compiler/emit_expr.c`)
now runs, frame by frame, the scope-exit drops a do-emitter has noted as
pending and not pushed yet, ahead of that frame's own fire, with the panic
signal cleared -- as the `return` emitter already did
(`emit_frame_run_pending_drops_on_panic`, `src/compiler/emit_core.c`). It
covers a panic raised directly in the body (`(panic ...)` ends in the same
check) and one propagated from a callee, and the CPS-hook variant of the
check.

The drops are written once per scope, not at every call: a call site jumps
to a shared exit (`if (tur_panicking) goto __pexit_N;`), which the do-emitter
writes as `if (0) { __pexit_N: ... }` when the frame's scope closes
(`emit_panic_exit_goto` / `emit_frame_flush_panic_exits`). The first version
wrote them out at each site, and the growth was enough to change GCC 15's
inlining in `tests/vectorize/numvec-probe.tur`, so `probe-float-partials`
stopped vectorizing (`tur_numvec_vectorized`, the Ubuntu 26.04 CI leg). No
panic check follows a call inside those drops: they run while a panic is in
progress, so a second one is the double panic `tur_panic_at` aborts on.

The moved-binding question is settled when the drops are noted, once per
`do`: elab_let appends a drop only when the body as a whole did not hand the
binding on, and an `if` / `match` join ignores an arm that leaves -- so the
one way the body can have moved it on some path is a leaving arm (or a nested
`return`). A drop whose binding such an arm names anywhere but at a `^borrow`
argument or under `@` is left out of the panic exit (the old leak, never a
double release); `emit_drop_owned_at_every_panic` is that test. A guard keeps
the drops emitted on a `return` or panic exit from re-running the pending set
at their own call checks.

Pinned by `tests/fixtures/caught-panic-scope-exit-drops`, under
`run-leak-check.sh`: the report's pair, nested scopes, a drop beside a user
defer, a value handed on in an arm that then panics (released once), one only
borrowed there (released), and the `ref` / `rc` / owning-field cases. The
interpreter skips these drops on a panic exit as it does on a `return`; that
is [turi-early-return-skips-injected-drops](../reported/turi-early-return-skips-injected-drops.md),
and the fixture is `requires.compiled` until it is fixed.

The arm-level half (#1486's one-arm shape, `(if c (eat h) (panic ...))`):
`elab_branch_with_scope_drop` used to leave a panicking arm unchanged, so the
value it still owned was never released. It now registers the drop AHEAD of
such an arm -- `(do (defer (drop h)) <arm>)` -- so the unwind fires it, in
both engines. Only when the arm uses the binding at most as a `^borrow`
argument; a nested leaving arm could otherwise have handed it on first
(pinned: `one-arm-nested`). That wrapper is a `!`-typed `do` with a frame,
which `emit_tail` (`src/compiler/emit_fns.c`) now spells like a tail `panic`:
the statement, then the function's zero.

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
