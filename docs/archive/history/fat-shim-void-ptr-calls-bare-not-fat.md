---
title: ^fat let-binding of runtime ptr<void> generates wrong shim -- calls fat closure as bare one-arg function
category: Reported
severity: high
description: When a runtime ptr<void> value (an already-heap-allocated fat closure) is bound via a `^fat` let-binding, the compiler wraps it in a `__tur_fatshim_void___void__` shim box. That shim reads slot 1 of the box as a bare `void *(*)(void *)` function and calls it with one argument -- but the value in slot 1 IS a fat closure whose thunk expects two arguments (env + arg). The result is a segfault at the call site of the composed closure. This blocks `>>>` from being usable with `ptr<void>->ptr<void>` Signal Functions (SFs) in the tur-signal spice.
---

# `^fat` let-binding of runtime `ptr<void>` generates wrong shim

## Summary

**Severity: High.** `__tur_fatshim_void___void__` dispatches the wrapped
value as a bare one-arg C function. When the wrapped value is itself a fat
closure (two-arg thunk), the call passes only one argument, corrupting the
stack and segfaulting. Blocks Phase 5 `effects-chain` from using `>>>` to
compose `ptr<void>->ptr<void>` Signal Functions.

## Minimal repro

```turmeric
(load "stdlib/arrow.tur")

(defn make-scale [k : float] : ptr<void>
  (fn [^fat sig : (fn [float] #{} float)] : ptr<void>
    (fn [t : float] : float (* k (sig t)))))

(defn constant [v : float] : ptr<void>
  (fn [t : float] : float v))

(defn main [] : int
  ;; Both sf1 and sf2 are real fat closures from make-scale.
  ;; Binding via ^fat let generates a shim. >>> stores the shim.
  ;; When the composed closure dispatches through the shim, it calls
  ;; make-scale's thunk with one arg instead of two -> segfault.
  (let [^fat sf1 : (fn [ptr<void>] #{} ptr<void>)      (make-scale 2.0)
        ^fat sf2 : (fn [ptr<void>] #{} ptr<void>)      (make-scale 3.0)
        ^fat composed : (fn [ptr<void>] #{} ptr<void>) (>>> sf1 sf2)
        ^fat input : (fn [float] #{} float)            (constant 1.0)
        ^fat out : (fn [float] #{} float)              (composed input)]
    (println (out 0.0)))   ;; expected 6.0, actual: Segmentation fault
  0)
```

## Observed vs expected

- **Observed**: Segmentation fault (exit 139) at `(composed input)`.
- **Expected**: `6` printed (2.0 * 3.0 * 1.0).

## Root cause

### What the compiler generates for `^fat sf1 : (fn [ptr<void>] #{} ptr<void>) (make-scale 2.0)`

```c
// make-scale 2.0 returns a fat closure: { thunk_ptr, k=2.0 }
int64_t sf1 = (int64_t)(intptr_t)(make_hyscale(2.0));

// ^fat let-binding creates a shim box:
int64_t *__t80 = (int64_t *)malloc(2 * sizeof(int64_t));
__t80[0] = (int64_t)(intptr_t)__tur_fatshim_void___void__;  // shim fn
__t80[1] = (int64_t)(intptr_t)sf1;                           // real fat closure
void *__t81 = __t80;  // sf1 = shim box, not the original fat closure
```

`>>>` stores the shim boxes as `f` and `g` in its composed closure env.

### What the shim does

```c
static void * __tur_fatshim_void___void__(void *__e, void * a0) {
    return ((void * (*)(void *))(intptr_t)((int64_t *)__e)[1])(a0);
    //                                                    ^^^^
    //      reads slot 1 (= real fat closure ptr) and calls it
    //      as a bare ONE-arg function: real_fn(a0)
}
```

