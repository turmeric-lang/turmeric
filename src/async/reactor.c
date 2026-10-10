/**
 * reactor.c -- lightweight evented I/O reactor (Phase R1/R4/R5/R6)
 *
 * Thin wrapper over IOBackend. Sources are heap-allocated and never freed
 * until tur_reactor_free so that the pointer passed as IOBackend user_data
 * remains valid for the entire lifetime of the reactor (prevents
 * use-after-free when a callback fires for a source that was removed
 * during the same poll batch).
 *
 * Timer sources (Phase R4) are stored in the same sources array but have
 * no IOBackend registration. tur_reactor_poll caps the io_poll timeout at
 * the earliest pending timer deadline and fires elapsed timers synchronously
 * after io_poll returns.
 *
 * Signal sources (Phase R5):
 *   Linux (IO_BACKEND_EPOLL): signalfd. Blocks the signal via sigprocmask,
 *   creates a signalfd and registers it as a readable fd.
 *   macOS/BSD (IO_BACKEND_KQUEUE): self-pipe + sigaction. A global dispatch
 *   table maps signal numbers to pipe write fds; the signal handler writes
 *   the signum as a byte; the reactor reads from the pipe read end.
 *   WASM: tur_reactor_add_signal returns -1 (unsupported).
 *
 * Channel bridge (Phase R6):
 *   tick_chans scans all TUR_RSRC_CHAN sources and non-blocking dequeues one
 *   value per source if available. Callers should call tur_reactor_wake(r)
 *   after chan-send for low-latency delivery.
 */

/* Before anything that might pull in <windows.h>, which would otherwise drag
 * in the old <winsock.h> and collide with this. */
#ifdef _WIN32
#include <winsock2.h>
#endif

#include "reactor.h"
#include "local_fiber.h"
#include "fiber.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

#ifdef IO_BACKEND_EPOLL
#include <sys/signalfd.h>
#endif

/*
 * Signal sources need either Linux signalfd, or POSIX sigaction() plus the
 * self-pipe trick.  Windows has neither -- there is no sigaction, and a Win32
 * console control handler is a different enough model that faking one here
 * would be a lie -- and Emscripten has no signals at all.
 *
 * Both platforms therefore compile the signal-source path out entirely and fall
 * through to the "signals not supported" arm, which reports -1 from
 * tur_reactor_add_signal().  Everything else in the reactor (timers, fd sources,
 * channel bridges) is unaffected.
 *
 * Named rather than repeating `!defined(__EMSCRIPTEN__) && !defined(_WIN32)` at
 * four sites: the next platform to lack signals should only have to edit here.
 */
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#define TUR_REACTOR_HAVE_SIGNALS 1
#endif

#define REACTOR_INITIAL_CAP 16

/* closure-drop-glue (async/reactor blocker): since closure-drop-glue graduated
 * (2026-07-22) every heap fat-closure env the emitted program builds carries an 8-byte
 * drop-glue header at env[-1] and the fat pointer is handed back PAST it.  The
 * reactor and fiber group own such callback boxes and free them at teardown,
 * but this precompiled libturi C cannot name the per-program `tur_closure_drop`.
 * The emitted program instead flips this runtime flag to 1 at startup (flag-off
 * it stays 0), and the reactor releases an owned box through its header (which
 * also walks owning captures) instead of interior-freeing the past-header
 * pointer.  Every box the reactor/fiber OWNS is an emitted (headered) closure --
 * httpd's hand-rolled { __fn, ptr } boxes are all disowned (tur_reactor_disown_cb
 * / tur_local_disown_body) -- so consulting the flag is sound.
 *
 * WEAK so a flag-on emitted program's STRONG `tur_closure_headers_enabled = 1`
 * overrides this default at link; flag-off (no override) it stays 0 and the
 * reactor keeps its plain-free behavior. */
__attribute__((weak)) int tur_closure_headers_enabled = 0;

/* Release a reactor/fiber-owned fat-closure box.  Header-aware when the emitted
 * program enabled closure headers; otherwise a plain free (the flag-off ABI). */
static void tur_reactor_release_box(int64_t box) {
    if (!box) return;
    if (tur_closure_headers_enabled) {
        void *h = (void *)(intptr_t)box;
        void (**hdr)(void *) = ((void (**)(void *))h) - 1;
        void (*drop)(void *) = *hdr;
        if (drop) drop(h);          /* walks owning captures, frees the base */
        else free((void *)hdr);     /* header-only (e.g. shim box): free base */
    } else {
        free((void *)(intptr_t)box);
    }
}

/* ------------------------------------------------------------------ */
/* Internal types                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    TUR_RSRC_FD             = 0,
    TUR_RSRC_TIMER_ONESHOT  = 1,
    TUR_RSRC_TIMER_INTERVAL = 2,
    TUR_RSRC_SIGNAL         = 3,
    TUR_RSRC_CHAN           = 4
} TurSourceKind;

typedef struct {
    int64_t       id;
    /* This source's index in r->sources.  Recorded so a removal can hand the
     * slot back for reuse without re-scanning the array. */
    size_t        slot;
    TurSourceKind kind;
    /* FD and signal (pipe read or signalfd) */
    int           fd;
    /* Signal-specific */
    int           signum;
    int           sig_write_fd;    /* kqueue: write end of self-pipe */
#ifdef TUR_REACTOR_HAVE_SIGNALS
    struct sigaction sig_old_sa;   /* kqueue: saved action for restore */
#endif
    /* Timer-specific */
    int64_t       deadline_ms;     /* absolute CLOCK_MONOTONIC ms */
    int64_t       interval_ms;     /* 0 for one-shot timers */
    /* Channel bridge */
    void         *chan_ptr;        /* ChanBlock* */
    /* Common callback fields */
    int64_t       tur_cb;
    int64_t       tur_user_data;
    bool          active;
    /* True when tur_cb points at a heap fat-closure box the reactor owns and
     * must free at teardown (the normal case for the public tur_reactor_add_*
     * ABI the emitted program calls). False for internal park registrations
     * whose box is an inline LocalFiber field, not a heap allocation --
     * freeing that would corrupt the heap. */
    bool          owns_cb;
} TurReactorSource;

