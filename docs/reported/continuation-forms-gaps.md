# The continuation forms: two abortive resets plus a capture will not build, a float through `k` comes out as garbage, and `shift0` is `shift`

**Severity: medium.** Found 2026-10-19 checking the examples for the continuation
forms' API reference (`stdlib/forms/continuations.tur`). The 9 cases in that page's
doctest file are in one function, so they ran into the first item and the page
annotates two of them (`; doctest: two abortive resets in one function ...`).
Delete the annotations when it is fixed.

## 1. A function with two or more abortive resets and a capture does not compile

```turmeric
(defn f [] : int
  (+ (reset 1) (reset 2)
     (cloneable-reset (+ 1 (cloneable-shift (fn [k : cont] (k (k 10))) 0)))))
(defn main [] : int (println (f)) 0)
```

`tur run`: `TUR-E0710 cloneable-shift context is not capturable`. `tur --interpret`
prints 15. With a `call/cc` in place of the `cloneable-reset`:

```turmeric
(defn f [] : int
  (+ (reset 1) (reset 2) (call/cc (fn [k] (+ 100 (k 41))))))
```

`tur run`: `tur: emit: EX_CALLCC reached the direct emitter (should be handled by
the CT-IR backend)`, an internal error and exit status 134; interpreted, 44.
`TUR_TRACE_EVICT=1` shows `[EVICT] BODY-STRUCT-CORE eff=0 f`: the function left
the CPS backend's supported subset, so the direct emitter, which cannot lower
either form, gets it.

Not triggered by: any number of `reset`s alone (two and four were tried),
`call/cc` or `cloneable-reset` alone, two or three `call/cc`s, or one abortive
`reset` beside the capture. A `reset` with
a `cont` receiver does not count (it is promoted to a cloneable reset). The same
happens with `serial-shift` (`TUR-E0706 serial-shift context is not capturable`),
`call/cc*` (E0710) and `escape` (the internal error), and across statements as
well as operands: nine example lines in one `(unsafe ...)` block hit it, and any
one of them removed does not help until only one abortive reset is left. Giving
each reset its own `defn` works. `escape-in-main-reaches-direct-emitter`
(archived) fixed a diagnostic for one shape; this is the general one.

## 2. A float passed to `k` of a compiled `call/cc` / `escape` is read as an integer

```turmeric
(defn e1 [] : float (+ 0.5 (call/cc (fn [k : escape-cont<float>] (k 2.25)))))
(defn e3 [] : float (escape (fn [k : escape-cont<float>] (k 2.25))))
(defn main [] : int (println (e1)) (println (e3)) 0)
```

`tur run` prints `4.61225e+18` twice; `tur --interpret` prints `2.75` and `2.25`.
Returning a float normally (`(call/cc (fn [k] 3.25))`) is right. The emitted C
reads the escaped word with a value conversion, `__t0 = (double)__cc0.result;`
(`src/compiler/emit_cps_ir.c:13966`, `%s = (%s)%s.result;`), where the word holds
the double's bits; `python3 tests/check-emitted-float-conversions.py FILE.tur`
reports it as an I2F conversion. Fix: reinterpret the bits for a float result
type instead of converting the value, and add a fixture.

## 3. `shift0` behaves as `shift`

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
there is no program in which it can resume. The fixtures I read that use it
(`continuation-substrate`, `cps-backend-shift0`, `delimited-control-guide-examples`)
are all shapes where it equals `shift`. Either implement the pop or say in the guides
that `shift0` is an alias for abortive `shift`.

## 4. A `shift` with no `reset` around it is not diagnosed when compiled

```turmeric
(defn h [] : int (+ 1 (shift (fn [v : int] (* 2 v)) 3)))
(defn main [] : int
  (println (reset (h)))
  (println (h))
  0)
```

`tur run` prints `6` and then exits with **status 6**: the second `(h)` has no
reset, and the receiver's value becomes the process's exit code. Without the first
line (no `reset` anywhere in the program) the same `(println (h))` prints 42 for
`(* 2 21)` and the program continues. `tur --interpret` stops with `shift used
outside of any reset boundary`, which is what both should do.

## 5. `call/cc`'s argument is not checked, and the guide's one-shot rule is not

- `(call/cc 5)`: `tur check` is silent; `tur run` is the internal error message of
  item 1; the interpreter says `call/cc expects a function, got tag 2`.
- effects-system-guide says the default `k` is `^unique` and "calling it more than
  once is `TUR-E0100` / `TUR-E0101`". `(call/cc (fn [k] (+ (k 1) (k 2))))` passes
  `tur check`; only `^linear k` is checked (`TUR-E0101`). The first `(k 1)` leaves
  the call, so the second never runs -- harmless, but the guide's claim is wrong
  or the check is missing.

## 6. A saved serial continuation cannot be restored by the interpreter

`serial-cont->bytes` and `bytes->serial-cont` are inline C taking a continuation,
and `tur --interpret` stops with `inline-C defn 'serial-cont->bytes' takes a
parameter of type cont<int>; the REPL JIT passes only int-class, float, cstr and
bool values`. The same program compiled prints 142. The serial forms themselves
run on both engines; only the byte round trip is compiled-only. The page says so.

## Fix directions

1. Find why `BODY-STRUCT-CORE` evicts a function with two abortive resets and a
   capture, or lower the capture on the direct path; add a fixture for each
   of the five captures.
2. Fix item 2's conversion and pin it with the float shapes above.
3. Implement the pop in `shift0` or retire the distinction in the guides.
4. Make the compiled program diagnose a `shift` with no dynamic `reset`.
5. Type-check `call/cc`'s argument as a function; reconcile the guide with the
   checker on repeated use of an unannotated `k`.
