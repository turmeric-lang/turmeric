# Fat-closure env is never freed (leaks once per construction)

**Status:** RESOLVED (scoped-free escape analysis). See the Resolution section
below.

**One-line summary:** Every `(fn ...)` that captures a free variable
heap-allocates a fat-closure env struct that is never `free`d, so closures
constructed in a loop or a repeatedly-invoked effect handler leak unboundedly.

**Severity:** Latent defect / ergonomics + memory-safety hole. Not a
miscompile -- the program produces correct results -- but it is an unbounded
heap leak in hot paths. Today it is masked because `tests/run.sh` compiles
fixture binaries with the *default* (non-ASan) toolchain, so the spawned
program's runtime leaks are invisible; only the `tur` compiler process itself
is leak-checked. The leak is real and shows immediately when the emitted C is
compiled with `-fsanitize=address` + `detect_leaks=1`.

## Minimal repro

```turmeric
(defeffect Ask [] : int)

;; The handler body constructs a local closure `f` that captures `base`.
(defn run [base : int] : int
  (handle
    (+ (perform (Ask)) (perform (Ask)))   ;; performs Ask twice
    (Ask [] k)
    (let [f (fn [x : int] : int (+ x base))]   ;; <-- fat closure, env malloc'd
      (resume k (f 1)))))

(println (run 100))                        ;; => 202 (correct)
```

Compile the emitted C with ASan and run with leak detection on:

```sh
tur emit-c repro.tur > repro.c
cc -g -O0 -fno-strict-aliasing -fsanitize=address,undefined \
   -Isrc/runtime -o repro repro.c \
   src/runtime/hamt.c src/runtime/runtime.c -lpthread
ASAN_OPTIONS=detect_leaks=1 ./repro
```

### Observed

```
==NNN==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 16 byte(s) in 1 object(s) allocated from:
    #1 ... in __effect_handler_26 repro.c:NN     <- malloc(sizeof(struct __env_882))
    ...
SUMMARY: AddressSanitizer: 32 byte(s) leaked in 2 allocation(s).
```

The handler runs once per `perform`, so two invocations leak `2 x 16 = 32`
bytes. A closure built inside a `while`/`for` loop leaks one env per iteration.

### Expected

The closure env should be released once the closure is no longer reachable
(after `f`'s last use here). Some ownership/drop story for fat-closure envs --
or at minimum a scoped free for closures that demonstrably do not escape their
defining block.

## Root cause

The fat-closure construction emits a bare `malloc` with no matching `free`:

`src/compiler/emit_fns.c` (closure construction; the `struct __env_<id>` heap
allocation) emits

```c
struct __env_882 *__t28 = (struct __env_882 *)malloc(sizeof(struct __env_882));
__t28->__fn  = (...)__fn_880;
__t28->base  = __henv_25->base;
```

with no corresponding `free(__t28)` anywhere. There is currently no
liveness/escape analysis for closure envs, so the emitter cannot know when the
env dies and conservatively never frees it. This is the same "process-lifetime
closures are intentionally leaked" posture the interpreter takes (see the ASan
policy in `CLAUDE.md`), but here it applies to *compiled* code in arbitrary hot
paths, not just process-lifetime registrations.

This is **not** specific to effect handlers -- the handler body is just a
convenient trigger because it is a `fn`-bearing context that runs repeatedly.
Any `(fn ...)` with captures in a loop reproduces it.

## Proposed fix directions

1. **Escape analysis + scoped free (preferred).** When a closure provably does
   not escape its defining lexical scope (not returned, not stored into a
   longer-lived structure, not passed to a function that retains it), emit a
   `free(env)` at the end of that scope. This is the same class of analysis the
   borrow checker already runs; it could reuse that machinery.
2. **Reference-counted closure envs.** Give each env a refcount and a `drop`;
   `resume`/handler-table teardown and normal scope exit decrement. Heavier,
   but composes with closures that genuinely escape (e.g. handler *values*).