struct TurReactor {
    IOBackend           *backend;
    TurReactorSource   **sources;   /* stable pointers; never realloc'd entries */
    size_t               sources_len;
    size_t               sources_cap;
    int64_t              next_id;
    atomic_int           stop_flag;
    /* Slot recycling, in TWO stages on purpose.
     *
     * Without it `sources` only ever grew: alloc_source appended, and
     * tur_reactor_remove deactivated a source but never reclaimed its slot.
     * A fiber that parks in a loop -- the normal shape of an await inside a
     * long-lived connection handler -- therefore leaked two slots per park,
     * and since cap_timeout and tick_timers each walk the WHOLE array on
     * every poll, the cost of a poll grew with sources-ever-created rather
     * than sources-active.  Measured on httpd-async-limit: 3953 -> 5285
     * slots in five seconds, still climbing.
     *
     * Two stages, not one.  tick_timers holds `src` across the callback it
     * runs and writes through it afterwards (`src->active = false` for a
     * one-shot).  If that callback both REMOVES a source and REGISTERS one,
     * immediate recycling hands the freed slot to the new registration and
     * the post-callback write then lands on the new source, silently
     * deactivating it.
     *
     * Both halves are required, which is narrower than it first looks:
     * tick_timers deactivates a one-shot directly and never calls
     * tur_reactor_remove, so an ordinary re-arming timer callback does NOT
     * trigger it (measured -- it passes under immediate recycling).  A
     * callback that calls tur_reactor_remove on its own source and then
     * registers a replacement DOES: under immediate recycling the
     * replacement never fires, under deferred it does.
     *
     * Nothing in-tree does both today.  local_park_wake comes one step
     * away -- it removes the source being iterated, via
     * local_fiber_clear_park -- but does not register.  So this is
     * defensive against a shape the API permits, not a bug caught in
     * flight.  It costs one push per removal.
     *
     * `pending` collects slots freed during a poll; tur_reactor_poll
     * promotes them to `freelist` at the TOP of the next poll, by which
     * point every loop that could have been holding one has returned.
     *
     * The source structs themselves are never freed here -- the "stable
     * pointers" invariant above still holds; a reused slot keeps its
     * allocation and is reset in place. */
    size_t              *freelist;      /* slot indices safe to reuse now */
    size_t               freelist_len, freelist_cap;
    size_t              *pending;       /* freed during the current poll */
    size_t               pending_len, pending_cap;
    /* Callback boxes orphaned by slot reuse.
     *
     * cleanup_source does NOT release a source's cb box -- ownership is
     * settled at teardown, which walks r->sources and frees each owned box
     * once (deduped, because one box may be registered on several
     * sources).  Reusing a slot resets it in place, so without this the
     * previous occupant's box became unreachable and leaked: LeakSanitizer
     * measured exactly 250 allocations across 250 add/remove rounds.
     *
     * Freeing at reuse instead would be wrong for the same reason teardown
     * dedupes: the box may still be live on another source.  So the pointer
     * is parked here and goes through teardown's existing dedup. */
    int64_t             *orphan_boxes;
    size_t               orphan_len, orphan_cap;
    /* Sockets closing gracefully (tur_reactor_linger_close).  Listed so that
     * teardown can close whatever is still lingering. */
    struct TurLinger    *lingers;
    size_t               lingers_len;
};

/* Push a slot index onto one of the two lists; a failed grow just drops the
 * slot, which costs a recycle and never correctness. */
static void slot_push(size_t **arr, size_t *len, size_t *cap, size_t v) {
    if (*len == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        size_t *n = (size_t *)realloc(*arr, nc * sizeof(size_t));
        if (!n) return;
        *arr = n; *cap = nc;
    }
    (*arr)[(*len)++] = v;
}

/* ------------------------------------------------------------------ */
/* Self-pipe signal dispatch table (macOS / kqueue platforms)          */
/* ------------------------------------------------------------------ */

#if !defined(IO_BACKEND_EPOLL) && defined(TUR_REACTOR_HAVE_SIGNALS)
static int  g_sig_write_fd[NSIG];
static bool g_sig_table_init = false;

