# A user `defdata` constructor is passed a closure pointer where it takes `int64_t`

> **RESOLVED 2026-09-26.** See [Resolution](#resolution) at the end. The
> analysis below is the original filing.

**Severity:** low -- the program runs correctly, but the emitted C carries a
`-Wint-conversion` warning, and `tests/run.sh`'s representation check fails any
fixture whose C has one. Found 2026-09-25 while writing
`tests/fixtures/hkt-ap-partial-head`; reproduces on the compiler before that
change.

## Repro

```turmeric
(defdata Either :copy [A E] (Right A) (Left E))
(defn main [] : int
  (let [bump 5
        cap  (:: (Right (fn [x : int] : int (+ x bump))) (Either (fn [int] int) int))]
    (println (match cap (Right f) (f 1) (Left e) e)))
  0)
```

```
warning: passing argument 1 of 'ctor_Either_Right__fn1_int__int__int' makes
integer from pointer without a cast [-Wint-conversion]
```

It prints `6`, which is right.

## Cause

The monomorphized constructor declares a function-typed field as the `int64_t`
carrier, `ctor_Either_Right__fn1_int__int__int(int64_t _0)`, while the call
site hands it the fat-closure handle as `void *` with no cast. A non-capturing
lambda does not trip it, and neither does the stdlib's `ok`, whose generic
specialization takes the payload as `void *`
(`ok__spec__..._void__(void *)`).

## Fix directions

Cast at the constructor call when the argument's C type is a pointer and the
field slot is the `int64_t` carrier (`(int64_t)(intptr_t)`), or declare a
function-typed field's constructor parameter as `void *` the way the generic
`ok` spec does. Once fixed, `hkt-ap-partial-head` can take its capturing-closure
case back.

## Resolution

The constructor argument path already bridges a pointer argument into an
`int64_t` slot (the straddle check against the constructor's recorded
parameter C types in `src/compiler/emit_expr.c`), but it recognised a
pointer argument only from a few spellings and from spec parameters. A
capturing closure's value is a `void *` temp. The check now also reads a
closure or boxed-function argument as `void *`, and falls back to a temp's
recorded C type, so the handle is cast `(int64_t)(intptr_t)` into a
type-variable field that the monomorph lays out as the carrier.

`tests/fixtures/hkt-ap-partial-head` takes its capturing-closure case back
(`ap` on a user `Either` holding a capturing closure, directly and through
the Applicative dictionary). It fails the suite's representation check on
the previous compiler and passes under both harnesses.
