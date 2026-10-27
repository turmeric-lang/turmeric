# Direct emitter: an effectful fn param invoked inside a handle in the same fn -> 'f' undeclared

**Severity:** medium (compile error; no miscompile). Mainline direct/fiber
codegen -- NOT CPS-backend-specific. Found while scoping effectful-callback
support for the CPS backend.

## Summary

A function that takes an effectful fn-value parameter and INVOKES it inside a
`handle` in the SAME function fails to compile: the emitted C references the
parameter (`f`) in a helper/handler scope where it was never declared.

## Minimal repro

```turmeric
(defeffect E [] : int)
(defn run-with [f : (fn [] #fx{E} int)] : int
  (handle (f)
    (E [] k) (resume k 5)))
(defn main [] : int
  (println (run-with (fn [] (perform (E)))))
  0)
```

`tur build repro.tur` (no flags -- plain direct/fiber path):

```
error: 'f' undeclared (first use in this function)
error: '__ps_NNN' undeclared (first use in this function)
```

Both a lambda argument (`(fn [] (perform (E)))`) and a named colored function
argument reproduce it.

## Contrast: the propagate-up pattern DOES compile

The same effectful param invoked WITHOUT a same-function handle -- the effect
propagates up to a caller's handler -- compiles and runs correctly:

```turmeric
(defn call-writer [f : (fn [cstr] #fx{Write} nil)] #fx{Write} : nil
  (f "hi"))                                     ; OK
```

So the bug is specific to the `handle`-in-the-same-function-that-invokes-`f`
shape: the delimited body `(f)` runs inside a fiber block whose scope does not
capture the enclosing parameter `f`.

## Fix direction

The direct/fiber lowering of a `handle` whose delimited body invokes an enclosing
fn-value parameter must capture that parameter into the fiber block's env (as it
does for other locals referenced from the delimited body). The `__ps_NNN`
companion error suggests the call's result temp is also declared in the wrong
scope.

## Relationship to the CPS backend

Orthogonal, but it bounds the CPS effectful-callback work
(`docs/upcoming/v1/cps-backend-effectful-callbacks-plan.md`): the propagate-up
pattern is the sound baseline to target; the handle-in-same-fn pattern must be
fixed on the direct path first (or the CPS path must handle it without relying on
the direct emitter).