static void sig_self_pipe_handler(int signum) {
    if (signum >= 0 && signum < NSIG && g_sig_write_fd[signum] >= 0) {
        uint8_t b = (uint8_t)signum;
        (void)write(g_sig_write_fd[signum], &b, 1);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Monotonic clock helper                                               */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* Turmeric fat-closure dispatch                                         */
/* ------------------------------------------------------------------ */

/*
 * The three fat-closure callback shapes the reactor invokes.
 *
 * These typedefs MUST match, exactly, the C signature the emitter gives the
 * Turmeric callback -- `-fsanitize=function` (and CFI, CET/BTI, WASM
 * `call_indirect`) compares the pointer type at the indirect call against the
 * callee's real type, and a mismatch is UB even when the two happen to share
 * an ABI slot, as `int64_t` and `void *` do on AArch64 and x86-64.
 *
 * The mapping follows the FAT-CLOSURE entry convention, not the plain-defn
 * one: slot 0 of the fat box holds either a fatshim
 * (`__tur_fatshim_void_int64_t...`) or a capturing lambda's lifted entry
 * (`__fn_N(void *env, ...)`), and BOTH spell every value parameter in the
 * int64 carrier -- a `ptr<void>` argument is `int64_t`, not `void *`.  (A
 * bare top-level defn does emit `void *` for `ptr<void>`, but a callback
 * never reaches this file as a bare entry: registration always wraps it in
 * a fat box, and the box's slot-0 entry is what these typedefs must match.)
 * Only the leading env parameter is `void *`.  A `nil` return is `void`.
 *
 * These typedefs previously spelled the trailing `user` parameter `void *`,
 * which matched the pre-fat-normalization era; the 2026-08-16 effect-row
 * fat-normalization moved lambda callbacks onto the carrier-typed entries
 * and every reactor fixture then tripped clang's -fsanitize=function here.
 * If you change a callback's Turmeric signature, change the matching typedef
 * -- nothing cross-checks them, which is how these drift.  See
 * docs/archive/reactor-fd-callback-fn-ptr-type-mismatch.md and
 * docs/archive/emitter-thunk-type-return-mismatch.md.
 */
/* (env, id, events|signum|value, user) : nil -- fd, signal, and chan sources */
typedef void (*TurFdCbFn)(void *, int64_t, int64_t, int64_t);
/* (env, id, user) : nil -- timer sources */
typedef void (*TurTimerCbFn)(void *, int64_t, int64_t);
/* (env, user) : nil -- a fiber body */
typedef void (*TurFiberBodyFn)(void *, int64_t);

/*
 * Call a Turmeric closure with signature (id:int, events:int, user:ptr<void>)
 * using the fat-pointer calling convention.  Reused for signal and chan
 * callbacks where the second argument carries signum/value respectively.
 */
static void call_tur_fd_cb(int64_t tur_cb, int64_t id, int events,
                            int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    ((TurFdCbFn)(intptr_t)fat[0])((void *)fat, id, (int64_t)events,
                                  user_data);
}

/*
 * Call a Turmeric closure with signature (id:int, user:ptr<void>)
 * using the fat-pointer calling convention. Used for timer callbacks.
 */
static void call_tur_timer_cb(int64_t tur_cb, int64_t id, int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    ((TurTimerCbFn)(intptr_t)fat[0])((void *)fat, id, user_data);
}

/*
 * Call a Turmeric closure with signature (id:int, value:int, user:ptr<void>)
 * for channel bridge callbacks where value is a full int64_t.
 */
static void call_tur_chan_cb(int64_t tur_cb, int64_t id, int64_t value,
                              int64_t user_data) {
    if (!tur_cb) return;
    int64_t *fat = (int64_t *)(intptr_t)tur_cb;
    ((TurFdCbFn)(intptr_t)fat[0])((void *)fat, id, value, user_data);
}

/* ------------------------------------------------------------------ */
/* IOBackend shims                                                       */
/* ------------------------------------------------------------------ */

/* Registered as the IOBackend callback for fd sources. */
static void fd_shim(int fd, int events, void *user_data) {
    (void)fd;
    TurReactorSource *src = (TurReactorSource *)user_data;
    if (!src->active) return;
    call_tur_fd_cb(src->tur_cb, src->id, events, src->tur_user_data);
}

/* Registered as the IOBackend callback for signal sources. */
static void signal_shim(int fd, int events, void *user_data) {
    TurReactorSource *src = (TurReactorSource *)user_data;
    if (!src->active) return;
    (void)events;
#ifdef IO_BACKEND_EPOLL
    struct signalfd_siginfo si;
    ssize_t n;
    while ((n = read(fd, &si, sizeof(si))) == (ssize_t)sizeof(si))
        call_tur_fd_cb(src->tur_cb, src->id, (int)si.ssi_signo, src->tur_user_data);
#elif defined(TUR_REACTOR_HAVE_SIGNALS)
    uint8_t b;
    while (read(fd, &b, 1) == 1)
        call_tur_fd_cb(src->tur_cb, src->id, (int)b, src->tur_user_data);
#else
    (void)fd;
#endif
}

/* ------------------------------------------------------------------ */
/* Lingering close                                                      */
/* ------------------------------------------------------------------ */

/* At most this many sockets linger at once.  Each holds an fd for up to its
 * timeout, so past the cap a close is abortive again rather than letting a
 * flood of rejected connections pin descriptors. */
#define TUR_LINGER_MAX 1024

typedef struct TurLinger {
    /* Inline { entry, self } fat boxes for the fd and timer sources.  Like the
     * park boxes they are not heap allocations of their own, so both sources
     * are disowned. */
    int64_t           fd_box[2];
    int64_t           tm_box[2];
    TurReactor       *r;
    int               fd;
    int64_t           fd_src;
    int64_t           tm_src;
    struct TurLinger *prev, *next;
} TurLinger;

static void sock_close(int fd) {
#ifdef _WIN32
    closesocket((SOCKET)(intptr_t)fd);
#else
    close(fd);
#endif
}

/* Read and discard what the peer has sent.  Returns 1 when the linger is over
 * (EOF, or an error other than would-block) and 0 when it should keep waiting.
 * Bounded per call so a peer that keeps streaming cannot hold the reactor
 * thread; the timer bounds the linger as a whole. */
static int linger_drain(int fd) {
    char buf[1024];
    for (int i = 0; i < 64; i++) {
#ifdef _WIN32
        int n = recv((SOCKET)(intptr_t)fd, buf, (int)sizeof(buf), 0);
        if (n > 0) continue;
        if (n == 0) return 1;
        return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : 1;
#else
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) continue;
        if (n == 0) return 1;
        if (errno == EINTR) continue;
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : 1;
#endif
    }
    return 0;
}

static void linger_finish(TurLinger *l) {
    TurReactor *r = l->r;
    /* Removing a source from inside its own callback is the park path's
     * shape too: the struct stays put and the slot is recycled a poll later. */
    tur_reactor_remove(r, l->fd_src);
    tur_reactor_remove(r, l->tm_src);
    sock_close(l->fd);
    if (l->prev) l->prev->next = l->next; else r->lingers = l->next;
    if (l->next) l->next->prev = l->prev;
    r->lingers_len--;
    free(l);
}

/* (env, id, events, user) : nil -- matches TurFdCbFn */
static void linger_fd_cb(void *self, int64_t id, int64_t events, int64_t user) {
    (void)id; (void)events; (void)user;
    TurLinger *l = (TurLinger *)(intptr_t)((int64_t *)self)[1];
    if (linger_drain(l->fd)) linger_finish(l);
}

/* (env, id, user) : nil -- matches TurTimerCbFn */
static void linger_timer_cb(void *self, int64_t id, int64_t user) {
    (void)id; (void)user;
    linger_finish((TurLinger *)(intptr_t)((int64_t *)self)[1]);
}

int64_t tur_reactor_linger_close(void *rp, int64_t fd64, int64_t timeout_ms) {
    TurReactor *r = (TurReactor *)rp;
    int fd = (int)fd64;
    if (fd < 0) return 0;
#ifdef _WIN32
    shutdown((SOCKET)(intptr_t)fd, SD_SEND);
    u_long nb = 1;
    ioctlsocket((SOCKET)(intptr_t)fd, FIONBIO, &nb);
#else
    shutdown(fd, SHUT_WR);
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    /* Discard what has already arrived; if the peer has already closed as
     * well, there is nothing left to wait for. */
    if (!r || r->lingers_len >= TUR_LINGER_MAX || linger_drain(fd)) {
        sock_close(fd);
        return 0;
    }
    TurLinger *l = (TurLinger *)calloc(1, sizeof(TurLinger));
    if (!l) { sock_close(fd); return 0; }
    l->r         = r;
    l->fd        = fd;
    l->fd_box[0] = (int64_t)(intptr_t)linger_fd_cb;
    l->fd_box[1] = (int64_t)(intptr_t)l;
    l->tm_box[0] = (int64_t)(intptr_t)linger_timer_cb;
    l->tm_box[1] = (int64_t)(intptr_t)l;
    l->fd_src = tur_reactor_add_fd(r, fd, IO_EVENT_READ,
                                   (int64_t)(intptr_t)l->fd_box, NULL);
    if (l->fd_src < 0) { free(l); sock_close(fd); return 0; }
    tur_reactor_disown_cb(r, l->fd_src);
    l->tm_src = tur_reactor_add_timer(r, timeout_ms < 0 ? 0 : timeout_ms,
                                      (int64_t)(intptr_t)l->tm_box, NULL);
    if (l->tm_src < 0) {
        tur_reactor_remove(r, l->fd_src);
        free(l);
        sock_close(fd);
        return 0;
    }
    tur_reactor_disown_cb(r, l->tm_src);
    l->next = r->lingers;
    if (r->lingers) r->lingers->prev = l;
    r->lingers = l;
    r->lingers_len++;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

TurReactor *tur_reactor_new(void) {
#ifdef __EMSCRIPTEN__
    return NULL;
#endif
    IOBackend *backend = io_backend_new();
    if (!backend) return NULL;

    TurReactor *r = (TurReactor *)calloc(1, sizeof(TurReactor));
    if (!r) {
        io_backend_free(backend);
        return NULL;
    }

    r->sources = (TurReactorSource **)calloc(REACTOR_INITIAL_CAP,
                                              sizeof(TurReactorSource *));
    if (!r->sources) {
        io_backend_free(backend);
        free(r);
        return NULL;
    }

    r->backend      = backend;
    r->sources_cap  = REACTOR_INITIAL_CAP;
    r->sources_len  = 0;
    r->next_id      = 0;
    atomic_init(&r->stop_flag, 0);
    return r;
}

static void cleanup_source(TurReactor *r, TurReactorSource *src) {
    switch (src->kind) {
    case TUR_RSRC_FD:
        io_unregister(r->backend, src->fd);
        break;
    case TUR_RSRC_SIGNAL:
#ifdef IO_BACKEND_EPOLL
        io_unregister(r->backend, src->fd);
        close(src->fd);
        {
            sigset_t mask;
            sigemptyset(&mask);
            sigaddset(&mask, src->signum);
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
        }
#elif defined(TUR_REACTOR_HAVE_SIGNALS)
        io_unregister(r->backend, src->fd);
        sigaction(src->signum, &src->sig_old_sa, NULL);
        if (src->signum >= 0 && src->signum < NSIG)
            g_sig_write_fd[src->signum] = -1;
        close(src->fd);
        if (src->sig_write_fd >= 0) close(src->sig_write_fd);
#endif
        break;
    case TUR_RSRC_TIMER_ONESHOT:
    case TUR_RSRC_TIMER_INTERVAL:
    case TUR_RSRC_CHAN:
        break; /* no IOBackend registration */
    }
}

void tur_reactor_free(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    /* Owned callback boxes are freed exactly once: a program may register the
     * same heap closure box on several sources (e.g. one handler on an fd and
     * a timer), so track the pointers already freed and skip duplicates.  A
     * failed realloc degrades to "free without dedup", which is only unsafe if
     * a box is shared -- acceptably rare on a teardown path that is about to
     * exit anyway. */
    int64_t *freed = NULL;
    size_t   nfreed = 0, freed_cap = 0;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src) continue;
        if (src->active)
            cleanup_source(r, src);
        if (src->owns_cb && src->tur_cb) {
            bool seen = false;
            for (size_t j = 0; j < nfreed; j++)
                if (freed[j] == src->tur_cb) { seen = true; break; }
            if (!seen) {
                if (nfreed == freed_cap) {
                    size_t new_cap = freed_cap ? freed_cap * 2 : 8;
                    int64_t *grown = (int64_t *)realloc(freed, new_cap * sizeof(int64_t));
                    if (grown) { freed = grown; freed_cap = new_cap; }
                }
                if (nfreed < freed_cap) freed[nfreed++] = src->tur_cb;
                tur_reactor_release_box(src->tur_cb);
            }
        }
        free(src);
    }
    /* Boxes whose slot was recycled out from under them.  Same dedup: one
     * may still be shared with a source freed above. */
    for (size_t i = 0; i < r->orphan_len; i++) {
        int64_t box = r->orphan_boxes[i];
        if (!box) continue;
        bool seen = false;
        for (size_t j = 0; j < nfreed; j++)
            if (freed[j] == box) { seen = true; break; }
        if (seen) continue;
        if (nfreed == freed_cap) {
            size_t new_cap = freed_cap ? freed_cap * 2 : 8;
            int64_t *grown = (int64_t *)realloc(freed, new_cap * sizeof(int64_t));
            if (grown) { freed = grown; freed_cap = new_cap; }
        }
        if (nfreed < freed_cap) freed[nfreed++] = box;
        tur_reactor_release_box(box);
    }
    /* Sockets still lingering.  Their sources went with the loop above (the
     * boxes are inline and disowned), so only the fd and the record remain. */
    for (TurLinger *l = r->lingers; l; ) {
        TurLinger *next = l->next;
        sock_close(l->fd);
        free(l);
        l = next;
    }
    free(r->orphan_boxes);
    free(freed);
    free(r->sources);
    free(r->freelist);
    free(r->pending);
    io_backend_free(r->backend);
    free(r);
}