The real fat closure (`make-scale`'s outer thunk) has signature:
```c
static void * __fn_1139(void * env, int64_t sig) { ... }  // TWO args
```

The shim passes only `a0` (one arg), so `env` is garbage → segfault.

### Why `__apply-sf` does NOT hit this bug

`__apply-sf` is defined with a `^fat` **parameter** (not a let-binding). At
call sites that pass a runtime `ptr<void>` value, the compiler does NOT create
a shim -- the value is passed as `int64_t` directly. The generated dispatch:

```c
static void * _un_unapply_hysf(int64_t sf, int64_t sig) {
    return (*( tur_thunk_void___void___t *)((void *)(intptr_t)(sf)))
           ((void *)(intptr_t)(sf), (void *)(intptr_t)(sig));
    // reads thunk from sf[0] and calls thunk(sf, sig) -- correct fat dispatch
}
```

`sf` IS the real fat closure, so `sf[0]` is the thunk pointer and the two-arg
call is correct. No shim is involved.

The asymmetry: `^fat` **parameter** annotations on function parameters do not
inject shims at call sites for runtime `ptr<void>` values. `^fat` **let
bindings** do inject shims. The shim is correct for NAMED captureless
functions (where slot 1 IS a bare one-arg C function), but wrong for runtime
fat closures (where slot 1 is a fat-closure pointer whose thunk is two-arg).

## Fix directions

**Direction 1 (compiler): Fat-passthrough shim for `ptr<void>` values.**
When generating a shim for a `^fat` let-binding of a runtime `ptr<void>`
value (not a named function reference), emit a fat-passthrough shim instead:

```c
static void * __tur_fat_passthrough_void___void__(void *__e, void * a0) {
    void * fat = (void *)(intptr_t)((int64_t *)__e)[1];
    return (*( tur_thunk_void___void___t *)(fat))(fat, a0);
    //     reads thunk from the real fat closure, calls thunk(fat, a0) -- correct
}
```

This requires the compiler to distinguish shim generation for named-function
references (current behavior, correct) from shim generation for runtime
`ptr<void>` values (needs the passthrough variant).

**Direction 2 (compiler): Don't shim runtime `ptr<void>` values.**
For a `^fat` let-binding where the RHS is a function-call expression (not a
named function literal), the result is already a fat closure. Skip the shim
and use the value directly. This is a compile-time heuristic: any expression
that isn't a bare named-function reference can be assumed to return a fat box.

**Direction 3 (workaround): Avoid `^fat` let-bindings of runtime `ptr<void>`.**
Pass `ptr<void>` values directly to functions that take `^fat` parameters.
The shim is NOT generated at call sites for `^fat` parameters -- only at
`^fat` let-binding sites. So:

```turmeric
;; Broken (^fat let-binding creates wrong shim):
(let [^fat sf : (fn [ptr<void>] #{} ptr<void>) (vec-get effects i)]
  (>>> acc sf))

;; Fine (^fat parameter -- no shim at call site):
(defn compose-step [^fat acc : (fn [ptr<void>] #{} ptr<void>)
                    ^fat sf  : (fn [ptr<void>] #{} ptr<void>)] : ptr<void>
  (>>> acc sf))
```

BUT: this doesn't actually help because `>>>` itself needs to call `acc` and
`sf` later, and it stores them as captured `int64_t` values -- the shim issue
re-appears when `>>>`'s inner closure dispatches through them.

Direction 1 or 2 is the real fix.

## Impact on tur-signal Phase 5

`compose.tur`'s `effects-chain` was intended to use `>>>` to fold a vec of SFs:

```turmeric
;; Intended Phase 5 shape (blocked):
(defn __sf-fold [^fat acc ...] ...
  (let [^fat sf : (fn [ptr<void>] #{} ptr<void>) (vec-get effects i)]
    (__sf-fold (>>> acc sf) ...)))
```

This segfaults because `sf` is a fat closure from `vec-get`, and the `^fat`
let-binding wraps it in the wrong shim before passing it to `>>>`.

**Workaround in effect**: `compose.tur` uses a correct `__chain-loop` +
`__apply-sf` approach that dispatches directly through the fat closure
protocol (no shim involved). This is correct and produces right results.
The `>>>` fold shape is deferred until this compiler gap is resolved.

## Validation

- Confirmed working: `tur run tests/fixtures/arrow-compose-float/` (uses named
  functions `add-05`, `scale-2` as `^fat` -- shim is correct for bare fns).
- Confirmed broken: the repro above segfaults with `^fat` let-binding of a
  runtime `ptr<void>` fat closure.
- Confirmed working: `__apply-sf` pattern avoids shims by using `^fat`
  parameters (not let-bindings), dispatching directly through `sf[0]`.

## Cross-references

- [[tur-signal-rebuild-plan]] Phase 5 -- blocked by this gap.
- `tests/fixtures/sf-compose-typed` -- exercises `>>>` with named float fns
  (works; not affected by this gap).
- `stdlib/arrow.tur` `>>>` -- the polymorphic compositor that triggers this
  when used with ptr<void>->ptr<void> SFs.
