# Thread Cancellation

**Status:** Not started. Deferred from `select-fair-blocking-plan.md` (Open Question 3).

**Prerequisites:** Phase SEL0--SEL2 (fair blocking `select`) must be complete
before this plan is executed. The waiter deregistration path described here
depends on `tur_select_blocking` and the `TurSelectWaiter` infrastructure.

**Last updated:** 2026-05-16

---

## Summary

This document plans cooperative thread cancellation for Turmeric. The primary
motivating constraint is that threads sleeping inside `tur_select_blocking` hold
registered waiters in one or more channel waiter lists. If a thread is cancelled
without cleaning up those waiters, dangling pointers remain in the channel lists
-- a guaranteed use-after-free on the next send or receive.

---

## Motivation

### The Problem

`tur_select_blocking` registers stack-allocated `TurSelectWaiter` nodes in each
channel's `recv_waiters` or `send_waiters` list, then sleeps on a
`pthread_cond_t`. If the sleeping thread is cancelled mid-wait:

- Its stack frame is gone.
- The waiter nodes it registered are now dangling pointers inside live channels.
- The next `tur_chan_send` or `tur_chan_recv` that walks the waiter list will
  read freed memory.

### Goals

- A cancelled thread always deregisters its waiters before its stack is
  reclaimed.
- Cancellation is **cooperative** -- the thread reaches a defined cancellation
  point and unwinds cleanly.
- No cancellation-point overhead on threads that are never cancelled.
- Compatible with the existing `pthread_mutex_t` / `pthread_cond_t` channel
  locking model.

---

## Design

### Cooperative Cancellation via a Cancel Flag

Each thread carries a per-thread cancel flag:

```c
typedef struct {
    volatile int  cancel_requested; /* set by canceller thread */
    pthread_mutex_t cancel_mutex;
    pthread_cond_t  cancel_cond;
} TurThreadState;
```

Cancellation is requested by setting `cancel_requested = 1` and signalling
`cancel_cond`. The sleeping thread checks the flag at each wakeup from
`pthread_cond_wait` and, if set, runs its cleanup path before returning.

### Cancellation Point in `tur_select_blocking`

The sleep loop inside `tur_select_blocking` becomes:

```c
while (*selected_idx == -1 && !tls->cancel_requested) {
    pthread_cond_wait(&wakeup_cond, &wakeup_mutex);
}

if (*selected_idx == -1) {
    /* cancelled -- deregister all waiters and return error */
    for (int i = 0; i < n; i++)
        tur_waiter_remove(clauses[i].chan, &waiters[i]);
    return TUR_SELECT_CANCELLED;
}
```

This guarantees that even on cancellation the waiter list is cleaned up under
the correct locks before the stack frame is released.

### Other Cancellation Points

Thread cancellation should be checkable at any long-running or blocking
operation:

| Site | Cancellation point? | Notes |
|---|---|---|
| `tur_select_blocking` | Yes | Primary motivation for this plan |
| `tur_chan_recv` (blocking) | Yes | Simple: check flag after `pthread_cond_wait` |
| `tur_chan_send` (blocking) | Yes | Symmetric with recv |
| `tur_sleep` / timer waits | Yes | Add cancel check to wait loop |
| Pure computation | No | Cooperative -- user must yield explicitly |

### Turmeric Surface Syntax

```turmeric
;;; cancel-thread -- request cooperative cancellation of a thread handle.
;;;
;;; Parameters:
;;;   t -- a thread handle returned by (thread ...)
;;;
;;; Returns:
;;;   :void
;;;
;;; Example:
;;;   (let [t (thread (fn [] (long-running-task)))]
;;;     (cancel-thread t)
;;;     (join-thread t))
;;;
;;; Since: Phase TC0
(defn cancel-thread [t] : void ...)

;;; cancelled? -- return true if the current thread has been cancelled.
;;;
;;; Returns:
;;;   bool -- true if a cancellation has been requested
;;;
;;; Example:
;;;   (when (cancelled?) (cleanup-and-return))
;;;
;;; Since: Phase TC0
(defn cancelled? [] : bool ...)
```

---

## Phases

### Phase TC0 -- Per-Thread Cancel State

**Goal:** Add cancel flag infrastructure without exposing it to user code.

**Tasks:**
- [x] Define `TurThreadState` (cancel flag + condvar) in `src/runtime/thread.h`
- [x] Allocate and attach `TurThreadState` to each spawned thread (thread-local
      storage or a handle table)
- [x] Implement `tur_thread_cancel(handle)`: set flag, signal cancel condvar
- [x] Implement `tur_thread_cancelled()`: read flag for current thread
- [x] Verify all existing threading fixture tests pass (`just test`)

**Exit Criterion:** No regressions; cancel state exists but is never set.


### Phase TC1 -- Cancellation Points in Blocking Primitives

**Goal:** Wire cancel checks into `tur_select_blocking`, `tur_chan_recv`, and
`tur_chan_send`.