/* ------------------------------------------------------------------ */
/* Source registration                                                  */
/* ------------------------------------------------------------------ */

/* Take a recycled slot when one is available, else append a fresh one
 * (growing the array if needed). */
static TurReactorSource *alloc_source(TurReactor *r) {
    if (r->freelist_len > 0) {
        size_t slot = r->freelist[--r->freelist_len];
        TurReactorSource *reused = r->sources[slot];
        if (reused) {
            /* The outgoing occupant's cb box outlives the slot -- park it so
             * teardown still frees it exactly once (see orphan_boxes). */
            if (reused->owns_cb && reused->tur_cb) {
                if (r->orphan_len == r->orphan_cap) {
                    size_t nc = r->orphan_cap ? r->orphan_cap * 2 : 16;
                    int64_t *g = (int64_t *)realloc(r->orphan_boxes,
                                                    nc * sizeof(int64_t));
                    if (g) { r->orphan_boxes = g; r->orphan_cap = nc; }
                }
                if (r->orphan_len < r->orphan_cap)
                    r->orphan_boxes[r->orphan_len++] = reused->tur_cb;
            }
            /* Reset in place: the allocation is reused, never the contents. */
            memset(reused, 0, sizeof(*reused));
            reused->slot         = slot;
            reused->sig_write_fd = -1;
            return reused;
        }
    }
    if (r->sources_len == r->sources_cap) {
        size_t new_cap = r->sources_cap * 2;
        TurReactorSource **arr = (TurReactorSource **)realloc(
            r->sources, new_cap * sizeof(TurReactorSource *));
        if (!arr) return NULL;
        memset(arr + r->sources_cap, 0,
               (new_cap - r->sources_cap) * sizeof(TurReactorSource *));
        r->sources     = arr;
        r->sources_cap = new_cap;
    }
    TurReactorSource *src = (TurReactorSource *)calloc(1, sizeof(TurReactorSource));
    if (!src) return NULL;
    src->sig_write_fd = -1;
    src->slot = r->sources_len;
    r->sources[r->sources_len++] = src;
    return src;
}

static TurReactorSource *find_source(TurReactor *r, int64_t id) {
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (src && src->active && src->id == id)
            return src;
    }
    return NULL;
}

/*
 * Relinquish reactor ownership of a source's callback box: after this call the
 * reactor will NOT free the box at teardown.
 *
 * The public tur_reactor_add_* entry points take ownership of the cb box by
 * default (owns_cb = true) -- the emitted program builds a heap fat-closure,
 * hands it to the reactor, and never frees it, so the reactor reclaiming it at
 * tur_reactor_free is what makes reactor programs leak-clean.  A caller that
 * manages the box lifetime itself must opt OUT with this call, or the box is
 * double-freed.  Two such callers:
 *   - the httpd runtime, which caches its accept-callback box and frees it in
 *     its own teardown;
 *   - the internal park registrations, whose cb is an inline LocalFiber field
 *     (&lf->park_cb_fat), not a heap allocation -- freeing it corrupts the heap.
 * A no-op for id < 0 (a failed registration). Idempotent.
 */
