/**
 * local_fiber.h -- local fiber driver on top of tur/reactor (Phase F1-F7)
 *
 * A LocalFiberGroup runs a flat bag of cooperative fibers on top of a single
 * TurReactor instance, without booting (or even touching) the global
 * work-stealing scheduler in scheduler.c. It is the C surface behind the
 * stdlib `reactor-run-fibers` / `local-spawn` / `local-park-*` bindings.
 *
 * Reuse of existing primitives:
 *   - Fiber context-switch + stack management is the TurFiber abstraction in
 *     fiber.c / fiber_ctx_*.S (the same stackful-coroutine primitive the
 *     global scheduler builds on). This header adds only the *driver*: a
 *     ready-queue pump plus a park/unpark bridge onto reactor sources.
 *   - Park/unpark maps onto tur_reactor_add_fd / tur_reactor_add_chan /
 *     tur_reactor_add_timer + tur_reactor_remove (one-shot semantics).
 *
 * Threading model: identical to TurReactor. A LocalFiberGroup, its bound
 * reactor, and every fiber spawned into it live on one OS thread and are NOT
 * thread-safe. Cross-thread handoff goes through channels + tur_reactor_wake.
 *
 * Fiber body calling convention: the body is a Turmeric fat-closure with
 * signature (user:ptr<void>) -> nil, passed as an int64_t fat pointer:
 *   int64_t *fat = (int64_t *)(intptr_t)tur_body;
 *   ((fn1_t)(intptr_t)fat[0])((void *)fat, user_data);
 */

#ifndef TUR_LOCAL_FIBER_H
#define TUR_LOCAL_FIBER_H

#include <stdint.h>

/**
 * Create a fiber group bound to reactor `r` (a TurReactor *). The group
 * borrows `r`; the caller must not free the reactor while the group is alive.
 * Returns an opaque LocalFiberGroup * (>0) or NULL on error.
 */
void *tur_local_fiber_group_new(void *r);

/**
 * Free the group. Any fiber still parked is cancelled: its park source is
 * removed from the bound reactor and its stack is freed (no unwinding /
 * cleanup hooks are run). Safe on NULL.
 */
void tur_local_fiber_group_free(void *group);

/**
 * Spawn a fiber into the group. Returns a fiber id (>= 0) or -1 on error.
 * The fiber first runs on the next tur_reactor_run_fibers tick.
 *
 * tur_body  -- Turmeric fat-closure (user:ptr<void>) -> nil
 * user_data -- forwarded to the body as its only argument
 */
int64_t tur_local_spawn(void *group, int64_t tur_body, void *user_data);

/**
 * Relinquish group ownership of a fiber's spawn-body box, so the group does NOT
 * free it at tur_local_fiber_group_free. tur_local_spawn owns the body box by
 * default; a caller that reuses one body box across many fibers and frees it
 * itself must disown every such fiber. No-op for an unknown fiber id.
 */
void tur_local_disown_body(void *group, int64_t fiber_id);

/**
 * Drive the bound reactor and pump the group's ready-queue until either the
 * group is empty AND the reactor has no remaining sources, or
 * tur_reactor_stop was called on the bound reactor. Returns the number of
 * fibers that ran to completion.
 *
 * Calling this re-entrantly from inside a fiber of the same group is a hard
 * error (returns -1 immediately) -- it would deadlock the pump.
 */
int64_t tur_reactor_run_fibers(void *group);

/**
 * Park the currently-running fiber on `fd` waiting for `events` (a READ/WRITE
 * bitmask). Resumes when the reactor fires the source; the source is removed
 * before the fiber runs again (one-shot). If timeout_ms >= 0 and the timeout
 * elapses first, the fiber resumes with the sentinel -2.
 *
 * Must be called from inside a fiber spawned into `group`. Outside of one,
 * returns -1 and leaves the caller running.
 *
 * Returns: the fired event mask (>= 0), -2 on timeout, or -1 if not in a
 * fiber of this group.
 */
int64_t tur_local_park_fd(void *group, int64_t fd, int64_t events,
                          int64_t timeout_ms);

/**
 * Park the currently-running fiber on channel `chan` (a ChanBlock *), resuming
 * with the received value when one is available. Same in-fiber requirement as
 * tur_local_park_fd. Returns 0 and stores the value in *value_out, or -1 (not
 * in a fiber of this group, or the watch could not be registered) and leaves
 * *value_out alone. This is what stdlib local-park-chan calls.
 */
int tur_local_park_chan_status(void *group, void *chan, int64_t *value_out);

/**
 * The single-return form: the received value, or -1 if not in a fiber. A
 * received -1 is indistinguishable from the error; prefer
 * tur_local_park_chan_status.
 */
int64_t tur_local_park_chan(void *group, void *chan);

#endif /* TUR_LOCAL_FIBER_H */
