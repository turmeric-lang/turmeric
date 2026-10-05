/* Phase S7: Async scheduler, future management, timers, and non-blocking I/O
 * for the Turmeric eval runtime (libturi).
 *
 * This file implements the cooperative fiber scheduler and future primitives.
 * The fiber thunk (the function that runs inside each async fiber) is in
 * eval.c because it needs to call eval_expr directly.
 */

/* Platform macros before system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "fiber.h"
#include "env.h"
#include "eval.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* Windows: mmap is shimmed over VirtualAlloc, but <poll.h> has no counterpart.
 * WSAPoll only accepts SOCKETs, and the descriptors here are CRT file
 * descriptors, so there is nothing honest to map poll() onto -- the pending-I/O
 * scan below compiles out.  Consistent with io_iocp.c, which cannot register an
 * fd in the first place; interpreter async I/O on Windows is WIN3. */
#  include "platform_mman.h"
#  include <fcntl.h>
#  include <unistd.h>
#elif !defined(__EMSCRIPTEN__)
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/mman.h>
#  include <unistd.h>
#  ifndef MAP_ANONYMOUS
#    define MAP_ANONYMOUS MAP_ANON
#  endif
#endif

/* -------------------------------------------------------------------------
 * Time utility
 * ---------------------------------------------------------------------- */

uint64_t turi_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

/* -------------------------------------------------------------------------
 * Future management
 * ---------------------------------------------------------------------- */

TuriFuture *turi_future_new(TuriEnv *env) {
    TuriFuture *f = (TuriFuture *)calloc(1, sizeof(TuriFuture));
    f->state        = TURI_FUTURE_PENDING;
    f->result       = turi_nil();
    f->wakers       = NULL;
    f->owner        = NULL;
    f->reported     = false;
    /* Track in env's allocation list for bulk free. */
    f->next_alloc   = env->all_futures;
    env->all_futures = f;
    return f;
}

void turi_future_add_waker(TuriFuture *f, TuriFiber *fiber) {
    TuriWaker *w = (TuriWaker *)malloc(sizeof(TuriWaker));
    w->fiber = fiber;
    w->next  = f->wakers;
    f->wakers = w;
}

/* Internal: move all wakers from f to the ready queue. */
static void flush_wakers(TuriEnv *env, TuriFuture *f) {
    TuriWaker *w = f->wakers;
    f->wakers = NULL;
    while (w) {
        TuriWaker *next = w->next;
        TuriFiber *fiber = w->fiber;
        free(w);
        if (fiber && fiber->state == TURI_FIBER_SUSPENDED) {
            fiber->awaiting_future = NULL;
            fiber->state = TURI_FIBER_READY;
            turi_sched_enqueue(env, fiber);
        }
        w = next;
    }
}

void turi_future_resolve(TuriEnv *env, TuriFuture *f, TuriValue result) {
    if (f->state != TURI_FUTURE_PENDING) return; /* already settled */
    f->state  = TURI_FUTURE_RESOLVED;
    f->result = result;
    flush_wakers(env, f);
}

void turi_future_reject(TuriEnv *env, TuriFuture *f, TuriValue error) {
    if (f->state != TURI_FUTURE_PENDING) return;
    f->state  = TURI_FUTURE_REJECTED;
    f->result = error;
    flush_wakers(env, f);
}

TuriValue turi_future_poll_val(TuriFuture *f) {
    if (!f || f->state == TURI_FUTURE_PENDING) return turi_nil();
    return f->result;
}

/* -------------------------------------------------------------------------
 * Scheduler: ready queue
 * ---------------------------------------------------------------------- */

void turi_sched_enqueue(TuriEnv *env, TuriFiber *fiber) {
    fiber->sched_next = NULL;
    if (!env->sched_ready_tail) {
        env->sched_ready_head = env->sched_ready_tail = fiber;
    } else {
        env->sched_ready_tail->sched_next = fiber;
        env->sched_ready_tail = fiber;
    }
}

static TuriFiber *sched_dequeue(TuriEnv *env) {
    TuriFiber *f = env->sched_ready_head;
    if (!f) return NULL;
    env->sched_ready_head = f->sched_next;
    if (!env->sched_ready_head) env->sched_ready_tail = NULL;
    f->sched_next = NULL;
    return f;
}

/* -------------------------------------------------------------------------
 * Timer management
 * ---------------------------------------------------------------------- */

void turi_timer_add(TuriEnv *env, uint64_t ms, TuriFuture *future) {
    TuriTimer *t = (TuriTimer *)malloc(sizeof(TuriTimer));
    t->deadline_ms = turi_now_ms() + ms;
    t->future      = future;

    /* Insert into sorted list (ascending deadline). */
    TuriTimer **pp = &env->timers_head;
    while (*pp && (*pp)->deadline_ms <= t->deadline_ms)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
}