void tur_reactor_disown_cb(void *rp, int64_t id) {
    TurReactor *r = (TurReactor *)rp;
    if (!r || id < 0) return;
    TurReactorSource *src = find_source(r, id);
    if (src) src->owns_cb = false;
}

int64_t tur_reactor_add_fd(void *rp, int64_t fd, int64_t events,
                            int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_FD;
    src->fd            = (int)fd;
    src->tur_cb        = tur_cb;
    src->owns_cb       = true;   /* default: reactor frees the box at teardown */
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;

    if (io_register(r->backend, src->fd, (int)events, fd_shim, src) < 0) {
        src->active = false;
        return -1;
    }
    return src->id;
}

int64_t tur_reactor_modify_fd(void *rp, int64_t id, int64_t new_events) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    TurReactorSource *src = find_source(r, id);
    if (!src) return -1;
    return (int64_t)io_modify(r->backend, src->fd, (int)new_events);
}

int64_t tur_reactor_remove(void *rp, int64_t id) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    TurReactorSource *src = find_source(r, id);
    if (!src) return -1;
    cleanup_source(r, src);
    src->active = false;
    /* Deferred: promoted to the freelist at the top of the next poll. */
    slot_push(&r->pending, &r->pending_len, &r->pending_cap, src->slot);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Timer registration (Phase R4)                                        */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_timer(void *rp, int64_t delay_ms,
                               int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_TIMER_ONESHOT;
    src->deadline_ms   = now_ms() + delay_ms;
    src->interval_ms   = 0;
    src->tur_cb        = tur_cb;
    src->owns_cb       = true;   /* default: reactor frees the box at teardown */
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

int64_t tur_reactor_add_interval(void *rp, int64_t first_ms, int64_t interval_ms,
                                  int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_TIMER_INTERVAL;
    src->deadline_ms   = now_ms() + first_ms;
    src->interval_ms   = interval_ms;
    src->tur_cb        = tur_cb;
    src->owns_cb       = true;   /* default: reactor frees the box at teardown */
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

/* ------------------------------------------------------------------ */
/* Signal registration (Phase R5)                                       */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_signal(void *rp, int64_t signum_i,
                                int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_SIGNAL;
    src->signum        = (int)signum_i;
    src->tur_cb        = tur_cb;
    src->owns_cb       = true;   /* default: reactor frees the box at teardown */
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = false;

#ifdef IO_BACKEND_EPOLL
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, (int)signum_i);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1;
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd < 0) return -1;
    src->fd = sfd;
    if (io_register(r->backend, sfd, IO_EVENT_READ, signal_shim, src) < 0) {
        close(sfd);
        return -1;
    }
    src->active = true;
    return src->id;

#elif defined(TUR_REACTOR_HAVE_SIGNALS)
    if (!g_sig_table_init) {
        for (int i = 0; i < NSIG; i++) g_sig_write_fd[i] = -1;
        g_sig_table_init = true;
    }
    int pipefds[2];
    if (pipe(pipefds) < 0) return -1;
    fcntl(pipefds[0], F_SETFL, fcntl(pipefds[0], F_GETFL) | O_NONBLOCK);
    fcntl(pipefds[1], F_SETFL, fcntl(pipefds[1], F_GETFL) | O_NONBLOCK);
    g_sig_write_fd[(int)signum_i] = pipefds[1];
    src->fd           = pipefds[0];
    src->sig_write_fd = pipefds[1];
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_self_pipe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction((int)signum_i, &sa, &src->sig_old_sa);
    if (io_register(r->backend, pipefds[0], IO_EVENT_READ, signal_shim, src) < 0) {
        sigaction((int)signum_i, &src->sig_old_sa, NULL);
        g_sig_write_fd[(int)signum_i] = -1;
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }
    src->active = true;
    return src->id;

#else
    /* WASM: signals not supported */
    return -1;
#endif
}

/* ------------------------------------------------------------------ */
/* Channel bridge registration (Phase R6)                               */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_add_chan(void *rp, void *chan,
                              int64_t tur_cb, void *tur_user_data) {
    TurReactor *r = (TurReactor *)rp;
    if (!r || !chan) return -1;

    TurReactorSource *src = alloc_source(r);
    if (!src) return -1;

    src->id            = r->next_id++;
    src->kind          = TUR_RSRC_CHAN;
    src->chan_ptr       = chan;
    src->tur_cb        = tur_cb;
    src->owns_cb       = true;   /* default: reactor frees the box at teardown */
    src->tur_user_data = (int64_t)(intptr_t)tur_user_data;
    src->active        = true;
    return src->id;
}

/* ------------------------------------------------------------------ */
/* Timer helpers                                                        */
/* ------------------------------------------------------------------ */

static bool chan_has_value(void *chan);

static int64_t cap_timeout(TurReactor *r, int64_t timeout_ms) {
    int64_t t = now_ms();
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active) continue;
        /* A channel watch is delivered by tick_chans, which runs only after
         * io_poll returns -- and nothing makes io_poll return for a channel:
         * the sender's reactor-wake is the only signal.  So a watch registered
         * on a channel that ALREADY holds a value (a value sent before the
         * watch, or a fiber re-parking on a channel the sender filled while it
         * ran) blocked io_poll forever with the value sitting there.  Do not
         * block while a watched channel can be delivered now. */
        if (src->kind == TUR_RSRC_CHAN) {
            if (chan_has_value(src->chan_ptr)) return 0;
            continue;
        }
        if (src->kind != TUR_RSRC_TIMER_ONESHOT &&
            src->kind != TUR_RSRC_TIMER_INTERVAL) continue;

        int64_t remaining = src->deadline_ms - t;
        if (remaining < 0) remaining = 0;
        if (timeout_ms < 0 || remaining < timeout_ms)
            timeout_ms = remaining;
    }
    return timeout_ms;
}

static int tick_timers(TurReactor *r) {
    int64_t t = now_ms();
    int fired = 0;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active) continue;
        if (src->kind != TUR_RSRC_TIMER_ONESHOT &&
            src->kind != TUR_RSRC_TIMER_INTERVAL) continue;
        if (t < src->deadline_ms) continue;

        call_tur_timer_cb(src->tur_cb, src->id, src->tur_user_data);
        fired++;

        if (src->kind == TUR_RSRC_TIMER_ONESHOT) {
            src->active = false;
        } else {
            src->deadline_ms += src->interval_ms;
            if (src->deadline_ms <= t)
                src->deadline_ms = t + src->interval_ms;
        }
    }
    return fired;
}

