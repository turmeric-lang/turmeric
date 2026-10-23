# The continuation forms: `shift0` is `shift`, a compiled `shift` is delimited by the root, and a serial continuation cannot be saved when interpreted

**ARCHIVED 2026-10-21** -- split into reports and plans; see the disposition below.

## Disposition (2026-10-21) -- archived; every open item has a home

- Item 1 (`shift0` is `shift`): decision -- implement the pop; feasibility
  investigated (small, after the root-delimiting fix). Plan:
  [shift0-pop-plan](shift0-pop-plan.md).
- Item 2 (a compiled `shift` delimited by the root; the wrong answer under a
  direct `reset`): its own report,
  [compiled-shift-delimited-by-root](compiled-shift-delimited-by-root.md).
- Item 3 (`call/cc` `k` reuse unchecked): decision -- leave as is; the guide says
  only `^linear` is checked.
- Item 4 (serial continuation bytes interpreted): decision -- compiled-only,
  documented in `stdlib/serial.tur`, the `serial-shift` forms entry and
  eval-api's interpreter limits.
- Item 5 (`(k 2.25)` into a `float32` continuation): its own report,
  [escape-cont-float32-literal-arg](escape-cont-float32-literal-arg.md) (resolved 2026-10-22).

**Severity: medium** (item 2 is a silent wrong answer). Found 2026-10-19 checking the examples for the
continuation forms' API reference (`stdlib/forms/continuations.tur`).

**Narrowed 2026-10-21.** Two abortive resets plus a capture, a float through an
escape `k`, and `call/cc` of a non-function are resolved: see
[cps-reset-continuation-joins-and-escape-float](cps-reset-continuation-joins-and-escape-float.md).

## 1. `shift0` behaves as `shift` (decision: implement the pop, or retire the distinction)

The guides (delimited-control-operators-guide, effects-system-guide) say `shift0`
removes the reset it captured to, so a `shift` in its receiver reaches the next
reset out:

```turmeric
(defn main [] : int
  (println (reset (+ 100 (reset (+ 10 (shift0 (fn [v : int] (shift (fn [w : int] (* 1000 w)) v)) 5))))))
  (println (reset (+ 100 (reset (+ 10 (shift (fn [v : int] (shift (fn [w : int] (* 1000 w)) v)) 5))))))
  0)
```

Both engines print 5100 twice; the standard `shift0` gives 5000 for the first.
`shift0` also accepts only a value receiver (a `cont` receiver is TUR-E0001), so
there is no program in which it can resume. The fixtures that use it
(`continuation-substrate`, `cps-backend-shift0`, `delimited-control-guide-examples`)
are all shapes where it equals `shift`. The guide and the forms page now say so.

The DK machine already distinguishes the two when the receiver resumes
(`reinstall = k->kind == DKK_SHIFT` in `dk_run_impl`), but a value receiver never
resumes. The pop would have to run the receiver in the context beyond the popped
prompt (the continuation after `P`, not the captured slice with a `done` tail), and
the nested-run model of `k->body(env, sub)` would then deliver that context's
result twice; it needs a design pass, not a patch.

## 2. A compiled `shift` delimited by the root: a bare `shift` is not diagnosed, and a direct `reset` around a CPS callee gives a wrong answer

```turmeric
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn main [] : int
  (println (reset (h)))
  (println (h))
  0)
```

`tur run` prints `6` and then exits with **status 6**: the second `(h)` has no
reset, and `dk_run_impl` (`src/compiler/emit_dk_runtime.c`), finding no prompt
with the reset tag, delimits the shift at the root (`to_root`) -- so the
receiver's value becomes `h`'s entry's result, here `main`'s, and the exit code.
`tur --interpret` stops with `shift used outside of any reset boundary`.

The same root capture is what makes a *direct-emitted* reset work around a CPS
callee, and it is wrong there in a non-tail position. When `main` is evicted to
the direct emitter (a `catch-unwind` in it is enough), `(reset (h))` has no
lexical `shift`, so it pushes no setjmp context (`tur_cur_shift_reset`); `h`'s
shift delimits at `h`'s own entry instead:

```turmeric
(defn main [] : int
  (println (reset (h)))           ; 6 compiled, 6 interpreted
  (println (reset (+ 10 (h))))    ; 16 compiled, 6 interpreted
  (let [r (catch-unwind (fn [] : int 0))] (println (err? r)))
  0)
```

The abortive shift should discard `(+ 10 _)` along with `(+ 1 _)`; compiled, only
the part inside `h` is discarded. A silent wrong answer.

**Tried 2026-10-21 and reverted:** a panic in `dk_run_impl` when a reset-tag shift
reaches the root with no `tur_cur_shift_reset` live. It turned the working
`(reset (h))` above into a panic, because the direct reset over a call never
pushes that context.

Fix: a direct-emitted `reset` whose body can reach a CPS function's `shift` has to
be a real delimiter for it -- either push a DK prompt (enter the body through the
DK machine) or always install the setjmp landing and have the root-reaching shift
longjmp to it. With that in place, a shift that still reaches the root has no
reset around it and can be the interpreter's error.

## 3. `call/cc`'s one-shot rule is only checked for `^linear`

effects-system-guide used to say the default `k` is `^unique` and calling it twice
is `TUR-E0100` / `TUR-E0101`. `(call/cc (fn [k] (+ (k 1) (k 2))))` passes `tur
check`; the first `(k 1)` leaves the call, so the second never runs. The guide now
says only `^linear` is checked. Open only if the stronger check is wanted.

## 4. A saved serial continuation cannot be restored by the interpreter

`serial-cont->bytes` and `bytes->serial-cont` are inline C taking a continuation,
and `tur --interpret` stops with `inline-C defn 'serial-cont->bytes' takes a
parameter of type cont<int>; the REPL JIT passes only int-class, float, cstr and
bool values`. The same program compiled prints 142. The serial forms themselves
run on both engines; only the byte round trip is compiled-only: the interpreter's
serial continuation is its own structure, not the DK chain the inline C marshals.
The page says so.

## 5. Small

`(k 2.25)` for an `escape-cont<float32>` hands over the double literal's bits, so
the float32 landing reads 0 compiled (a `float32`-typed argument is right).
`(k v)` should convert the argument to the continuation's declared element type.