void turi_timer_cancel(TuriEnv *env, TuriFuture *future) {
    TuriTimer **pp = &env->timers_head;
    while (*pp) {
        if ((*pp)->future == future) {
            TuriTimer *dead = *pp;
            *pp = dead->next;
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

void turi_future_remove_waker(TuriFuture *f, TuriFiber *fiber) {
    TuriWaker **pp = &f->wakers;
    while (*pp) {
        if ((*pp)->fiber == fiber) {
            TuriWaker *dead = *pp;
            *pp = dead->next;
            free(dead);
        } else {
            pp = &(*pp)->next;
        }
    }
}

static void fire_timers(TuriEnv *env) {
    uint64_t now = turi_now_ms();
    while (env->timers_head && env->timers_head->deadline_ms <= now) {
        TuriTimer *t = env->timers_head;
        env->timers_head = t->next;
        turi_future_resolve(env, t->future, turi_nil());
        free(t);
    }
}

static uint64_t next_timer_deadline(TuriEnv *env) {
    return env->timers_head ? env->timers_head->deadline_ms : UINT64_MAX;
}

/* -------------------------------------------------------------------------
 * Non-blocking I/O (S7.7)
 * Not available in WASM — all functions are stubbed out.
 * ---------------------------------------------------------------------- */

#ifndef __EMSCRIPTEN__

void turi_io_read_async(TuriEnv *env, int fd, int n, TuriFuture *future,
                        TuriFiber *fiber) {
    TuriIoPending *p = (TuriIoPending *)malloc(sizeof(TuriIoPending));
    p->fd         = fd;
    p->op         = TURI_IO_READ;
    p->future     = future;
    p->fiber      = fiber;
    /* `n` was range-checked by native_read_async, the only caller. */
    p->read_buf   = (char *)malloc((size_t)(n < 0 ? 0 : n) + 1);
    if (!p->read_buf) {
        fprintf(stderr, "tur: read-async: out of memory\n");
        abort();
    }
    p->read_len   = n < 0 ? 0 : n;
    p->write_buf  = NULL;
    p->write_len  = 0;
    p->write_cap  = 0;
    p->next       = env->io_pending_head;
    env->io_pending_head = p;
    env->io_pending_count++;
}

void turi_io_write_async(TuriEnv *env, int fd, const char *data, int len,
                         TuriFuture *future, TuriFiber *fiber) {
    TuriIoPending *p = (TuriIoPending *)malloc(sizeof(TuriIoPending));
    p->fd         = fd;
    p->op         = TURI_IO_WRITE;
    p->future     = future;
    p->fiber      = fiber;
    p->read_buf   = NULL;
    p->read_len   = 0;
    p->write_buf  = (char *)malloc((size_t)len);
    memcpy(p->write_buf, data, (size_t)len);
    p->write_len  = len;
    p->write_cap  = len;
    p->next       = env->io_pending_head;
    env->io_pending_head = p;
    env->io_pending_count++;
}

static void poll_io(TuriEnv *env, int timeout_ms) {
    if (!env->io_pending_head) return;

#ifdef _WIN32
    /* No poll(2) for CRT file descriptors on Windows -- see the include block
     * at the top of this file.  Nothing can be marked ready, so pending entries
     * would simply be re-scanned forever; returning immediately at least keeps
     * the scheduler responsive rather than spinning inside a fake poll. */
    (void)timeout_ms;
    return;
#else
    /* Build pollfds array. */
    uint32_t n = env->io_pending_count;
    struct pollfd *pfds = (struct pollfd *)malloc(n * sizeof(struct pollfd));
    TuriIoPending **pptrs = (TuriIoPending **)malloc(n * sizeof(TuriIoPending *));
    uint32_t i = 0;
    for (TuriIoPending *p = env->io_pending_head; p; p = p->next, i++) {
        pfds[i].fd      = p->fd;
        pfds[i].events  = (p->op == TURI_IO_READ) ? POLLIN : POLLOUT;
        pfds[i].revents = 0;
        pptrs[i] = p;
    }

    poll(pfds, (nfds_t)n, timeout_ms);

    /* Process ready fds; build a new pending list of unresolved entries. */
    TuriIoPending *new_head = NULL;
    uint32_t new_count = 0;

    for (i = 0; i < n; i++) {
        TuriIoPending *p = pptrs[i];
        bool ready = (pfds[i].revents & (pfds[i].events | POLLERR | POLLHUP)) != 0;

        if (ready) {
            TuriValue result;
            if (p->op == TURI_IO_READ) {
                ssize_t r = read(p->fd, p->read_buf, (size_t)p->read_len);
                if (r < 0) {
                    result = turi_errorf("read-async: %s", strerror(errno));
                    turi_future_reject(env, p->future, result);
                } else {
                    p->read_buf[r] = '\0';
                    /* Duplicate the string so it survives after we free p. */
                    char *s = strdup(p->read_buf);
                    turi_future_resolve(env, p->future, turi_cstr(s));
                }
                free(p->read_buf);
            } else {
                ssize_t w = write(p->fd, p->write_buf, (size_t)p->write_len);
                if (w < 0) {
                    result = turi_errorf("write-async: %s", strerror(errno));
                    turi_future_reject(env, p->future, result);
                } else {
                    turi_future_resolve(env, p->future, turi_int((int64_t)w));
                }
                free(p->write_buf);
            }
            free(p);
        } else {
            /* Re-add to new pending list. */
            p->next = new_head;
            new_head = p;
            new_count++;
        }
    }

    env->io_pending_head  = new_head;
    env->io_pending_count = new_count;

    free(pfds);
    free(pptrs);
#endif /* !_WIN32 */
}

#else  /* __EMSCRIPTEN__ — stub out POSIX I/O */

void turi_io_read_async(TuriEnv *env, int fd, int n, TuriFuture *future,
                        TuriFiber *fiber) {
    (void)env; (void)fd; (void)n; (void)fiber;
    turi_future_reject(env, future, turi_error("read-async: not supported in WASM"));
}

void turi_io_write_async(TuriEnv *env, int fd, const char *data, int len,
                         TuriFuture *future, TuriFiber *fiber) {
    (void)env; (void)fd; (void)data; (void)len; (void)fiber;
    turi_future_reject(env, future, turi_error("write-async: not supported in WASM"));
}

static void poll_io(TuriEnv *env, int timeout_ms) { (void)env; (void)timeout_ms; }

#endif /* __EMSCRIPTEN__ */

/* -------------------------------------------------------------------------
 * Cancellation
 * ---------------------------------------------------------------------- */

void turi_task_cancel(TuriEnv *env, TuriFuture *f) {
    if (!f) return;
    TuriFiber *owner = f->owner;
    if (owner) owner->cancelled = true;
    turi_future_reject(env, f, turi_rejection("task cancelled"));
}

bool turi_fiber_is_cancelled(TuriEnv *env) {
    return env->current_fiber && env->current_fiber->cancelled;
}

/* -------------------------------------------------------------------------
 * await: run event loop until a future resolves
 * ---------------------------------------------------------------------- */

TuriValue turi_await_future(TuriEnv *env, TuriFuture *f) {
    /* If already settled, return immediately. */
    if (f->state == TURI_FUTURE_RESOLVED) return f->result;
    if (f->state == TURI_FUTURE_REJECTED) {
        /* Surface as a TURI_REJECTION value the caller can observe via
         * (error? r) -- distinct from TURI_ERROR so binding it does not
         * trigger the interpreter's universal error short-circuit. */
        if (turi_is_rejection(f->result)) return f->result;
        if (turi_is_error(f->result))
            return turi_rejection(turi_error_message(f->result));
        return turi_rejection("async task rejected");
    }

    /* Run event loop until the future settles. */
    while (f->state == TURI_FUTURE_PENDING) {
        fire_timers(env);

        /* If a fiber is ready, run it. */
        TuriFiber *fiber = sched_dequeue(env);
        if (fiber) {
            env->current_fiber = fiber;
            fiber->state = TURI_FIBER_RUNNING;
            /* Set before swapcontext so async_fiber_thunk (eval.c) reads the
             * right pointer on first entry, even when multiple fibers were
             * created before any of them ran. */
            g_pending_async_fiber = fiber;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            swapcontext(&env->sched_ctx, &fiber->ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
            /* Control is back on the scheduler stack; if the fiber finished,
             * release its (now idle) execution stack immediately rather than
             * holding it until env teardown. */
            turi_fiber_reclaim_if_done(env, fiber);
            env->current_fiber = NULL;
            continue;
        }

        /* Nothing runnable, no timer armed, no I/O in flight, and the future
         * is still pending: nothing in this single-threaded scheduler can
         * ever settle it.  The awaited task is parked (on a session channel
         * or on another parked task) with no peer to wake it.  Report the
         * deadlock instead of polling forever at 100% CPU -- the same
         * detection the session recv path does (turi-session-expansion S6).
         * Natives never resolve a future from another thread, so there is no
         * outside wake source to wait for. */
        /* Re-test the future: the fire_timers at the top of this iteration
         * may just have settled it (e.g. `(await (sleep-async n))` in the
         * main context), in which case an empty timer list is the normal
         * exit, not a deadlock. */
        if (f->state == TURI_FUTURE_PENDING && !env->timers_head &&
            env->io_pending_count == 0) {
            return turi_error("eval: await deadlocked: the awaited task is parked "
                              "and nothing else is runnable -- no participant can "
                              "make progress; this is a deadlock in the program "
                              "(the compiled binary would hang here)");
        }

        /* No ready fibers — wait for timers or I/O. */
        uint64_t now = turi_now_ms();
        uint64_t next = next_timer_deadline(env);
        int wait_ms = (next == UINT64_MAX) ? 10
                    : (next <= now)        ? 0
                    : (int)(next - now);
        if (wait_ms > 50) wait_ms = 50;

        poll_io(env, wait_ms);
        fire_timers(env);
    }

    if (f->state == TURI_FUTURE_RESOLVED) return f->result;
    /* Rejected mid-event-loop: mirror the entry-rejection branch above and
     * surface a TURI_REJECTION value -- callers observe via (error? r) /
     * (error-message r).  No throw channel. */
    if (turi_is_rejection(f->result)) return f->result;
    if (turi_is_error(f->result))
        return turi_rejection(turi_error_message(f->result));
    return turi_rejection("async task rejected");
}

/* -------------------------------------------------------------------------
 * Single scheduler step (cooperative session runtime, eval.c)
 * ---------------------------------------------------------------------- */

bool turi_sched_step(TuriEnv *env) {
    fire_timers(env);

    TuriFiber *fiber = sched_dequeue(env);
    if (fiber) {
        env->current_fiber = fiber;
        fiber->state = TURI_FIBER_RUNNING;
        g_pending_async_fiber = fiber;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&env->sched_ctx, &fiber->ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        turi_fiber_reclaim_if_done(env, fiber);
        env->current_fiber = NULL;
        return true;
    }

    bool has_timers = (env->timers_head != NULL);
    bool has_io     = (env->io_pending_count > 0);
    if (!has_timers && !has_io) return false;

    uint64_t now  = turi_now_ms();
    uint64_t next = next_timer_deadline(env);
    int wait_ms   = (next == UINT64_MAX) ? 10
                  : (next <= now)        ? 0
                  : (int)(next - now);
    if (wait_ms > 50) wait_ms = 50;

    poll_io(env, wait_ms);
    fire_timers(env);
    return true;
}

bool turi_sched_yield(TuriEnv *env) {
    TuriFiber *cur = env->current_fiber;
    if (!cur) return turi_sched_step(env);
    cur->state = TURI_FIBER_READY;
    turi_sched_enqueue(env, cur);
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
    return true;
}

/* -------------------------------------------------------------------------
 * Main event loop
 * ---------------------------------------------------------------------- */

void turi_run_event_loop(TuriEnv *env) {
    for (;;) {
        fire_timers(env);

        TuriFiber *fiber = sched_dequeue(env);
        if (fiber) {
            env->current_fiber = fiber;
            fiber->state = TURI_FIBER_RUNNING;
            g_pending_async_fiber = fiber;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            swapcontext(&env->sched_ctx, &fiber->ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
            /* Control is back on the scheduler stack; if the fiber finished,
             * release its (now idle) execution stack immediately rather than
             * holding it until env teardown. */
            turi_fiber_reclaim_if_done(env, fiber);
            env->current_fiber = NULL;
            continue;
        }

        /* Nothing to do? */
        bool has_timers = (env->timers_head != NULL);
        bool has_io     = (env->io_pending_count > 0);
        if (!has_timers && !has_io) break;

        uint64_t now  = turi_now_ms();
        uint64_t next = next_timer_deadline(env);
        int wait_ms   = (next == UINT64_MAX) ? 10
                      : (next <= now)        ? 0
                      : (int)(next - now);
        if (wait_ms > 50) wait_ms = 50;

        poll_io(env, wait_ms);
        fire_timers(env);
    }

    /* Report unhandled rejected futures. */
    for (TuriFuture *f = env->all_futures; f; f = f->next_alloc) {
        if (f->state == TURI_FUTURE_REJECTED && !f->reported && !f->wakers) {
            f->reported = true;
            const char *msg = "<rejected>";
            if (turi_is_rejection(f->result) || turi_is_error(f->result))
                msg = f->result.as_error ? f->result.as_error : msg;
            fprintf(stderr, "warning: unhandled async task error: %s\n", msg);
        }
    }
}

/* -------------------------------------------------------------------------
 * sleep-async  (S7.4)
 * ---------------------------------------------------------------------- */

TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms) {
    TuriFuture *f = turi_future_new(env);
    turi_timer_add(env, ms, f);
    return turi_future_val(f);
}

/* -------------------------------------------------------------------------
 * turi_task_spawn — spawn from source string (C API, S7.9)
 * ---------------------------------------------------------------------- */

TuriValue turi_task_spawn(TuriEnv *env, const char *src) {
    /* Evaluate src to get a closure, then spawn as async. */
    TuriValue cl = turi_eval(env, src);
    if (turi_is_error(cl)) return cl;

    /* Wrap the closure in an async expression by spawning via native path. */
    /* We re-use the async fiber spawn logic from eval.c via a side channel:
     * eval the src inside a new fiber directly.  Since we don't have the
     * Expr* here, we use turi_eval to make a TURI_FUTURE through source. */
    char buf[256];
    (void)src;
    (void)cl;
    /* Simplest approach: build "(async (fn [] <TYPE> ...))" source wrapper.
     * But we don't know the return type.  Instead, treat the result of
     * evaluating src as the final value: wrap in a future immediately. */
    if (cl.tag != TURI_CLOSURE)
        return turi_error("turi_task_spawn: src must evaluate to a function");

    /* Synchronous fallback: turi_task_spawn cannot yet spawn a real async fiber
     * without access to the eval.c internals (EX_ASYNC path). Warn and resolve
     * the closure result immediately as a completed future. */
    fprintf(stderr, "warning: turi_task_spawn is running synchronously (not yet integrated with async fiber scheduler)\n");
    (void)buf;

    TuriValue result = turi_eval(env, src);
    TuriFuture *f = turi_future_new(env);
    if (turi_is_error(result))
        turi_future_reject(env, f, result);
    else
        turi_future_resolve(env, f, result);
    return turi_future_val(f);
}

/* -------------------------------------------------------------------------
 * Native async builtins (registered by turi_async_register_builtins)
 * ---------------------------------------------------------------------- */

/* sleep-async : (ms :int) -> Future */
static TuriValue native_sleep_async(TuriEnv *env, TuriValue *args, uint32_t n,
                                    void *ud) {
    (void)ud;
    if (n != 1 || args[0].tag != TURI_INT)
        return turi_error("sleep-async: expected (ms :int)");
    uint64_t ms = (uint64_t)args[0].as_int;

    TuriFiber *cur = env->current_fiber;
    if (!cur) {
        /* Main context: return future so (await (sleep-async ms)) works. */
        return turi_sleep_async(env, ms);
    }

    /* Inside a fiber: create future, add timer, suspend until it fires. */
    TuriFuture *f = turi_future_new(env);
    turi_timer_add(env, ms, f);
    turi_future_add_waker(f, cur);
    cur->awaiting_future = f;
    cur->state = TURI_FIBER_SUSPENDED;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
    /* Resumed: timer fired.  Hand back the (now resolved) future rather than
     * nil: `(await (sleep-async ms))` is the documented spelling and the type
     * checker sees a Future, so a bare nil here made the fiber's `await` fail
     * on "expected a future" and silently drop the rest of the fiber body
     * (awaited-sleep-async-in-a-fiber-drops-the-rest-of-the-body).  A resolved
     * future is what the main-context arm's caller ends up awaiting too, so
     * both contexts now agree; a bare `(sleep-async ms)` still just blocks and
     * discards the value. */
    return turi_future_val(f);
}

/* with-timeout : (ms :int, task :Future) -> value or TURI_ERROR("timeout") */
static TuriValue native_with_timeout(TuriEnv *env, TuriValue *args, uint32_t n,
                                      void *ud) {
    (void)ud;
    if (n != 2 || args[0].tag != TURI_INT || args[1].tag != TURI_FUTURE)
        return turi_error("with-timeout: expected (ms :int, task :future)");
    uint64_t ms = (uint64_t)args[0].as_int;
    TuriFuture *task = args[1].as_future;

    /* Create a timer future. */
    TuriFuture *timer_f = turi_future_new(env);
    turi_timer_add(env, ms, timer_f);

    /* Run until either the task or the timer resolves. */
    while (task->state == TURI_FUTURE_PENDING && timer_f->state == TURI_FUTURE_PENDING) {
        fire_timers(env);
        TuriFiber *fiber = sched_dequeue(env);
        if (fiber) {
            env->current_fiber = fiber;
            fiber->state = TURI_FIBER_RUNNING;
            g_pending_async_fiber = fiber;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            swapcontext(&env->sched_ctx, &fiber->ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
            /* Control is back on the scheduler stack; if the fiber finished,
             * release its (now idle) execution stack immediately rather than
             * holding it until env teardown. */
            turi_fiber_reclaim_if_done(env, fiber);
            env->current_fiber = NULL;
        } else {
            poll_io(env, 5);
            fire_timers(env);
        }
    }

    if (task->state != TURI_FUTURE_PENDING) {
        /* Task resolved first. */
        if (task->state == TURI_FUTURE_RESOLVED) return task->result;
        if (turi_is_error(task->result)) return task->result;
        return turi_error("async task rejected");
    }
    /* Timer fired first: cancel the timed-out task, then surface a
     * TURI_REJECTION("timeout") -- callers observe via (error? r). */
    turi_task_cancel(env, task);
    return turi_rejection("timeout");
}

/* task-cancelled? : () -> bool */
static TuriValue native_task_cancelled(TuriEnv *env, TuriValue *args,
                                       uint32_t n, void *ud) {
    (void)args; (void)n; (void)ud;
    return turi_bool(turi_fiber_is_cancelled(env));
}

/* cancel-task : (task :Future) -> nil */
static TuriValue native_cancel_task(TuriEnv *env, TuriValue *args, uint32_t n,
                                    void *ud) {
    (void)ud;
    if (n != 1 || args[0].tag != TURI_FUTURE)
        return turi_error("cancel-task: expected (task :future)");
    turi_task_cancel(env, args[0].as_future);
    return turi_nil();
}

/* async-pipe-init : () -> nil  (sets up test pipe in env) */
#if defined(__EMSCRIPTEN__)
static TuriValue native_async_pipe_init(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_error("async-pipe-init: not supported in WASM");
}
#elif defined(_WIN32)
/* This helper exists to feed poll_io(), which is itself compiled out on Windows
 * (no poll(2) for CRT file descriptors -- see the include block at the top).
 * Handing back a working pipe nobody can ever poll would be a trap, so fail
 * where the mistake is visible. */
static TuriValue native_async_pipe_init(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_error("async-pipe-init: not supported on Windows");
}
#else
static TuriValue native_async_pipe_init(TuriEnv *env, TuriValue *args,
                                        uint32_t n, void *ud) {
    (void)args; (void)n; (void)ud;
    int fds[2];
    if (pipe(fds) != 0)
        return turi_errorf("async-pipe-init: %s", strerror(errno));
    /* Set both ends non-blocking. */
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    env->test_pipe_rfd = fds[0];
    env->test_pipe_wfd = fds[1];
    return turi_nil();
}
#endif

/* async-pipe-rfd / async-pipe-wfd : () -> int */
static TuriValue native_async_pipe_rfd(TuriEnv *env, TuriValue *args,
                                       uint32_t n, void *ud) {
    (void)args; (void)n; (void)ud;
    if (env->test_pipe_rfd < 0)
        return turi_error("async-pipe-rfd: pipe not initialised");
    return turi_int(env->test_pipe_rfd);
}
static TuriValue native_async_pipe_wfd(TuriEnv *env, TuriValue *args,
                                       uint32_t n, void *ud) {
    (void)args; (void)n; (void)ud;
    if (env->test_pipe_wfd < 0)
        return turi_error("async-pipe-wfd: pipe not initialised");
    return turi_int(env->test_pipe_wfd);
}

/* read-async : (fd :int, n :int) -> Future[cstr] */
#ifndef __EMSCRIPTEN__
static TuriValue native_read_async(TuriEnv *env, TuriValue *args, uint32_t n,
                                   void *ud) {
    (void)ud;
    if (n != 2 || args[0].tag != TURI_INT || args[1].tag != TURI_INT)
        return turi_error("read-async: expected (fd :int, n :int)");
    /* Range-check the byte count BEFORE narrowing it to `int`: a negative
     * count made `(size_t)bytes + 1` wrap to a tiny malloc and the read below
     * run SIZE_MAX bytes into it, and 2^32-1 narrowed to -1 on the way
     * (security audit WP5, M-5).  It needs TURI_CAP_IO since WP3, but an
     * unsandboxed program's own count reaches it just the same. */
    if (args[1].as_int < 0 || args[1].as_int > INT_MAX - 1)
        return turi_errorf("read-async: byte count %lld out of range",
                           (long long)args[1].as_int);
    int fd    = (int)args[0].as_int;
    int bytes = (int)args[1].as_int;

    /* Try non-blocking read first. */
    char *buf = (char *)malloc((size_t)bytes + 1);
    if (!buf) return turi_error("read-async: out of memory");
    ssize_t r = read(fd, buf, (size_t)bytes);
    if (r >= 0) {
        buf[r] = '\0';
        char *s = strdup(buf);
        free(buf);
        TuriFuture *f = turi_future_new(env);
        turi_future_resolve(env, f, turi_cstr(s));
        return turi_future_val(f);
    }
    free(buf);
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return turi_errorf("read-async: %s", strerror(errno));

    /* Would block: register pending I/O and suspend current fiber. */
    TuriFuture *f = turi_future_new(env);
    TuriFiber  *cur = env->current_fiber;
    if (cur) {
        turi_future_add_waker(f, cur);
        cur->awaiting_future = f;
        cur->state = TURI_FIBER_SUSPENDED;
    }
    turi_io_read_async(env, fd, bytes, f, cur);
    if (cur) {
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        /* Resumed: I/O completed and `f` is settled.  Hand back the FUTURE, as
         * the main-context arm below already does -- same reason as
         * native_sleep_async above: this native's contract is `-> Future` in
         * both contexts, and returning the bare value made `(await (read-async
         * fd n))` inside a fiber fail EX_AWAIT's future-tag check, which
         * rejects the enclosing fiber's own future and silently drops the rest
         * of its body.  Awaiting an already-settled future is free, so the bare
         * spelling still blocks here and returns immediately. */
    }
    return turi_future_val(f);
}
#else
static TuriValue native_read_async(TuriEnv *env, TuriValue *args, uint32_t n,
                                   void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_error("read-async: not supported in WASM");
}
#endif

/* write-async : (fd :int, data :cstr) -> Future[int] */
#ifndef __EMSCRIPTEN__
static TuriValue native_write_async(TuriEnv *env, TuriValue *args, uint32_t n,
                                    void *ud) {
    (void)ud;
    if (n != 2 || args[0].tag != TURI_INT || args[1].tag != TURI_CSTR)
        return turi_error("write-async: expected (fd :int, data :cstr)");
    int         fd   = (int)args[0].as_int;
    const char *data = args[1].as_cstr ? args[1].as_cstr : "";
    int         len  = (int)strlen(data);

    /* Try non-blocking write first. */
    ssize_t w = write(fd, data, (size_t)len);
    if (w >= 0) {
        TuriFuture *f = turi_future_new(env);
        turi_future_resolve(env, f, turi_int((int64_t)w));
        return turi_future_val(f);
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
        return turi_errorf("write-async: %s", strerror(errno));

    TuriFuture *f   = turi_future_new(env);
    TuriFiber  *cur = env->current_fiber;
    if (cur) {
        turi_future_add_waker(f, cur);
        cur->awaiting_future = f;
        cur->state = TURI_FIBER_SUSPENDED;
    }
    turi_io_write_async(env, fd, data, len, f, cur);
    if (cur) {
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        swapcontext(&cur->ctx, &env->sched_ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
        /* Resumed: `f` is settled.  Return the FUTURE -- see native_read_async. */
    }
    return turi_future_val(f);
}
#else
static TuriValue native_write_async(TuriEnv *env, TuriValue *args, uint32_t n,
                                    void *ud) {
    (void)env; (void)args; (void)n; (void)ud;
    return turi_error("write-async: not supported in WASM");
}
#endif

/* async-all2 : (fa :Future, fb :Future) -> (a + b) as int — simplified
 * (a real implementation would need a list type; this is a test helper) */
static TuriValue native_async_all2(TuriEnv *env, TuriValue *args, uint32_t n,
                                   void *ud) {
    (void)ud;
    if (n != 2 || args[0].tag != TURI_FUTURE || args[1].tag != TURI_FUTURE)
        return turi_error("async-all2: expected (fa :future, fb :future)");
    TuriValue a = turi_await_future(env, args[0].as_future);
    if (turi_is_error(a)) return a;
    TuriValue b = turi_await_future(env, args[1].as_future);
    if (turi_is_error(b)) return b;
    /* Return first result (callers may use separately) */
    (void)b;
    /* Pack both into a struct? No struct support here.  Return as a string. */
    if (a.tag == TURI_INT && b.tag == TURI_INT)
        return turi_int(a.as_int + b.as_int);
    return a; /* fallback */
}

/* await-val : (task :Future) -> value — synchronous await from Turmeric */
static TuriValue native_await_val(TuriEnv *env, TuriValue *args, uint32_t n,
                                  void *ud) {
    (void)ud;
    if (n != 1 || args[0].tag != TURI_FUTURE)
        return turi_error("await-val: expected (task :future)");
    return turi_await_future(env, args[0].as_future);
}

/* async-race : (fa :Future, fb :Future) -> value
 * Waits for whichever future resolves first; cancels the other. */
static TuriValue native_async_race(TuriEnv *env, TuriValue *args, uint32_t n, void *ud) {
    (void)ud;
    if (n != 2 || args[0].tag != TURI_FUTURE || args[1].tag != TURI_FUTURE)
        return turi_error("async-race: expected (fa :future, fb :future)");
    TuriFuture *fa = args[0].as_future;
    TuriFuture *fb = args[1].as_future;

    /* Run scheduler until one of the futures resolves. */
    while (fa->state == TURI_FUTURE_PENDING && fb->state == TURI_FUTURE_PENDING) {
        fire_timers(env);
        TuriFiber *fiber = sched_dequeue(env);
        if (fiber) {
            env->current_fiber = fiber;
            fiber->state = TURI_FIBER_RUNNING;
            g_pending_async_fiber = fiber;
#if defined(__APPLE__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
            swapcontext(&env->sched_ctx, &fiber->ctx);
#if defined(__APPLE__)
#  pragma clang diagnostic pop
#endif
            /* Control is back on the scheduler stack; if the fiber finished,
             * release its (now idle) execution stack immediately rather than
             * holding it until env teardown. */
            turi_fiber_reclaim_if_done(env, fiber);
            env->current_fiber = NULL;
        } else {
            poll_io(env, 5);
            fire_timers(env);
        }
    }

    /* Cancel the loser and return the winner's result. */
    if (fa->state != TURI_FUTURE_PENDING) {
        turi_task_cancel(env, fb);
        if (fa->state == TURI_FUTURE_RESOLVED) return fa->result;
        if (turi_is_error(fa->result)) return fa->result;
        return turi_error("async-race: task a rejected");
    } else {
        turi_task_cancel(env, fa);
        if (fb->state == TURI_FUTURE_RESOLVED) return fb->result;
        if (turi_is_error(fb->result)) return fb->result;
        return turi_error("async-race: task b rejected");
    }
}

/* -------------------------------------------------------------------------
 * Registration
 * ---------------------------------------------------------------------- */

void turi_async_register_builtins(TuriEnv *env) {
    turi_env_register_native(env, "sleep-async",      native_sleep_async,    NULL);
    turi_env_register_native(env, "with-timeout",     native_with_timeout,   NULL);
    turi_env_register_native(env, "task-cancelled?",  native_task_cancelled, NULL);
    turi_env_register_native(env, "cancel-task",      native_cancel_task,    NULL);
    turi_env_register_native(env, "async-pipe-init",  native_async_pipe_init,NULL);
    turi_env_register_native(env, "async-pipe-rfd",   native_async_pipe_rfd, NULL);
    turi_env_register_native(env, "async-pipe-wfd",   native_async_pipe_wfd, NULL);
    turi_env_register_native(env, "read-async",       native_read_async,     NULL);
    turi_env_register_native(env, "write-async",      native_write_async,    NULL);
    turi_env_register_native(env, "async-all2",       native_async_all2,     NULL);
    turi_env_register_native(env, "await-val",        native_await_val,      NULL);
    turi_env_register_native(env, "async-race",       native_async_race,     NULL);
}

/* -------------------------------------------------------------------------
 * Scheduler init / free
 * ---------------------------------------------------------------------- */

void turi_sched_init(TuriEnv *env) {
    env->sched_ready_head = NULL;
    env->sched_ready_tail = NULL;
    env->current_fiber    = NULL;
    env->timers_head      = NULL;
    env->io_pending_head  = NULL;
    env->io_pending_count = 0;
    env->all_futures      = NULL;
    env->coro_stacks      = NULL;
    env->test_pipe_rfd    = -1;
    env->test_pipe_wfd    = -1;
}

/* turi-value-pool-residual-sites: track a coroutine stack for bulk reclaim in
 * turi_sched_free.  The node lives in the value pool (freed with the env); the
 * base pointer is munmap/free'd in the teardown walk below. */
TuriCoroStack *turi_env_track_coro_stack(TuriEnv *env, void *base, size_t size) {
    if (!base) return NULL;
    /* Env-lifetime metadata (the coro_stacks list is walked at turi_sched_free),
     * so it must live in value_perm, never the rewindable value_scratch pool:
     * with scratch promotion enabled, a scratch-allocated node would be poisoned
     * by arena_reset while env->coro_stacks still linked it, crashing teardown.
     * value_perm is present and freed at turi_env_free regardless of the
     * promotion setting, so this is correct on the default path too. */
    TuriCoroStack *node = (TuriCoroStack *)turi_val_perm_alloc(env, sizeof(TuriCoroStack));
    node->base = base;
    node->size = size;
    node->next = env->coro_stacks;
    env->coro_stacks = node;
    return node;
}

/* turi-async-fiber-stack-reclaim: munmap/free a DONE fiber's stack early.  See
 * the header declaration for the calling contract.  The teardown walk in
 * turi_sched_free skips nodes whose base is NULL, so tombstoning here makes the
 * two reclaim paths mutually exclusive. */
void turi_fiber_reclaim_if_done(TuriEnv *env, TuriFiber *fiber) {
    (void)env;
    if (!fiber || fiber->state != TURI_FIBER_DONE || !fiber->stack) return;
#ifndef __EMSCRIPTEN__
    munmap(fiber->stack, TURI_ASYNC_STACK_SIZE);
#else
    free(fiber->stack);
#endif
    if (fiber->stack_node) fiber->stack_node->base = NULL;
    fiber->stack      = NULL;
    fiber->stack_node = NULL;
}

void turi_sched_free(TuriEnv *env) {
    /* Free timers. */
    TuriTimer *t = env->timers_head;
    while (t) { TuriTimer *next = t->next; free(t); t = next; }
    env->timers_head = NULL;

    /* Free I/O pending. */
    TuriIoPending *io = env->io_pending_head;
    while (io) {
        TuriIoPending *next = io->next;
        free(io->read_buf);
        free(io->write_buf);
        free(io);
        io = next;
    }
    env->io_pending_head = NULL;

    /* Free all futures (including their waker chains). */
    TuriFuture *f = env->all_futures;
    while (f) {
        TuriWaker *w = f->wakers;
        while (w) { TuriWaker *wn = w->next; free(w); w = wn; }
        TuriFuture *fn = f->next_alloc;
        free(f);
        f = fn;
    }
    env->all_futures = NULL;

    /* Close test pipe fds. */
#ifndef __EMSCRIPTEN__
    if (env->test_pipe_rfd >= 0) { close(env->test_pipe_rfd); env->test_pipe_rfd = -1; }
    if (env->test_pipe_wfd >= 0) { close(env->test_pipe_wfd); env->test_pipe_wfd = -1; }
#endif

    /* turi-value-pool-residual-sites: reclaim coroutine execution stacks.
     * These were mmap'd (native) / malloc'd (WASM) outside the value pool so
     * they could back a ucontext_t; release them with the matching dealloc.
     * Runs before turi_env_free reclaims the value pool, so the nodes (which
     * live in that pool) are still valid here. */
    TuriCoroStack *cs = env->coro_stacks;
    while (cs) {
        TuriCoroStack *next = cs->next;
        if (cs->base) {
#ifndef __EMSCRIPTEN__
            munmap(cs->base, cs->size);
#else
            free(cs->base);
#endif
        }
        cs = next;
    }
    env->coro_stacks = NULL;
}