/* ------------------------------------------------------------------ */
/* Channel bridge tick (Phase R6)                                       */
/* ------------------------------------------------------------------ */

/*
 * Minimal ChanBlock layout -- must match the inline typedef used by
 * chan.tur and async-chan*.  Both this file and the generated C are compiled
 * into the same binary, so the platform's pthread struct sizes agree.
 */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int64_t        *buf;
    int64_t         head;
    int64_t         tail;
    int64_t         count;
    int64_t         cap;
    void           *recv_waiters;
    void           *send_waiters;
} TurChanBlock;

static bool chan_has_value(void *chan) {
    TurChanBlock *c = (TurChanBlock *)chan;
    pthread_mutex_lock(&c->lock);
    bool ready = c->count > 0;
    pthread_mutex_unlock(&c->lock);
    return ready;
}

static int tick_chans(TurReactor *r) {
    int fired = 0;
    for (size_t i = 0; i < r->sources_len; i++) {
        TurReactorSource *src = r->sources[i];
        if (!src || !src->active || src->kind != TUR_RSRC_CHAN) continue;
        TurChanBlock *c = (TurChanBlock *)src->chan_ptr;
        pthread_mutex_lock(&c->lock);
        if (c->count == 0) {
            pthread_mutex_unlock(&c->lock);
            continue;
        }
        int64_t v = c->buf[c->head];
        c->head = (c->head + 1) % c->cap;
        c->count--;
        pthread_cond_signal(&c->not_full);
        pthread_mutex_unlock(&c->lock);
        call_tur_chan_cb(src->tur_cb, src->id, v, src->tur_user_data);
        src->active = false; /* one-shot */
        fired++;
    }
    return fired;
}

/* ------------------------------------------------------------------ */
/* Running                                                              */
/* ------------------------------------------------------------------ */

int64_t tur_reactor_poll(void *rp, int64_t timeout_ms) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return -1;
    /* Slots freed during the PREVIOUS poll become reusable now: every loop
     * that could still have been holding one has returned.  See the
     * two-stage note on struct TurReactor. */
    for (size_t i = 0; i < r->pending_len; i++)
        slot_push(&r->freelist, &r->freelist_len, &r->freelist_cap,
                  r->pending[i]);
    r->pending_len = 0;
    int64_t capped = cap_timeout(r, timeout_ms);
    int fd_count = io_poll(r->backend, (int)capped);
    if (fd_count < 0) return -1;
    int timer_count = tick_timers(r);
    int chan_count  = tick_chans(r);
    return (int64_t)(fd_count + timer_count + chan_count);
}

void tur_reactor_run(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    atomic_store(&r->stop_flag, 0);
    for (;;) {
        if (atomic_load(&r->stop_flag)) break;

        bool has_active = false;
        for (size_t i = 0; i < r->sources_len; i++) {
            if (r->sources[i] && r->sources[i]->active) {
                has_active = true;
                break;
            }
        }
        if (!has_active) break;

        int64_t capped = cap_timeout(r, -1);
        io_poll(r->backend, (int)capped);
        tick_timers(r);
        tick_chans(r);
    }
}

void tur_reactor_stop(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    atomic_store(&r->stop_flag, 1);
    io_wake(r->backend);
}

void tur_reactor_wake(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return;
    io_wake(r->backend);
}

/* ================================================================== */
/* Local fiber driver (Phase F2-F7)                                    */
/*                                                                      */
/* A LocalFiberGroup runs cooperative fibers on top of one reactor      */
/* using the TurFiber stackful-coroutine primitive (fiber.c). It is     */
/* deliberately not thread-safe -- the group, its reactor, and every    */
/* fiber it owns live on a single OS thread (same rule as TurReactor).  */
/* ================================================================== */

/* Per-fiber park sentinels (mirror the documented stdlib contract). */
#define TUR_LOCAL_PARK_TIMEOUT (-2)
#define TUR_LOCAL_NOT_IN_FIBER (-1)

typedef struct LocalFiber {
    int64_t            id;
    TurFiber          *fiber;       /* underlying stackful coroutine */
    int64_t            tur_body;    /* Turmeric fat-closure (user)->nil */
    /* True when tur_body is a heap fat-closure box this fiber owns and must
     * free at group teardown (the default for tur_local_spawn). False for
     * callers that share one body box across many fibers and free it
     * themselves -- e.g. the httpd async server -- which opt out via
     * tur_local_disown_body. */
    bool               owns_body;
    void              *user_data;
    bool               done;        /* ran to completion */
    bool               in_ready;    /* currently linked into the ready queue */
    struct LocalFiberGroup *group;  /* owning group (for park callbacks) */
    /* Park state.  Each park callback is a C fat-closure matching the
     * reactor's fat-pointer convention: slot [0] is the C handler and slot [1]
     * is this LocalFiber*, recovered as `self`.
     *
     * There are TWO because the reactor calls fd/chan sources with four
     * arguments and timer sources with three, and one handler cannot carry
     * both types.  `tur_local_park_fd` registers both at once (an fd source
     * plus a companion timeout), so they must be separate storage rather than
     * one array rewritten per registration. */
    int64_t            park_cb_fat[2];        /* fd / chan wake */
    int64_t            park_timer_cb_fat[2];  /* timeout wake */
    int64_t            park_fd_src;     /* reactor source id, or -1 */
    int64_t            park_timer_src;  /* reactor source id, or -1 */
    int64_t            park_chan_src;   /* reactor source id, or -1 */
    int64_t            park_result;     /* value handed back from park */
    struct LocalFiber *ready_next;      /* ready-queue link */
    struct LocalFiber *all_next;        /* group-wide list link */
} LocalFiber;

struct LocalFiberGroup {
    TurReactor *reactor;        /* borrowed; not owned */
    LocalFiber *all_head;       /* every spawned fiber, done or not */
    LocalFiber *ready_head;     /* FIFO ready-queue head */
    LocalFiber *ready_tail;     /* FIFO ready-queue tail */
    LocalFiber *current;        /* fiber currently being resumed, or NULL */
    int64_t     next_id;
    int64_t     completed;      /* fibers that ran to completion */
    int         running;        /* re-entrancy guard for run-fibers */
};
typedef struct LocalFiberGroup LocalFiberGroup;

/* The group whose pump is currently active on this thread. Used by the fiber
 * trampoline (which only receives a TurFiber*) to recover the LocalFiber it is
 * running via group->current. */
static __thread LocalFiberGroup *tl_current_group = NULL;

/* ---- ready-queue helpers (FIFO) ---- */

static void ready_push(LocalFiberGroup *g, LocalFiber *lf) {
    if (lf->in_ready) return;
    lf->in_ready   = true;
    lf->ready_next = NULL;
    if (g->ready_tail)
        g->ready_tail->ready_next = lf;
    else
        g->ready_head = lf;
    g->ready_tail = lf;
}