3. **Arena per handler activation.** For the effect-handler case specifically,
   allocate handler-body closures from an arena tied to the
   `TurEffectCaptureCtx` and free the arena when the fiber is done (next to the
   existing `body_env` free). Narrower, but kills the repeated-invocation leak
   without a general escape analysis.

## How to validate a fix

- The repro above must report `0 byte(s) leaked` under
  `ASAN_OPTIONS=detect_leaks=1`.
- Add a fixture under `tests/fixtures/` that builds a capturing closure inside
  a loop and assert no leak; to exercise it you must compile the *fixture
  binary* with ASan (the default suite toolchain does not), e.g. a dedicated
  ctest target with `TUR_CC_FLAGS=-fsanitize=address` + `detect_leaks=1`,
  rather than relying on `run.sh`.
- Confirm escaping closures (returned from a function, stored in a handler
  *value*) are *not* freed early -- those must still survive.

## Context

Found while executing Phase A4 (effect-handler closure capture) of
`docs/upcoming/stdlib-type-erasure-cleanup-plan.md`. The A4 capture machinery
(threading captured outer vars through the handler env) is sound and ASan-clean
for value/struct/nested captures; this leak is the orthogonal, pre-existing
fat-closure-env lifetime gap that the same ASan sweep surfaced.

## Resolution

Implemented direction (1): **escape analysis + scoped free**, in the codegen
emitter.

- `closure_binding_escapes` (`src/compiler/emit_core.c`) is a sound, conservative
  walk of a let body: it returns true the moment the bound closure value is used
  as anything other than the callee of a direct call `(b ...)` -- passed as an
  argument, stored, returned, borrowed, or captured by a nested closure (detected
  via that closure's precomputed capture set). Any unmodeled Expr kind, and any
  `perform` (a continuation-capture point that a multi-shot handler could replay
  over the free), conservatively reports an escape. The analysis only ever
  greenlights a free, so a false negative preserves the status-quo leak; it never
  frees a still-live env.
- `emit_let_value` (`src/compiler/emit_expr.c`) consults it via
  `let_binding_env_freeable`: a let binding whose initializer is an `EX_CLOSURE`,
  whose result is a scalar (so the return value can never alias the env), and
  which provably does not escape the body or any sibling initializer, gets a
  `free((void *)(intptr_t)<name>)` emitted at scope exit, after the body (its last
  use). Only the clean, non-`return`/`throw` paths emit the free (an early return
  would skip it -- a leak, not a UAF).

This frees the env at the end of its defining scope for the common non-escaping
case (loop bodies, repeatedly-invoked handler bodies) while leaving genuinely
escaping closures (returned, stored, passed to a retaining call) untouched.

### Validation

- The minimal repro above reports `0 byte(s) leaked` under
  `ASAN_OPTIONS=detect_leaks=1` and still prints `202`.
- `tests/fixtures/closure-env-no-leak/` builds a capturing closure both in a
  1000-iteration loop and in a twice-invoked effect handler; the dedicated ctest
  target `tur_closure_env_leak` (`tests/run-closure-env-leak.sh`) compiles the
  emitted C with `-fsanitize=address,undefined` + `detect_leaks=1` and asserts a
  leak-clean, correct run. The fixture carries `requires.dedicated-runner` so the
  non-sanitized `tests/run.sh` PASS-skips it.
- Escaping closures are confirmed *not* freed early (returned closure, closure
  passed as an argument): both emit zero scoped frees.
- Full suite green (`summary: 1458 passed, 0 failed`); the two snapshots that
  legitimately gained a scoped free (`define-in-fn`, `letrec-shadows-outer`) were
  regenerated and ASan-verified.

### Known conservative limits (safe, leave as status quo)

- Only scalar-returning, let-bound closures are freed. Closures returning
  structs/strings/pointers, or bound by other forms (e.g. internal `define`),
  still leak; broadening to non-reference results is sound future work.
- A let body containing a `perform` (or any unmodeled control form) disables the
  free for that scope to stay safe under multi-shot resume.