**Tasks:**
- [x] Update `tur_select_blocking` sleep loop to check cancel flag; deregister
      all waiters and return `TUR_SELECT_CANCELLED` on cancellation
- [x] Update blocking `tur_chan_recv` to return an error value on cancellation
- [x] Update blocking `tur_chan_send` to return an error value on cancellation
- [x] Propagate cancellation via `tur_thread_do_cancel()` (longjmp to cancel
      guard if active, else `pthread_exit(NULL)`); `with-cancel-guard` uses
      setjmp/longjmp for opt-in cleanup
- [x] Implement `with-cancel-guard` in `stdlib/thread.tur`:
      saves/restores jmpbuf state; calls cleanup thunk on cancel longjmp
- [x] Add fixture `tests/fixtures/cancel-select/`:
      - Thread sleeps in a `select` with no immediately ready channels
      - Main thread cancels it; assert clean exit, no sanitizer errors
- [x] Add fixture `tests/fixtures/cancel-chan/`:
      - Thread blocks on `tur_chan_recv`; main thread cancels it
      - Assert clean exit

**Exit Criterion:** Cancel fixtures pass; no data races under TSan.


### Phase TC2 -- User-Facing API and Cooperative Yield

**Goal:** Expose `cancel-thread` and `cancelled?` to Turmeric code.

**Tasks:**
- [x] Implement `cancel-thread` and `cancelled?` in `stdlib/thread.tur`
- [x] Add docstrings (full format per CLAUDE.md standard)
- [x] Add fixture `tests/fixtures/cancel-cooperative/`:
      - Long-running computation checks `(cancelled?)` in its loop
      - Main thread cancels it mid-run; assert it exits at the next yield point
- [ ] Update `docs/guides/threading-guide.md` with cancellation semantics
- [ ] Update `CHANGELOG` / release notes

**Exit Criterion:** Cooperative cancel fixture passes; API documented.

---

## Open Questions

1. **Cancellation vs. error propagation:** ~~Should a cancelled blocking call
   panic (unwind) or return an `:err` value?~~ **Resolved: Option C -- result
   return with `result-must` default and opt-in `with-cancel-guard`.**

   Three approaches were considered:

   **Option A -- Panic:** cancelled blocking calls terminate the thread
   immediately via `tur_panic`. No return value; no cleanup possible.

   ```turmeric
   ;; No caller changes needed. Cleanup after chan-recv never runs.
   (defn drain-loop [ch]
     (let [v (chan-recv ch)]   ; panics if cancelled mid-wait
       (process v)
       (drain-loop ch)))

   (let [t (thread-spawn-fn drain-loop ch)]
     (cancel-thread t)
     (thread-join t))          ; returns once panic unwinds the thread
   ```

   *Pro:* simplest implementation; no call-site boilerplate.
   *Con:* cleanup code (releasing resources, closing files) can never run.

   **Option B -- `:err` result everywhere:** `chan-recv` and `select` return
   `(ok value)` on success or `(err 0)` on cancellation. Every call site must
   handle the error branch.

   ```turmeric
   (defn drain-loop [ch]
     (let [r (chan-recv ch)]   ; returns (ok v) or (err 0) if cancelled
       (cond
         (ok?  r) (do (process (ok-val r))
                      (drain-loop ch))
         (err? r) (do (cleanup)   ; runs on cancellation
                      nil))))
   ```

   *Pro:* cleanup is possible; composable with existing `result.tur`.
   *Con:* significant boilerplate -- most threads are never cancelled, so every
   `chan-recv` / `select` call site pays the handling cost unconditionally.

   **Option C -- result return + `result-must` default + opt-in
   `with-cancel-guard` (chosen):** blocking calls return a result. Callers that
   don't need cleanup use `result-must` (same ergonomics as panic, zero
   boilerplate). Callers that need cleanup opt in with `with-cancel-guard`.

   ```turmeric
   ;; Simple caller -- no boilerplate; panics on cancellation like Option A:
   (defn drain-loop [ch]
     (let [v (result-must (chan-recv ch))]  ; panics if cancelled
       (process v)
       (drain-loop ch)))

   ;; Caller that needs cleanup -- opts in explicitly:
   (defn drain-loop-safe [ch resource]
     (with-cancel-guard
       (fn [] (release resource))           ; cleanup thunk
       (fn []
         (let [v (result-must (chan-recv ch))]
           (process v)
           (drain-loop-safe ch resource)))))
   ```

   *Pro:* safe path (no cleanup needed) is as ergonomic as Option A; cleanup
   is available when needed; `result-must` already exists in `result.tur` so
   no new primitives are required for the common case.
   *Con:* `with-cancel-guard` is a new macro; call sites that forget
   `result-must` silently hold an unhandled `(err 0)` result.