static LocalFiber *ready_pop(LocalFiberGroup *g) {
    LocalFiber *lf = g->ready_head;
    if (!lf) return NULL;
    g->ready_head = lf->ready_next;
    if (!g->ready_head) g->ready_tail = NULL;
    lf->ready_next = NULL;
    lf->in_ready   = false;
    return lf;
}

/* Remove any reactor sources a parked fiber is waiting on. */
static void local_fiber_clear_park(LocalFiber *lf) {
    LocalFiberGroup *g = lf->group;
    if (lf->park_fd_src >= 0) {
        tur_reactor_remove(g->reactor, lf->park_fd_src);
        lf->park_fd_src = -1;
    }
    if (lf->park_timer_src >= 0) {
        tur_reactor_remove(g->reactor, lf->park_timer_src);
        lf->park_timer_src = -1;
    }
    if (lf->park_chan_src >= 0) {
        tur_reactor_remove(g->reactor, lf->park_chan_src);
        lf->park_chan_src = -1;
    }
}

void *tur_local_fiber_group_new(void *rp) {
    TurReactor *r = (TurReactor *)rp;
    if (!r) return NULL;
    LocalFiberGroup *g = (LocalFiberGroup *)calloc(1, sizeof(LocalFiberGroup));
    if (!g) return NULL;
    g->reactor = r;
    g->next_id = 0;
    return g;
}

void tur_local_fiber_group_free(void *gp) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g) return;
    /* Owned spawn-body boxes are freed exactly once: even under owning callers
     * two fibers could share a body box, so dedup like tur_reactor_free does.
     * Callers that manage the box themselves (httpd async, whose fibers share a
     * single body_closure it frees itself) opt out with tur_local_disown_body. */
    int64_t *freed = NULL;
    size_t   nfreed = 0, freed_cap = 0;
    LocalFiber *lf = g->all_head;
    while (lf) {
        LocalFiber *next = lf->all_next;
        /* Cancel any still-parked fiber: drop its reactor sources and free its
         * stack without resuming it (no cleanup hooks -- see local_fiber.h). */
        if (!lf->done)
            local_fiber_clear_park(lf);
        if (lf->fiber)
            tur_fiber_free(lf->fiber);
        if (lf->owns_body && lf->tur_body) {
            bool seen = false;
            for (size_t j = 0; j < nfreed; j++)
                if (freed[j] == lf->tur_body) { seen = true; break; }
            if (!seen) {
                if (nfreed == freed_cap) {
                    size_t new_cap = freed_cap ? freed_cap * 2 : 8;
                    int64_t *grown = (int64_t *)realloc(freed, new_cap * sizeof(int64_t));
                    if (grown) { freed = grown; freed_cap = new_cap; }
                }
                if (nfreed < freed_cap) freed[nfreed++] = lf->tur_body;
                tur_reactor_release_box(lf->tur_body);
            }
        }
        free(lf);
        lf = next;
    }
    free(freed);
    free(g);
}

/* True if the reactor still has at least one active source registered. */
static bool reactor_has_active_sources(TurReactor *r) {
    for (size_t i = 0; i < r->sources_len; i++) {
        if (r->sources[i] && r->sources[i]->active)
            return true;
    }
    return false;
}

/* TurFiber entry trampoline: recover the LocalFiber currently being resumed
 * (the driver sets group->current before each resume) and invoke its Turmeric
 * body fat-closure with the stored user-data pointer. */
static void local_fiber_trampoline(TurFiber *tf) {
    (void)tf;
    LocalFiberGroup *g = tl_current_group;
    LocalFiber *lf = g ? g->current : NULL;
    if (!lf || !lf->tur_body) return;
    int64_t *fat = (int64_t *)(intptr_t)lf->tur_body;
    ((TurFiberBodyFn)(intptr_t)fat[0])((void *)fat,
                                       (int64_t)(intptr_t)lf->user_data);
}

int64_t tur_local_spawn(void *gp, int64_t tur_body, void *user_data) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g) return -1;
    LocalFiber *lf = (LocalFiber *)calloc(1, sizeof(LocalFiber));
    if (!lf) return -1;
    lf->fiber = tur_fiber_new(local_fiber_trampoline, 0);
    if (!lf->fiber) {
        free(lf);
        return -1;
    }
    lf->id             = g->next_id++;
    lf->tur_body       = tur_body;
    lf->owns_body      = true;   /* default: group teardown frees the box */
    lf->user_data      = user_data;
    lf->group          = g;
    lf->done           = false;
    lf->park_fd_src    = -1;
    lf->park_timer_src = -1;
    lf->park_chan_src  = -1;
    /* Link into the group-wide list and the ready queue. */
    lf->all_next = g->all_head;
    g->all_head  = lf;
    ready_push(g, lf);
    return lf->id;
}

/*
 * Relinquish group ownership of a fiber's spawn-body box: after this call the
 * group will NOT free it at tur_local_fiber_group_free.  tur_local_spawn owns
 * the body box by default (the emitted program builds a heap fat-closure and
 * hands it off), so a caller that reuses one body box across many fibers and
 * frees it itself -- the httpd async server, whose per-request fibers all share
 * ha->body_closure -- must disown every such fiber, or the box is
 * multi-/double-freed.  A no-op for an unknown fiber id.
 */
void tur_local_disown_body(void *gp, int64_t fiber_id) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g || fiber_id < 0) return;
    for (LocalFiber *lf = g->all_head; lf; lf = lf->all_next) {
        if (lf->id == fiber_id) { lf->owns_body = false; return; }
    }
}

int64_t tur_reactor_run_fibers(void *gp) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g) return -1;
    /* Re-entrant invocation from inside one of this group's own fibers would
     * deadlock the pump -- reject it (open-question resolution: hard error). */
    if (g->running) return -1;

    g->running = 1;
    LocalFiberGroup *prev = tl_current_group;
    tl_current_group = g;
    /* Mirror tur_reactor_run: clear any stale stop request at entry. */
    atomic_store(&g->reactor->stop_flag, 0);

    for (;;) {
        if (atomic_load(&g->reactor->stop_flag)) break;

        /* Drain the ready queue. A resumed fiber runs until it parks (yields)
         * or returns; parks re-enter the queue via the reactor callbacks. */
        LocalFiber *lf;
        while (!atomic_load(&g->reactor->stop_flag) && (lf = ready_pop(g))) {
            g->current = lf;
            tur_fiber_resume(lf->fiber, NULL);
            g->current = NULL;
            if (tur_fiber_done(lf->fiber)) {
                lf->done = true;
                g->completed++;
            }
        }

        if (atomic_load(&g->reactor->stop_flag)) break;

        /* Stop when nothing can make further progress: the ready queue is
         * drained and the reactor has no active source to wake a parked fiber
         * (this also covers "group empty AND no remaining sources"). */
        if (!reactor_has_active_sources(g->reactor)) break;

        /* Block until a source fires; its callback re-readies a parked fiber
         * (or services a non-fiber reactor source). */
        tur_reactor_poll(g->reactor, -1);
    }

    g->current = NULL;
    g->running = 0;
    tl_current_group = prev;
    return g->completed;
}

