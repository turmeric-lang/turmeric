# The fnsan gate never checks a callback that libturi calls

**Severity: low-medium (a gap in a gate; no wrong answer in the corpus today).**
**Discovered:** 2026-10-10, retyping `reactor-add-chan` (v0.63.9 + `main`).

`tests/run-fnsan.sh` and CI's "Suite under -fsanitize=function" job build
fixtures with `-fsanitize=function -fsanitize-trap=function`, but they link a
`libturi.a` built with gcc and no sanitizer flags (`build-nosan`, ci.yml).
`-fsanitize=function` checks an indirect call at the **call site**, so every
indirect call made from inside libturi goes unchecked. That covers the reactor
calling a source's callback, the local fiber group resuming a fiber body, and
anything else libturi dispatches through a Turmeric fat box.
`src/async/reactor.c`'s typedef comment assumes the opposite ("every reactor
fixture then tripped clang's -fsanitize=function here").

## Repro

A callback whose C type deliberately differs from reactor.c's `TurFdCbFn`
(a `float` value slot, registered through the raw extern so the checker's
float-vs-word rule does not refuse it):

```turmeric
(load "stdlib/chan.tur")
(defn reactor-link [] : int
  ```c /* __tur_autolink__: -lturi -pthread */
  return 0;
  ```)
(extern-c tur_reactor_new [] :ptr)
(extern-c tur_reactor_add_chan [r :ptr ch :ptr cb :int ud :ptr] :int)
(extern-c tur_reactor_poll [r :ptr timeout-ms :int] :int)
(defn main [] : int
  (let [r  (tur_reactor_new)
        ch (chan-new 2)
        k  1]
    (chan-send ch 5)
    (tur_reactor_add_chan r (:: ch :ptr<void>)
      (fn [id : int v : float user : int] : nil (println (+ v (if (= k 1) 0.5 0.25))))
      (:: 0 :ptr<void>))
    (tur_reactor_poll r 0)
    0))
```

Built with clang and the fnsan flags:

| libturi.a | Result |
| --- | --- |
| `build-nosan` (what the gate links) | runs, prints `0.5`: `v` read 0.0 from a float register reactor.c never loaded |
| clang, `-fsanitize=function -fsanitize-trap=function` | `Illegal instruction` at the call in `call_tur_chan_cb` |

## Measured: the corpus is clean with libturi instrumented

`TUR_FNSAN_LIB_DIR=<instrumented build> bash tests/run-fnsan.sh` over the whole
corpus gave **3684 passed, 1 failed**. The one failure was a fixture written the
same day, `reactor-chan-typed`. Its first version annotated a reactor callback
slot `v : cstr`, and it was changed before landing. So turning the
instrumentation on would not redden the job.

That failure is the class the gate is missing. A captureless lambda's fat shim
normalizes `ptr<void>` slots to `int64_t` but keeps a `cstr` slot as
`const char *`, e.g. `__tur_fatshim_void_int64_t_const_char___void__(void *,
int64_t, const char *, int64_t)`, while reactor.c calls
`(void *, int64_t, int64_t, int64_t)`. It runs on x86-64 and arm64 and is a
type mismatch everywhere. The reactor guide and `reactor-add-chan`'s
docstring now say to keep callback slots untyped, `int` or `ptr<void>`.

## Fix directions

1. **Instrument libturi in the fnsan job** (the measured one). In ci.yml's
   build-nosan configure, add
   `-DCMAKE_C_COMPILER=clang "-DCMAKE_C_FLAGS=-fsanitize=function -fsanitize-trap=function"`.
   Trap mode needs no UBSan runtime, and the job already requires clang. Then
   update `tests/run-fnsan.sh`'s header, which asks only for "an UNSANITIZED
   libturi.a": what it needs is no ASan, and this build has none.
2. **Make the boundary exact rather than documented**: have the fat shim
   lower every word-sized slot to `int64_t` when the box is headed into a
   carrier-convention caller (an `:int` callback parameter of libturi), as it
   already does for `ptr<void>`. Then a `v : cstr` slot is exact too.
