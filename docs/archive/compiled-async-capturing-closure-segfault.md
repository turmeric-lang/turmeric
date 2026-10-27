# Compiled `(async <capturing-closure>)` segfaults (env pointer called as code)

**Status:** RESOLVED (2026-07-13). A boxed closure now spawns via an env-taking
async helper instead of being called as a bare function pointer -- see the
Resolution section at the end.

**Severity:** medium (silent SIGSEGV, no diagnostic; the interpreter handles the
same program correctly). Pre-existing on the compiled path; **independent of
`--enable=cps-async`** (the async lowering is shared -- the crash reproduces
identically with and without the flag).

## Summary

`(async (fn [] ...))` where the lambda **captures** an outer variable compiles to
code that calls the closure's *environment pointer* as if it were a bare function
pointer, dereferencing data as code -> SIGSEGV at the first call, at any depth.
A non-capturing async lambda works.

## Minimal repro

```turmeric
;; crashes (captures x):
(defn f [x : int] : int
  (await (async (fn [] : int (+ x 1)))))
(defn main [] : nil (println (f 41)))     ;; expected 42; actual: SIGSEGV (rc 139)

;; works (no capture):
(defn g [] : int
  (await (async (fn [] : int 42))))
(defn main [] : nil (println (g)))         ;; 42
```

`tur build cap.tur` and `tur build --enable=cps-async cap.tur` both crash;
`tur --interpret cap.tur` prints `42`.

## Root cause

`EX_ASYNC` path (a) in `src/compiler/emit_expr.c` (~5804-5809) fires when the
`fn_expr` has type `TY_FN` and passes it straight to `tur_async_fiber` as a bare
function pointer:

```c
buf_printf(body, "void *%s = (void *)tur_async_fiber((int64_t(*)(void))(intptr_t)%s);\n", tmp, fn_val);
```

But a *capturing* lambda is a **fat closure** -- an env struct plus a code
pointer -- and `fn_val` is the **env pointer**, not a function pointer. The
emitted `f__cps` shows it directly:

```c
struct __env_1271 *__t174 = malloc(sizeof(struct __env_1271));
__t174->__fn = (tur_thunk_int64_t_t)__fn_1269;
__t174->x = x;
void *__t175 = __t174;                                   /* the ENV pointer */
tur_async_fiber((int64_t(*)(void))(intptr_t)__t175);     /* calls env as code -> crash */
```

`tur_async_fiber` then does `int64_t result = fn();`, jumping to the first bytes
of the `__env_1271` struct.

A captureless lambda decays to a plain function pointer (no env), so the cast is
accidentally correct and path (a) works -- which is why `g` above is fine.

## Impact on the archived async repros (F3.4 context)

The archived reports `turi-async-fiber-stack-never-reclaimed` and
`turi-async-await-deep-recursion-garbage` both use

```turmeric
(defn a-rec [n : int] : int
  (if (= n 0) 0 (+ 1 (await (async (fn [] : int (a-rec (- n 1))))))))
```

whose async lambda captures `n`. On the compiled path this hits *this* bug and
SIGSEGVs at every depth (including 1), with and without `--enable=cps-async`, so
the repro cannot exercise the deep-recursion representation on the compiled path.
The interpreter (where both bugs were filed and resolved) returns correct values
at depth. This crash is orthogonal to F3 and is not a cps-async regression.

## Fix directions

- Detect a fat closure at the `EX_ASYNC` site (captures present / value is a
  closure env, not a bare `cfnptr`). Either:
  - route it through a proper closure-invoking shim -- give `tur_async_fiber` an
    env-taking variant `tur_async_fiber_env(int64_t (*fn)(void*), void *env)` that
    does `fn(env)`, and pass the closure's `__fn` + env; or
  - emit a compile-time `TUR-E` ("async lambda may not capture; lift captured
    values into parameters") until env-carrying async closures are supported,
    matching the documented v1 limitation instead of crashing at runtime.
- Path (b) (the expression thunk) already refuses captures by construction; the
  gap is that a `(fn ...)` *literal* takes path (a), which silently miscompiles
  a capturing closure rather than diagnosing it.

## Resolution (2026-07-13)

Implemented the env-taking spawn (the first fix direction).

**Changes**

- `src/compiler/emit_module.c` -- new runtime helper `tur_async_fiber_closure(void
  *clos)` emitted in the async preamble beside `tur_async_fiber`. It reads the
  thunk out of the box's slot 0 (`int64_t (*__fn)(void *) = *(int64_t (**)(void
  *))clos;`) and invokes it with the box as its env (`__fn(clos)`), then applies
  the same future / F3.2 suspend handling as `tur_async_fiber`.
- `src/compiler/emit_expr.c` (`EX_ASYNC` path a) -- when the async fn value is a
  FAT closure (`fn_expr->type.as.fn.boxed`), route to `tur_async_fiber_closure`;
  a THIN (bare) fn still spawns via `tur_async_fiber`. The `boxed` gate catches
  both a closure literal `(async (fn [] captures))` and a let-bound closure
  `(async c)`.

**Why slot 0 is the thunk.** An `EX_CLOSURE` box is laid out `{ __fn; captures...
}` with `__fn` first (emit_expr.c EX_CLOSURE: "fat closure struct -- __fn first,
then captures"), and the thunk's C signature takes the box as its `void *env`
first parameter, so `*(fn**)clos` is the thunk and `__fn(clos)` runs the body
with its captures in scope. The helper reads slot 0 generically (no dependency on
the per-closure `tur_thunk_*` typedef), so it lives in the fixed preamble.

**Tests.** `tests/fixtures/async-capturing-closure` (default/fiber path) and
`tests/fixtures/async-capturing-closure-cps` (`--enable=cps-async`) both exercise
a closure literal, a let-bound closure, and the recursive capturing async (the
archived `a-rec` shape) -> `42 / 42 / 100`. The archived `a-rec` repro now runs
correctly on the compiled path at moderate depth (it is C-stack-bounded because
`(async fn)` is synchronous -- the gap-1 finding -- not crash-at-any-depth as
before). Full `bash tests/run.sh`: **2115 passed, 0 failed**; 139 snapshots
regenerated for the new helper.

Not pursued: the compile-time-diagnostic alternative -- supporting the shape is
strictly better than rejecting it, and the env-taking spawn is a small, local
change.

## Status

RESOLVED. Filed during F3.4 sign-off; fixed 2026-07-13.