2. **`join-thread` on a cancelled thread:** **Resolved: Option A -- always
   block until the thread exits.**

   `cancel-thread` sets the cancel flag; `join-thread` calls `pthread_join`
   and waits for the thread to reach a cancellation point, unwind, and return.
   The thread's stack is guaranteed to be gone by the time `join-thread`
   returns.

   ```turmeric
   (cancel-thread t)
   (join-thread t)   ; blocks until thread reaches a cancel point and exits
   ```

   Option B (return immediately if cancel was requested) was rejected: it
   leaves the thread potentially still running with no way to know when it has
   actually stopped, making resource reclamation unsafe.

3. **Cancellation of non-blocking code:** **Resolved: Option B -- provide
   `yield-point` helper, documented in detail.**

   `(cancelled?)` is the only mechanism for pure computation to observe a
   cancel request. Rather than leaving callers to write the check manually,
   a `yield-point` macro is provided that makes the correct pattern idiomatic.

   ### What `yield-point` does

   ```turmeric
   ;;; yield-point -- check for a pending cancellation and exit if one is set.
   ;;;
   ;;; Call this inside any CPU-bound loop that must respond to cancel-thread.
   ;;; Blocking operations (chan-recv, chan-send, select) check automatically;
   ;;; pure computation loops must call yield-point explicitly.
   ;;;
   ;;; Behaviour:
   ;;;   1. Reads the cancel flag for the current thread (cheap atomic read).
   ;;;   2. If not set, returns immediately with no side effects.
   ;;;   3. If set, behaves identically to result-must applied to (err 0):
   ;;;      calls tur_panic("cancelled") and unwinds the thread.
   ;;;      If the call is wrapped in with-cancel-guard, the cleanup thunk
   ;;;      runs before the thread exits (same as a cancelled chan-recv).
   ;;;
   ;;; Returns:
   ;;;   :void (never returns if the thread has been cancelled)
   ;;;
   ;;; Example:
   ;;;   (defn crunch [n]
   ;;;     (let [i 0]
   ;;;       (while (< i n)
   ;;;         (yield-point)        ; responds to cancel-thread
   ;;;         (do-work i)
   ;;;         (set! i (+ i 1)))))
   ;;;
   ;;; Since: Phase TC2
   (defmacro yield-point []
     `(when (cancelled?) (tur_panic "cancelled")))
   ```

   **Why this pattern instead of `(when (cancelled?) ...)`:**
   - The check + action is a single token, making it easy to audit loops for
     cancel-safety at a glance.
   - It integrates with `with-cancel-guard` automatically: if the loop body
     is already inside a `with-cancel-guard`, the cleanup thunk runs on the
     panic, exactly as it would for a cancelled `chan-recv`.
   - It is a macro (zero-cost abstraction): no heap allocation, no function
     call overhead when the cancel flag is not set.

   **What `yield-point` does NOT do:**
   - It does not yield the OS thread scheduler (`sched_yield`). It only checks
     the cancel flag. CPU-bound threads that call `yield-point` in a tight loop
     will still consume a full core; they will merely stop promptly when
     cancelled.
   - It does not check the cancel flag on behalf of child threads. Each thread
     must call `yield-point` (or reach a blocking primitive) in its own loop.

   **How often to call `yield-point`:**
   - Once per loop iteration is the standard recommendation. Calling it more
     often wastes cycles; calling it less often increases the latency between
     `cancel-thread` and the thread actually stopping.
   - For very tight numeric loops where even one check per iteration is
     measurable, call it every N iterations (e.g., every 1000) and document
     the tradeoff.

   **Tasks added to TC2:**
   - [ ] Implement `yield-point` macro in `stdlib/thread.tur`
   - [ ] Add full docstring (as above)
   - [ ] Update `docs/guides/threading-guide.md` with a "cancel-safe loops"
         section covering `yield-point`, call frequency, and the
         `with-cancel-guard` interaction

4. **WASM:** Emscripten's pthread cancel support is limited. **Partially
   resolved:** `docs/wasm-threads-plan.md` Phases WT0--WT2 are complete; the
   WASM build now uses `-pthread` with lazy pool growth and the eval runs in a
   dedicated Worker. Phase WT3 (verifying cooperative cancel in the browser
   REPL) remains open pending TC0--TC2 implementation.
   The cooperative cancel flag design is unaffected by Emscripten's lack of
   `pthread_cancel`.

---

## Related Work

| System | Cancellation model |
|---|---|
| POSIX pthreads | `pthread_cancel` + deferred/async cancel points |
| Go | `context.Context` passed explicitly; `ctx.Done()` channel |
| Kotlin coroutines | Cooperative via `isActive` / `ensureActive()` |
| Erlang | `exit/2` signal; processes handle or propagate |

---

## Summary

**Recommendation:** Implement cooperative cancellation in phases TC0--TC2 after
SEL0--SEL2 are complete. The `tur_select_blocking` cleanup path (Phase TC1) is
the primary correctness requirement driving this work.

**Next step:** Begin TC0 after SEL2 is closed.