/* ---- park bridge (Phase F4-F5) ---- */

/*
 * Wake a parked fiber.  Shared body behind the two entry points below.
 *
 * `arg2` is the fd event mask or the received chan value; a timer wake has no
 * such argument and is disambiguated by source id instead.
 */
static void local_park_wake(void *self, int64_t id, int64_t arg2) {
    int64_t *fat = (int64_t *)self;
    LocalFiber *lf = (LocalFiber *)(intptr_t)fat[1];
    LocalFiberGroup *g = lf->group;

    if (id == lf->park_timer_src) {
        lf->park_result = TUR_LOCAL_PARK_TIMEOUT;
    } else {
        /* fd: arg2 is the event mask; chan: arg2 is the received value. */
        lf->park_result = arg2;
    }
    /* One-shot: drop the fd/chan source and any companion timeout source. */
    local_fiber_clear_park(lf);
    ready_push(g, lf);
}

/*
 * Wake callbacks for a parked fiber. Registered with the reactor as a C
 * fat-closure: park_cb_fat[0] is one of these handlers, park_cb_fat[1] is the
 * LocalFiber*.
 *
 * There are two because the reactor calls fd/chan sources with four arguments
 * and timer sources with three.  A single four-parameter handler used to serve
 * both, on the reasoning that the extra argument goes unread so "the differing
 * arity is harmless" -- true of the ABI, false of the language: an indirect
 * call through a mismatched function-pointer type is UB regardless of whether
 * the callee reads the extra slot, and `-fsanitize=function` reports it (see
 * docs/archive/reactor-fd-callback-fn-ptr-type-mismatch.md).  Each entry point
 * now matches its call site's typedef exactly.
 */
static void local_park_wake_fd_cb(void *self, int64_t id, int64_t arg2,
                                  int64_t user) {
    (void)user;
    local_park_wake(self, id, arg2);
}

static void local_park_wake_timer_cb(void *self, int64_t id, int64_t user) {
    (void)user;
    /* A timer wake carries no event mask; local_park_wake ignores arg2 on the
     * timeout path, which `id == lf->park_timer_src` selects. */
    local_park_wake(self, id, 0);
}

int64_t tur_local_park_fd(void *gp, int64_t fd, int64_t events,
                          int64_t timeout_ms) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g) return TUR_LOCAL_NOT_IN_FIBER;
    LocalFiber *lf = g->current;
    if (!lf) return TUR_LOCAL_NOT_IN_FIBER; /* not inside a fiber */

    lf->park_cb_fat[0] = (int64_t)(intptr_t)local_park_wake_fd_cb;
    lf->park_cb_fat[1] = (int64_t)(intptr_t)lf;
    int64_t cb = (int64_t)(intptr_t)lf->park_cb_fat;

    lf->park_result = TUR_LOCAL_NOT_IN_FIBER;
    lf->park_fd_src = tur_reactor_add_fd(g->reactor, fd, events, cb, NULL);
    if (lf->park_fd_src < 0)
        return TUR_LOCAL_NOT_IN_FIBER; /* registration failed */
    /* cb is &lf->park_cb_fat (inline field), not a heap box -- disown. */
    tur_reactor_disown_cb(g->reactor, lf->park_fd_src);

    if (timeout_ms >= 0) {
        /* A timer source is invoked with the 3-arg timer type, so it gets its
         * own fat closure rather than sharing the fd one above. */
        lf->park_timer_cb_fat[0] = (int64_t)(intptr_t)local_park_wake_timer_cb;
        lf->park_timer_cb_fat[1] = (int64_t)(intptr_t)lf;
        int64_t tcb = (int64_t)(intptr_t)lf->park_timer_cb_fat;
        lf->park_timer_src =
            tur_reactor_add_timer(g->reactor, timeout_ms, tcb, NULL);
        /* On timer-registration failure just park without a timeout. */
        tur_reactor_disown_cb(g->reactor, lf->park_timer_src);
    }

    /* Yield to the driver; resumed by one of the local_park_wake_* handlers. */
    tur_fiber_yield(lf->fiber, NULL);
    return lf->park_result;
}

/* local-park-chan's real entry.  Returns 0 and stores the received value in
 * *value_out, or TUR_LOCAL_NOT_IN_FIBER (no group, not inside a fiber of it,
 * or the watch could not be registered) and leaves *value_out alone.  The
 * status is separate from the value because a channel carries any int64 --
 * folding "not in a fiber" into the value, as tur_local_park_chan does, makes
 * a received -1 indistinguishable from the error. */
int tur_local_park_chan_status(void *gp, void *chan, int64_t *value_out) {
    LocalFiberGroup *g = (LocalFiberGroup *)gp;
    if (!g) return TUR_LOCAL_NOT_IN_FIBER;
    LocalFiber *lf = g->current;
    if (!lf) return TUR_LOCAL_NOT_IN_FIBER; /* not inside a fiber */

    /* A chan source is invoked with the 4-arg fd/chan type. */
    lf->park_cb_fat[0] = (int64_t)(intptr_t)local_park_wake_fd_cb;
    lf->park_cb_fat[1] = (int64_t)(intptr_t)lf;
    int64_t cb = (int64_t)(intptr_t)lf->park_cb_fat;

    lf->park_result   = TUR_LOCAL_NOT_IN_FIBER;
    lf->park_chan_src = tur_reactor_add_chan(g->reactor, chan, cb, NULL);
    if (lf->park_chan_src < 0)
        return TUR_LOCAL_NOT_IN_FIBER;
    /* cb is &lf->park_cb_fat (inline field), not a heap box -- disown. */
    tur_reactor_disown_cb(g->reactor, lf->park_chan_src);

    /* Yield to the driver; resumed by local_park_wake_fd_cb with the value.
     * Like all reactor channel watchers, prompt delivery on the same thread
     * needs a reactor-wake after the chan-send (see reactor-add-chan). */
    tur_fiber_yield(lf->fiber, NULL);
    if (value_out) *value_out = lf->park_result;
    return 0;
}

/* The original single-return form, kept for ABI compatibility: the value, or
 * TUR_LOCAL_NOT_IN_FIBER (-1), which a received -1 cannot be told apart from. */
int64_t tur_local_park_chan(void *gp, void *chan) {
    int64_t v = TUR_LOCAL_NOT_IN_FIBER;
    if (tur_local_park_chan_status(gp, chan, &v) != 0)
        return TUR_LOCAL_NOT_IN_FIBER;
    return v;
}
