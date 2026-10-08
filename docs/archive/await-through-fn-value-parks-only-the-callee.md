# An `await` reached through a function value parks only the callee's continuation

**RESOLVED 2026-10-08** (filed and fixed the same day). Root cause as
measured below: `await` adds nothing to an effect row, and the fn_cps slot of
a function value was filled only for a non-runtime-pure row. Now a function
that may suspend on an `await` gets the slot as an effectful one does.
`cps_color_program` computes a transitive `may_await` flag (its own `await`,
or a named callee that may; nested lambdas and `(async ...)` bodies are
boundaries), and both fills in `emit_expr.c` -- the named-fn arm and the
capturing-lambda arm -- read it through `cps_fn_may_await`. Pinned by
`tests/fixtures/await-through-fn-value`: a named awaiting fn, one that awaits
only through a named callee, a capture-free lambda and a capturing lambda,
each 50 turns over a parked future. Before the fix they printed 90 / 91 / 82 /
580, against 3275 / 3325 / 4100 / 15525. No codegen snapshot moved; suite
3626/0, turi 2670/0, JIT over the effect/async/CPS fixtures 312/0,
leak-check 126/0/3 known.

The leak noted under "Also seen here" was NOT fixed by this. It was filed as
[async-parked-body-chains-never-reaped](async-parked-body-chains-never-reaped.md),
since fixed too (it turned out not to depend on parking).

**Severity: medium-high (silent wrong answer).** A function that `await`s,
called through a function value from an `async` body, suspends only its own
rest. The caller's loop carries on with a placeholder value, so the async
body finishes with the wrong result and nothing is reported. Calling the same
function directly gives the right answer.

Found 2026-10-08 while writing the async arm of the
[fn-value-call-cps-frames-held-until-outer-entry](../reported/fn-value-call-cps-frames-held-until-outer-entry.md)
fixture. It predates that change: a build with the change stripped prints the
same numbers.

## Repro

````turmeric
(defn the-future [] : ptr<void>
  ```c
  static TurFuture *g = NULL;
  if (!g) g = tur_future_new();
  return (void *)g;
  ```)
(defn fulfill-future [f : ptr<void> v : int] : nil
  ```c
  tur_future_fulfill((TurFuture *)f, (int64_t)v);
  ```)
(defn future-value [f : ptr<void>] : int
  ```c
  return (int64_t)tur_future_get((TurFuture *)f);
  ```)

(defn await-plus [x : int] : int (+ x (await (the-future))))
(defn use1 [f : (fn [int] int) i : int] : int (f i))

(defn work-loop [i : int n : int acc : int] : int
  (if (= i n) acc (work-loop (+ i 1) n (+ acc (use1 await-plus i)))))

(defn work [] : int (work-loop 0 2 0))

(defn main [] : nil
  (let [f (async work)]
    (fulfill-future (the-future) 41)
    (println (future-value f))))
````

| loop length | `(use1 await-plus i)` | `(await-plus i)` (direct) | right answer |
| --- | --- | --- | --- |
| 1 | 41 | 41 | 41 |
| 2 | **42** | 83 | 83 |
| 3 | **43** | 126 | 126 |

## Root cause (measured; fixed 2026-10-08, see the top)

The function value is built without a CPS entry:

```c
__t4 = (tur_poly_fn_t){ NULL, (int64_t(*)(void*,int64_t))__poly_21 };
```

so `use1__cps` takes its direct arm and calls `await-plus` through its direct
wrapper. That wrapper installs its own root prompt, the `await`'s shift to
`DK_ROOT_TAG` captures only `await-plus`'s rest, and the park resumes that
alone. The wrapper returns a dummy value (with `tur_async_suspended` set), and
`work-loop` adds it to its accumulator and continues.

The `fn_cps` slot is filled only when the source function's effect row is not
runtime-pure (`emit_expr.c`, the E2 named-fn and capturing-lambda fills,
`effect_row_is_runtime_pure`). An effectful function gets it: the same loop
through `(use1 ask-plus i)`, where `ask-plus` performs a handled effect,
answers correctly. So `await` evidently leaves the row runtime-pure. Either it
contributes no effect or it contributes only a capability. That is the
likely fix site: an awaiting function needs the CPS entry in its function
value exactly as an effectful one does.

## Fix directions

1. Make an `await` (and anything else that shifts to the entry root) count
   toward the runtime-impurity test that decides the `fn_cps` fill, so the
   function value threads the caller's chain.
2. Failing that, refuse the call at compile time when a function value that
   can suspend reaches a direct-arm call site. A loud refusal beats the
   current silent wrong answer.

## Also seen here

The same program, built under ASan, leaks ~12 KB over 50 turns (DK nodes and
`await-plus`'s continuation env). A parked body's chains are never reaped,
because every entry wrapper skips its reap while `tur_async_suspended` is set.
The archived
[compiled-async-heap-continuations-plan](compiled-async-heap-continuations-plan.md)
lists a narrower form of this ("a parked async body leaks its `__root`
prompt") as a known residual. It scales with the number of awaits, not one
prompt per park.
