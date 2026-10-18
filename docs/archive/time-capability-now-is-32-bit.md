# A `Real-Time` capability's `now` truncates epoch milliseconds to 32 bits

**Severity: medium (silent wrong value).** The Time capability's `now` slot is
`int (*)(void)`, and `__tur_real_time_now` returns `(int)(time(NULL) * 1000)`.
Epoch milliseconds are about 1.79e12, far past `INT32_MAX`, so the value
wraps. It is also only accurate to the second. `now-ms`, the direct function
beside it, is correct, so the two disagree. Filed 2026-10-17 while revisiting
[dynvar-adoption-plan](../upcoming/dynvar-adoption-plan.md), whose mock-time
target lives in the same capability.

## Repro

Measured on the v0.65.5 release binary. `cap-now` does what `capability.tur`'s
`time-now` does (`((__tur_cap_time*)t)->now()`):

```turmeric
(defmodule timecap
  (import time :refer [Real-Time Time now-ms])
  (defn cap-now [t : Time] : int
    ```c
    typedef struct { int (*now)(void); void (*sleep)(int); } cap;
    return ((cap *)t)->now();
    ```)
  (defn main [] : int
    (let [clock (Real-Time)]
      (println (cap-now clock))
      (println (now-ms)))
    0))
```

```
1254165568        <- the capability: wrapped, and whole seconds
1792255528626     <- now-ms
```

`capability.tur`'s own `time-now` is the API a program would use instead of
`cap-now`, but it cannot be called on a `Real-Time` handle here.
`capability.tur:23` does `(load "stdlib/time.tur")`, and `time.tur` is a
`defmodule`. So a program that imports both `capability` and `time` defines
`Time` twice:

```
stdlib/time.tur:81:14: error: defopaque: 'Time' is already defined
```

## Root cause

The `now` slot is declared `int (*)(void)` in both struct copies of the Time
capability:
- `__tur_time_cap` and `__tur_mock_cap` (`stdlib/time.tur:41-42`);
- `__tur_cap_time` (`stdlib/capability.tur:55-58`), whose comment says it has
  "the same layout" so that a handle from `time.tur` is read there directly.

The three implementations follow that slot:
- `__tur_real_time_now` (`time.tur:44-47`) multiplies seconds by 1000 and casts
  the result to `int`.
- `__tur_hires_time_now` (`time.tur:55-57`) returns `clock()`, which is CPU
  time and not wall time. Its docstring does say "CPU-clock", but it fills the
  same "milliseconds since the epoch" slot.
- `__tur_mock_time_now` (`time.tur:68-70`) reads a process-wide singleton.

The singleton exists because the slots take no context, so a `Mock-Time` handle
cannot reach its own `current_time` field. Hence the docstring rule at
`time.tur:162-163`: "only one Mock-Time should be live at a time".

## Fix directions

1. **Widen `now` to `int64_t`** in all three struct copies and the three
   implementations, and have `__tur_real_time_now` use the same
   `clock_gettime(CLOCK_REALTIME)` read as `now-ms`. `make-Time`'s
   `(c-fn [] i32)` parameter (`capability.tur:231`) widens with it.
2. **Give the slots a `void *self` argument.** `Mock-Time` can then read its
   own `current_time`. That removes the singleton, and with it the "one live
   at a time" rule. It is also the fix the dynvar plan's mock-time target
   actually wanted.
3. **Let one module own `Time`.** For example, `capability.tur` could
   `(import time :refer [Time])` instead of `load`ing the file, so that
   importing both modules works.

## Resolution (2026-10-18)

Fix directions 1 and 2. Direction 3 is filed on its own, because it is not
about time:
[defmodule-file-loaded-and-imported-defined-twice](defmodule-file-loaded-and-imported-defined-twice.md).

- **`now` is 64-bit.** The slot is `int64_t (*now)(void *self)` in
  `__tur_time_cap`, `__tur_mock_cap` (`stdlib/time.tur`) and
  `__tur_cap_time` (`stdlib/capability.tur`). `Real-Time`'s `now` reads
  `__tur_wall_clock_ms`, a file-scope helper that `now-ms` now calls too, so
  the two agree and both have millisecond resolution. A Mock-Time's clock is
  an `int64_t`, and `mock-time-set` stores it without the `(int)` cast.
  `make-Time` takes `(c-fn [] i64)`.
- **The slots take `self`.** `__tur_mock_time_now` / `_sleep` read and move
  the handle's own `current_time`, and `__tur_mock_time_singleton` is gone, so
  any number of Mock-Times can be live at once. `time-now` and `time-sleep`
  pass the handle. `make-Time`'s callbacks still take no context: its handle
  is a larger struct that keeps them, and its two slots are trampolines that
  call each one through its own type, so `-fsanitize=function` stays clean.
- **CPU clock.** `HighRes-Time`'s `now` and `get-cpu-time` share
  `__tur_cpu_clock_ms`, which widens `clock()` before the multiply (a 32-bit
  `clock_t` overflowed `now * 1000` after 36 minutes) and no longer truncates
  to `int`.

Pinned by `capability-module-roundtrip`: a `make-Time` callback returning
`1792255528626`, two Mock-Times read through `time-now` with different
clocks, and a Real-Time whose `now` is within a minute before `now-ms`.
Checked under `tur run`, `tur jit`, and clang `-fsanitize=function`.

`stdlib/test/capability.tur`'s `Test-Time` has the same process-wide clock
shape (`__tur_test_time_value`), and the dynvar-adoption plan lists it beside
this one. Its API is untyped (`:int` handles, a handle-less
`testtime-reset`), so giving it a `self` changes its signatures; it is left
for that plan.
