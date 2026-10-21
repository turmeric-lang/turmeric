# The continuation forms: `shift0` is `shift`, a bare compiled `shift` has no reset to find, and a serial continuation cannot be saved when interpreted

**Severity: low-medium.** Found 2026-10-19 checking the examples for the
continuation forms' API reference (`stdlib/forms/continuations.tur`).

**Narrowed 2026-10-21.** Two abortive resets plus a capture, a float through an
escape `k`, and `call/cc` of a non-function are resolved: see
[cps-reset-continuation-joins-and-escape-float](../archive/cps-reset-continuation-joins-and-escape-float.md).

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

## 2. A `shift` with no `reset` around it is not diagnosed when compiled (decision)

```turmeric
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn main [] : int
  (println (reset (h)))
  (println (h))
  0)
```

`tur run` prints `6` and then exits with **status 6**: the second `(h)` has no
reset, `dk_run_impl` takes the root prompt as the delimiter (`to_root`, on
purpose: `call/cc` and `escape` rely on the implicit root prompt), and the
receiver's value becomes the process's exit code. Without the first line the same
`(println (h))` prints 42 for `(* 2 21)` and the program continues. `tur
--interpret` stops with `shift used outside of any reset boundary`.

Decision: is a bare `shift` an error (the interpreter) or captures to the root (the
compiled program)? Making it an error compiled means a runtime check in the DK
machine (a preamble change: it regenerates the snapshots) or a static check that
no call path reaches a `shift` without a `reset`.

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
