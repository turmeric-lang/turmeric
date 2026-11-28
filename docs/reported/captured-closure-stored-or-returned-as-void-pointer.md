# A captured closure stored into a fn-typed global, or a closure field returned, is an int-to-pointer conversion

**Severity: medium (a C build failure on gcc 14 / clang 15+, where
`-Wint-conversion` is an error; a warning, and the right answer, on older
toolchains).** Inside a lambda, a closure word read out of the env or out of a
struct field is an `int64_t` carrier. Two places hand it to a `void *` slot with
no cast.

Found 2026-11-28 while writing `tests/fixtures/closure-env-owns-capture-refused`
(closure-stored-beyond-its-let-has-no-owner). The fixture was reshaped to
avoid it. Reproduces with the compiler at the previous commit too, and is not
caused by that work.

## Repro

```turmeric
(defstruct Holder [run : (fn [int] int)  tag : int])
(defn zero [x : int] : int 0)
(def ^mut kept : (fn [int] int) zero)
(defn stash-step [k : int] : (fn [] int)
  (let [step (fn [x : int] : int (* x k))]
    (fn [] : int (do (set! kept step) 0))))
(defn get-run [k : int] : (fn [] (fn [int] int))
  (let [h (Holder (fn [x : int] : int (- x k)) 1)]
    (fn [] : (fn [int] int) (.run h))))
(defn main [] : int
  ((stash-step 4))
  (println (kept 2))
  (println (((get-run 5)) 10))
  0)
```

`tur run` prints `8` and `5`, after:

```
warning: assignment to 'void *' from 'int64_t' ... [-Wint-conversion]
        kept_12 = __env___env_36->step;
warning: returning 'long int' from a function with return type 'void *' ... [-Wint-conversion]
        return (int64_t)(__env___env_49->h).run;
```

`tests/run.sh` fails a fixture with either warning ("emitted C
representation warning").

## Root cause

Not investigated. Both are a closure word moving from its `int64_t` carrier
(an env field, a struct field) into a slot typed as the fn type's C spelling,
`void *`. That slot is the global `kept`, or the lambda's own return when its
result is fn-typed. The direct emitter's other carrier-to-pointer sites cast
through `(void *)(intptr_t)`; these two do not.

## Fix directions

1. Cast the word at the `set!` of a fn-typed global and at the return of a
   fn-typed lambda result when the value is an `int64_t` carrier.
   `tests/check-emitted-float-conversions.py`'s sibling checks are the model
   for finding the rest by mechanism.
