/* r7gc.c -- the r7rs-gc experiment: a conservative mark-sweep collector for a
 * compiled `#lang r7rs` program (docs/archive/r7rs-gc-plan.md).
 *
 * Why it exists: a Scheme value is a `:heap` box, and the memory model never
 * frees one (docs/guides/gc-guide.md), so a Scheme program's memory only grows
 * (docs/reported/r7rs-heap-data-never-reclaimed.md).  Shared, mutable, cyclic
 * data is the case only a tracing collector handles well.
 *
 * How it plugs in: the emitter pastes this file into the program's
 * translation unit, right after the preamble's system includes and ahead of
 * everything that allocates, and then
 * redirects that unit's allocator -- `malloc`, `calloc`, `realloc`, `free`,
 * `strdup`, `strndup`, and the region fallbacks -- to the `tur_gc_*` entry
 * points below (emit_module.c, emit_r7rs_gc_prologue).  Every object the
 * program itself allocates -- Scheme data, closure environments, DK frames,
 * continuation images, vector buffers, the prelude's C -- lands on this heap.
 *
 * Conservative: any aligned word that points into an allocated object (its
 * first byte or any byte inside it) keeps the object alive.  The roots are
 *   - every registered thread's C stack and registers: the collecting
 *     thread's from its own frame, with the callee-saved registers spilled
 *     into a jmp_buf there; a thread parked in a blocking call from the point
 *     it parked at, with the registers it spilled there; any other thread
 *     from the frame of the signal handler that stopped it (below), with the
 *     registers the kernel saved on its stack;
 *   - each thread's thread-local runtime state (the emitted runtime's
 *     TUR_THREAD_LOCAL variables), whose addresses the thread registers when
 *     it starts (tur_rt_tls_roots, emitted after the preamble), and every
 *     value it stored through pthread_setspecific (dynamic variables, the
 *     ^thread-local block), which libc keeps where no scan reaches;
 *   - the executable's writable data and bss (`__data_start` .. `_end`);
 *   - the used bytes of every live and retired region generation on every
 *     thread (the region runtime's ownership registry);
 *   - each thread's allocation cache: slots it holds but has not handed out
 *     are kept, not scanned.
 * Objects are scanned whole, word by word.
 *
 * Threads (docs/archive/r7rs-gc-threads-plan.md, stages A to C): threads
 * run in parallel.  Allocation is a per-thread cache of slots per size
 * class, refilled from the shared free lists under `G->heap`; a collection
 * takes `G->world` (one collector at a time), then `G->heap` (no thread is
 * inside the allocator's slow path), then stops every other thread: a thread
 * parked in a blocking call (tur_gc_park, the release points at the end of
 * this file) has already spilled its registers and stack pointer and is left
 * where it is; any other thread is sent a signal whose handler spills the
 * same and waits, on its own stack, for the resume signal (on macOS, for
 * the collector to clear its flag: tur_gc_stop_handler says why).  This is the
 * Boehm collector's design: a thread can be stopped anywhere, so no safe
 * points are compiled in.  The stop signal restarts the interrupted system
 * call (SA_RESTART); the few that do not restart are the wrapped ones, whose
 * wrappers retry an EINTR the collector caused.  A thread's record outlives
 * its start routine until nothing of the thread is left: a joinable thread's
 * until the join, a detached one's until its key destructors have run (the
 * collector's own key, re-armed to run last).  A fork takes every collector
 * lock first, so the child never inherits one another thread held.  A thread
 * must never be stopped holding a lock the collector's own stop takes: on
 * macOS pthread_kill takes libpthread's thread-list lock, so the calls that
 * hold it (pthread_create, pthread_detach, pthread_join) are release points
 * too.
 *
 * The runtime archive (libturt_runtime.a: the HAMT, rc<T>, strings, symbols)
 * allocates through the hook in src/runtime/rt_alloc.h, which the
 * constructor below points at this heap, so a Scheme value kept in a
 * Turmeric map or cell is scanned through the node that holds it.  What it
 * still cannot see, and so must not be relied on (the plan's limits): memory
 * libc allocates, the trail's `__thread`-rooted arrays (trail.c), and a
 * thread the unit did not start (a library's own thread calling back in
 * stops here with the reason).  A pure Scheme program allocates nothing
 * there that points back.
 *
 * Linux/glibc and macOS.  Elsewhere every entry point is the libc call it
 * replaces, and nothing is collected.
 *
 * Knobs (environment, read once):
 *   TUR_GC_STATS=1       print collections, bytes freed, the live size at the
 *                        last collection, and the heap now and at its peak.
 *   TUR_GC_TORTURE=N     collect on every Nth allocation (1 = every one): the
 *                        test mode that turns a missing root into a crash.
 *   TUR_GC_TORTURE_SCALE=R  with TUR_GC_TORTURE, stretch the interval to
 *                        the live object count / R when that is longer, so
 *                        a program holding a large structure is not
 *                        re-marked every N allocations (quadratic in its
 *                        size).  Small heaps keep the N interval.  Off (0)
 *                        by default.
 *   TUR_GC_THRESHOLD=B   bytes allocated between collections (floor; default
 *                        8 MiB, raised to the live size after each one). */

#pragma GCC diagnostic ignored "-Wunused-function"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

/* Off under `tur jit` (TUR_JIT_ENGINE comes from the engine's prelude): a
 * JIT'd program's globals live in memory MIR allocates, not in the
 * executable's data segment, so the root scan would miss them and free live
 * objects.  The entry points stay; they are libc there. */
#if ((defined(__linux__) && defined(__GLIBC__)) || defined(__APPLE__)) && !defined(TUR_JIT_ENGINE)
#define TUR_GC_ON 1
#include <sys/mman.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
/* Every header that declares a call the macros at the end of this file
 * wrap, included here so that a later include of it is a no-op (guarded) and
 * never a prototype the function-like macro would mangle. */
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <semaphore.h>
#if defined(__linux__)
#include <sys/epoll.h>
#else
#include <sys/event.h>
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#if defined(__APPLE__)
/* The data roots are the main image's writable segments, walked from its
 * Mach-O header (there is no __data_start/_end pair to bracket them). */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#endif
#else
#define TUR_GC_ON 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#define TUR_GC_NOASAN __attribute__((no_sanitize_address))
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TUR_GC_NOASAN __attribute__((no_sanitize_address))
#endif
#endif
#ifndef TUR_GC_NOASAN
#define TUR_GC_NOASAN
#endif

#if TUR_GC_ON

#define TUR_GC_CHUNK   ((uintptr_t)1 << 16)          /* 64 KiB, aligned */
#define TUR_GC_MAXSMALL 32768
#define TUR_GC_NCLASS  36

/* The stop-the-world signals: Boehm's choices, which no library sends and
 * the kernel sends only under a resource limit the program set itself. */
#if defined(__linux__)
#define TUR_GC_SIG_STOP   SIGPWR
#define TUR_GC_SIG_RESUME SIGXCPU
#else
#define TUR_GC_SIG_STOP   SIGXCPU
#define TUR_GC_SIG_RESUME SIGXFSZ
#endif

/* Up to half a chunk.  The large classes matter for continuations: a stack
 * image is a few KiB, and as a large object each one cost a 64 KiB mapping. */
static const uint32_t tur_gc_class_size[TUR_GC_NCLASS] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384,
    448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048, 2560, 3072,
    3584, 4096, 5120, 6144, 8192, 10912, 13104, 16384, 21840, 32768
};

/* One chunk (small objects of one size) or one large object (one or more
 * chunks).  Descriptors and bitmaps live in mmap'd metadata, never on the
 * collected heap and never in the data segment the collector scans. */
typedef struct tur_gc_page {
    uintptr_t base;
    size_t    size;       /* slot size, or the large object's byte size */
    uint32_t  nslots;     /* 1 for a large object */
    uint32_t  nchunks;    /* 1 for a small page */
    uint32_t  cls;        /* size class, or TUR_GC_NCLASS for large */
    uint32_t  used;       /* slots handed out so far (bump) */
    uint64_t  inv;        /* 2^48 / size + 1: `(off * inv) >> 48` is `off / size`
                           * for every offset in a chunk, without a divide
                           * (tur_gc_slot_of).  0 for a large object. */
    uint64_t *alloc;      /* bit per slot: allocated (to the program, or to a thread's cache) */
    uint64_t *mark;       /* bit per slot: reached this collection */
    struct tur_gc_page *next;   /* every page, for the sweep */
    bool      dead;       /* a released large object's descriptor */
} tur_gc_page;

/* One registered thread.  Records live in mmap'd metadata: never scanned as
 * data (the collector reads the fields it wants), never collected, reused
 * from a free list once the thread is retired (joined, or gone if detached).
 * The registry (the list, and `tid`, `started`, `done`, `detached`, `gone`)
 * is under G->world; `parked`, `stopped` and `gc_intr` are atomics the
 * thread and the collector share. */
typedef struct tur_gc_thread {
    struct tur_gc_thread *next;
    pthread_t      tid;          /* written by the creator and by the thread, both under G->world */
    unsigned long  gen;          /* bumped at each retirement, so a creator can tell its record was reused */
    unsigned char *stack_base;   /* the top of its stack, taken on the thread */
    unsigned char *stack_sp;     /* its stack pointer at its last park */
    unsigned char *sig_sp;       /* its stack pointer in the stop handler */
    unsigned char *os_sp;        /* where it left its own stack for a fiber's, or NULL */
    int            fiber_depth;
    int            park_depth;   /* nesting of parks; `parked` follows the outermost */
    volatile int   parked;       /* in a blocking call: regs and stack_sp are current */
    volatile int   stopped;      /* in the stop handler, waiting for the resume signal */
    volatile int   gc_intr;      /* the stop signal landed since the wrapper last cleared it */
    volatile int   in_stop;      /* macOS: inside the stop handler's wait (tur_gc_stop_handler) */
    int            stop_state;   /* this collection: 0 untouched, 1 signaled, 2 parked */
    /* Aligned, since the scan reads aligned words.  macOS's jmp_buf is an
     * int array: left to its own alignment, `regs` sat at 4 mod 8, every
     * register saved in it straddled two of the words the scan read, and a
     * parked thread's registers were never seen (parked-snapshot-unaligned). */
    jmp_buf        regs __attribute__((aligned(16)));      /* its callee-saved registers, spilled at the park */
    jmp_buf        sig_regs __attribute__((aligned(16)));  /* the same, spilled by the stop handler */
    uintptr_t      regs_fp;      /* the frame pointer at the park, as a plain word (TUR_GC_FP) */
    uintptr_t      sig_fp;       /* the same, in the stop handler */
    void         **tls_roots;    /* addresses of its TUR_THREAD_LOCAL variables */
    size_t        *tls_sizes;
    size_t         n_tls, cap_tls;
    /* Its pthread_setspecific values, one per key, written only by the thread
     * (tur_gc_setspecific), in an order a stop at any instruction reads
     * consistently.  Roots until the record is retired, since the key
     * destructors that read them run after the start routine returns. */
    pthread_key_t *spec_keys;
    void         **spec_vals;
    size_t         n_spec, cap_spec;
    void          *cache[TUR_GC_NCLASS];   /* its slots, per class, linked through their first word */
    bool           has_tid;      /* `tid` is set */
    bool           started;      /* it took the world once: its stack and TLS are roots */
    bool           done;         /* fn returned: `result` and its key values are roots, until retired */
    bool           detached;     /* retired when gone, not by a join */
    bool           gone;         /* its key destructors have run: nothing of it is left */
    int            exit_rounds;  /* key-destructor rounds tur_gc_thread_gone has seen */
    void *(*fn)(void *);
    void          *arg;          /* a root until the thread starts */
    void          *result;       /* a root from `done` until the join */
} tur_gc_thread;

typedef struct tur_gc_state {
    bool       ready, off;
    uintptr_t  lo, hi;                 /* heap bounds, for a fast reject (under heap) */
    tur_gc_page *pages;
    /* chunk index (base >> 16) -> page, open addressing, power-of-two size */
    uintptr_t *map_key;
    tur_gc_page **map_val;
    size_t     map_cap, map_n;
    void      *freelist[TUR_GC_NCLASS];
    tur_gc_page *bump[TUR_GC_NCLASS];  /* the page being carved for a class */
    /* metadata bump allocator */
    unsigned char *meta, *meta_end;
    pthread_mutex_t meta_lock;
    /* mark stack */
    uintptr_t *mstack;
    size_t     mstack_n, mstack_cap;
    /* accounting.  `since`, `threshold` and `off` are read on every
     * allocation with no lock held, so every access to them is atomic. */
    size_t     since, threshold, floor, live, heap_bytes, heap_peak;
    size_t     n_collect, freed_total;
    unsigned long torture;
    volatile unsigned long torture_count;
    unsigned long torture_scale;       /* TUR_GC_TORTURE_SCALE; 0 = a fixed interval */
    volatile unsigned long torture_due; /* torture_count at which the next one is due (scaled) */
    size_t     live_objs;              /* objects kept by the last collection */
    bool       stats;
    /* threads */
    pthread_mutex_t world;             /* the registry; held by the collector for a collection */
    pthread_mutex_t heap;              /* the free lists, the bump pages, the chunk map, the bounds */
    tur_gc_thread *threads;            /* every registered thread, the initial one included */
    tur_gc_thread *free_threads;       /* retired threads' records, for reuse */
    volatile long  acks;               /* threads that have reached the stop handler */
    pthread_key_t  exit_key;           /* each thread's record, for tur_gc_thread_gone */
} tur_gc_state;

static tur_gc_state *tur_gc_G;          /* points into mmap'd metadata */
/* The calling thread's record.  A real thread-local: it holds no heap
 * pointer and is read on every allocation. */
static __thread tur_gc_thread *tur_gc_self;
/* A fiber can yield on one worker and resume on another, and clang keeps a
 * thread-local's address in a register for the rest of a function call
 * (TUR_TLS_FRESH, emit_module.c).  An allocation after such a move took the
 * first worker's record and popped its cache while that worker popped it
 * too: one slot handed out twice (the migration fixture under torture, on
 * macOS).  So the allocator and free read the record through TUR_TLS_FRESH's
 * accessor, TUR_GC_SELF_FRESH(), and so does every other read the program's
 * own code reaches: tur_gc_stop_world from an allocation (stale, it skipped
 * the wrong thread and left the one it was on running through the
 * collection), the park and unpark around a blocking call (stale, a fiber
 * that parked on one worker unparked the other's record and stayed parked,
 * its stack scanned from a point it had long left), the EINTR helpers that
 * run with them, and the thread and key wrappers.  The stop handler, the
 * collector's own frames and a thread's start and end run on the thread
 * they read, never inlined into a fiber's code, and keep the plain read. */
#if defined(TUR_TLS_FRESH)
TUR_TLS_FRESH(tur_gc_thread *, tur_gc_self, tur_gc_self__at);
#define TUR_GC_SELF_FRESH() (*tur_gc_self__at())
#else
#define TUR_GC_SELF_FRESH() tur_gc_self
#endif

/* The thread-local roots of the emitted runtime: defined after the preamble
 * (emit_module.c, emit_r7rs_gc_tls_roots), it calls `add` once per
 * TUR_THREAD_LOCAL variable with the calling thread's instance. */
static void tur_rt_tls_roots(void (*add)(void *p, size_t n));

#define TUR_GC_LOAD(p)     __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define TUR_GC_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)

/* The frame pointer, read as a plain word.  Every register spill below is a
 * setjmp, and glibc's x86-64 setjmp stores rbp (with rsp and rip)
 * POINTER-MANGLED -- xor'd with a per-process secret and rotated -- so a root
 * a function kept in rbp across a park was never in the words the scan read.
 * At -O2 rbp is an ordinary callee-saved register, and the wrapper that
 * parks saves only what it uses itself, so the caller's rbp stays in rbp
 * through the park: main's freshly made FiberBlock, held in rbp across the
 * contended scheduler lock inside spawn, was freed with its stack under a
 * torture collection, and the queue then carried a dangling block
 * (docs/archive/r7rs-gc-torture-fiber-thread-cases-crash.md).  The other
 * ports keep the frame pointer plain (aarch64 mangles lr and sp only), and
 * the stop handler's kernel frame carries every register besides; this read
 * closes the x86-64 park and the collector's own spill, and costs one move. */
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#  define TUR_GC_FP(out) __asm__ volatile ("movq %%rbp, %0" : "=r"(out))
#else
#  define TUR_GC_FP(out) ((out) = 0)
#endif

static void *tur_gc_os(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { fputs("tur: r7rs-gc: out of memory\n", stderr); abort(); }
    return p;
}

/* Metadata: under its own lock, since thread records are made under
 * G->world and pages under G->heap. */
static void *tur_gc_meta(size_t n) {
    tur_gc_state *G = tur_gc_G;
    n = (n + 15) & ~(size_t)15;
    pthread_mutex_lock(&G->meta_lock);
    if (!G->meta || (size_t)(G->meta_end - G->meta) < n) {
        size_t sz = n > (1u << 20) ? n : (1u << 20);
        G->meta = (unsigned char *)tur_gc_os(sz);
        G->meta_end = G->meta + sz;
    }
    void *p = G->meta;
    G->meta += n;
    pthread_mutex_unlock(&G->meta_lock);
    return p;
}

/* A chunk-aligned run of `n` chunks. */
static uintptr_t tur_gc_chunks(size_t n) {
    size_t len = n * TUR_GC_CHUNK;
    unsigned char *raw = (unsigned char *)tur_gc_os(len + TUR_GC_CHUNK);
    uintptr_t base = ((uintptr_t)raw + TUR_GC_CHUNK - 1) & ~(TUR_GC_CHUNK - 1);
    size_t head = base - (uintptr_t)raw;
    if (head) munmap(raw, head);
    size_t tail = TUR_GC_CHUNK - head;
    if (tail) munmap((void *)(base + len), tail);
    return base;
}

static size_t tur_gc_hash(uintptr_t k, size_t cap) {
    k ^= k >> 17; k *= 0x9E3779B97F4A7C15ull; k ^= k >> 29;
    return (size_t)k & (cap - 1);
}

/* The chunk map: every reader and writer holds G->heap (the allocator's
 * slow path, free, realloc, and the collector, which takes G->heap before
 * it stops the world). */
static void tur_gc_map_put(uintptr_t key, tur_gc_page *pg);
static void tur_gc_map_grow(void) {
    tur_gc_state *G = tur_gc_G;
    size_t oc = G->map_cap, nc = oc ? oc * 2 : 1024;
    uintptr_t *ok = G->map_key; tur_gc_page **ov = G->map_val;
    G->map_key = (uintptr_t *)tur_gc_os(nc * sizeof(uintptr_t));
    G->map_val = (tur_gc_page **)tur_gc_os(nc * sizeof(tur_gc_page *));
    G->map_cap = nc; G->map_n = 0;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ov[i]) tur_gc_map_put(ok[i], ov[i]);
    if (ok) { munmap(ok, oc * sizeof(uintptr_t)); munmap(ov, oc * sizeof(tur_gc_page *)); }
}
static void tur_gc_map_put(uintptr_t key, tur_gc_page *pg) {
    tur_gc_state *G = tur_gc_G;
    if ((G->map_n + 1) * 2 > G->map_cap) tur_gc_map_grow();
    size_t i = tur_gc_hash(key, G->map_cap);
    while (G->map_key[i] && G->map_key[i] != key) i = (i + 1) & (G->map_cap - 1);
    if (!G->map_key[i]) G->map_n++;
    G->map_key[i] = key;
    G->map_val[i] = pg;
}
static tur_gc_page *tur_gc_map_get(uintptr_t key) {
    tur_gc_state *G = tur_gc_G;
    if (!G->map_cap) return NULL;
    size_t i = tur_gc_hash(key, G->map_cap);
    while (G->map_key[i]) {
        if (G->map_key[i] == key) return G->map_val[i];
        i = (i + 1) & (G->map_cap - 1);
    }
    return NULL;
}

/* The top of the calling thread's stack: the same two calls the prelude's
 * r7k_stack_base makes (glibc and macOS). */
static unsigned char *tur_gc_stack_base_here(void) {
    unsigned char *base = NULL;
#if defined(__APPLE__)
    base = (unsigned char *)pthread_get_stackaddr_np(pthread_self());
#else
    pthread_attr_t a; void *addr = NULL; size_t sz = 0;
    extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        if (pthread_attr_getstack(&a, &addr, &sz) == 0 && addr)
            base = (unsigned char *)addr + sz;
        pthread_attr_destroy(&a);
    }
#endif
    return base;
}

/* Under G->world. */
static tur_gc_thread *tur_gc_thread_new(void) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_thread *t = G->free_threads;
    if (t) {
        G->free_threads = t->next;
        void **roots = t->tls_roots; size_t *sizes = t->tls_sizes; size_t cap = t->cap_tls;
        pthread_key_t *keys = t->spec_keys; void **vals = t->spec_vals; size_t scap = t->cap_spec;
        unsigned long gen = t->gen;
        memset(t, 0, sizeof *t);
        t->tls_roots = roots; t->tls_sizes = sizes; t->cap_tls = cap;
        t->spec_keys = keys; t->spec_vals = vals; t->cap_spec = scap;
        t->gen = gen;
    } else {
        t = (tur_gc_thread *)tur_gc_meta(sizeof *t);
        memset(t, 0, sizeof *t);
    }
    return t;
}

/* Under G->world: take a record out of the registry for reuse.  Nothing of
 * its thread is a root any more. */
static void tur_gc_retire(tur_gc_thread *t) {
    tur_gc_state *G = tur_gc_G;
    for (tur_gc_thread **pp = &G->threads; *pp; pp = &(*pp)->next)
        if (*pp == t) { *pp = t->next; break; }
    t->gen++;
    t->next = G->free_threads; G->free_threads = t;
}

/* Under G->world, on the thread itself. */
static void tur_gc_add_tls_root(void *p, size_t n) {
    tur_gc_thread *t = tur_gc_self;
    if (!t) return;
    if (t->n_tls == t->cap_tls) {
        size_t nc = t->cap_tls ? t->cap_tls * 2 : 16;
        void **nr = (void **)tur_gc_meta(nc * sizeof *nr);
        size_t *ns = (size_t *)tur_gc_meta(nc * sizeof *ns);
        if (t->n_tls) {
            memcpy(nr, t->tls_roots, t->n_tls * sizeof *nr);
            memcpy(ns, t->tls_sizes, t->n_tls * sizeof *ns);
        }
        t->tls_roots = nr; t->tls_sizes = ns; t->cap_tls = nc;
    }
    t->tls_roots[t->n_tls] = p;
    t->tls_sizes[t->n_tls] = n;
    t->n_tls++;
}

static void tur_gc_stop_handler(int sig);
static void tur_gc_resume_handler(int sig);
static void tur_gc_thread_gone(void *raw);
static void tur_gc_atfork_prepare(void);
static void tur_gc_atfork_parent(void);
static void tur_gc_atfork_child(void);

static void tur_gc_init(void) {
    tur_gc_state *G = (tur_gc_state *)tur_gc_os(sizeof(tur_gc_state));
    memset(G, 0, sizeof *G);
    tur_gc_G = G;
    const char *e;
    G->floor = (size_t)8 << 20;
    if ((e = getenv("TUR_GC_THRESHOLD")) && atol(e) > 0) G->floor = (size_t)atol(e);
    G->threshold = G->floor;
    if ((e = getenv("TUR_GC_TORTURE")) && atol(e) > 0) G->torture = (unsigned long)atol(e);
    if ((e = getenv("TUR_GC_TORTURE_SCALE")) && atol(e) > 0) G->torture_scale = (unsigned long)atol(e);
    G->torture_due = G->torture;
    G->stats = (e = getenv("TUR_GC_STATS")) && e[0] == '1';
    G->mstack_cap = 1u << 16;
    G->mstack = (uintptr_t *)tur_gc_os(G->mstack_cap * sizeof(uintptr_t));
    pthread_mutex_init(&G->meta_lock, NULL);
    pthread_mutex_init(&G->world, NULL);
    pthread_mutex_init(&G->heap, NULL);
    /* The stop signal's handler blocks everything but the resume signal
     * while it runs (its sigsuspend unblocks that one), and restarts the
     * system call it interrupted. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = tur_gc_stop_handler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(TUR_GC_SIG_STOP, &sa, NULL);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = tur_gc_resume_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(TUR_GC_SIG_RESUME, &sa, NULL);
    /* Created before any key the program makes, so its destructor runs
     * first in a round; it re-arms itself to run in the next one, after
     * theirs (tur_gc_thread_gone). */
    if (pthread_key_create(&G->exit_key, tur_gc_thread_gone) != 0) {
        fputs("tur: r7rs-gc: pthread_key_create failed\n", stderr);
        abort();
    }
    pthread_atfork(tur_gc_atfork_prepare, tur_gc_atfork_parent, tur_gc_atfork_child);
    /* The initial thread: registered first.  Its thread-local roots are
     * added by the constructor below, once the emitted runtime's variables
     * exist to take the address of. */
    pthread_mutex_lock(&G->world);
    tur_gc_thread *t = tur_gc_thread_new();
    t->tid = pthread_self();
    t->has_tid = true;
    t->stack_base = tur_gc_stack_base_here();
    t->started = true;
    t->next = G->threads; G->threads = t;
    tur_gc_self = t;
    /* No stack base, no collection: every object is simply kept. */
    if (!t->stack_base) TUR_GC_STORE(&G->off, true);
    pthread_mutex_unlock(&G->world);
    G->ready = true;
}

/* The one rule a thread must keep: it touches the heap only between its
 * registration and its end, and never while parked.  A thread the collector
 * never registered (a library's own, calling back in) has no roots the
 * collector could scan; a registered thread allocating while parked is an
 * inline-C callback out of a blocking call that did not tur_gc_unpark()
 * first.  Either would be a silent use-after-free, so both stop here. */
static void tur_gc_bad_thread(const char *what) {
    fputs("tur: r7rs-gc: ", stderr);
    fputs(what, stderr);
    fputs(!tur_gc_self
          ? " on a thread the collector does not know (not started through the "
            "program's pthread_create); its roots cannot be scanned. See "
            "docs/archive/r7rs-gc-threads-plan.md, or build with TUR_R7RS_GC=0\n"
          : " while the thread is parked in a blocking call; inline C called "
            "back from such a call must tur_gc_unpark() first. See "
            "docs/archive/r7rs-gc-threads-plan.md, or build with TUR_R7RS_GC=0\n",
          stderr);
    abort();
}
static inline tur_gc_thread *tur_gc_check_thread(const char *what) {
    tur_gc_thread *t = TUR_GC_SELF_FRESH();
    if (__builtin_expect(!t || t->park_depth > 0, 0)) tur_gc_bad_thread(what);
    return t;
}

/* Under G->heap. */
static void tur_gc_note_bounds(uintptr_t base, size_t len) {
    tur_gc_state *G = tur_gc_G;
    if (!G->lo || base < G->lo) G->lo = base;
    if (base + len > G->hi) G->hi = base + len;
}

/* Under G->heap. */
static tur_gc_page *tur_gc_new_small(uint32_t cls) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_page *pg = (tur_gc_page *)tur_gc_meta(sizeof *pg);
    memset(pg, 0, sizeof *pg);
    pg->base = tur_gc_chunks(1);
    pg->size = tur_gc_class_size[cls];
    pg->nslots = (uint32_t)(TUR_GC_CHUNK / pg->size);
    pg->inv = ((uint64_t)1 << 48) / pg->size + 1;
    pg->nchunks = 1;
    pg->cls = cls;
    size_t words = (pg->nslots + 63) / 64;
    pg->alloc = (uint64_t *)tur_gc_meta(words * 8);
    pg->mark = (uint64_t *)tur_gc_meta(words * 8);
    memset(pg->alloc, 0, words * 8);
    memset(pg->mark, 0, words * 8);
    pg->next = G->pages; G->pages = pg;
    tur_gc_map_put(pg->base >> 16, pg);
    tur_gc_note_bounds(pg->base, TUR_GC_CHUNK);
    G->heap_bytes += TUR_GC_CHUNK;
    if (G->heap_bytes > G->heap_peak) G->heap_peak = G->heap_bytes;
    return pg;
}

TUR_GC_NOASAN static void tur_gc_collect_now(bool if_due);

/* Called with no lock held, at every allocation.  `since` is added to at
 * refill time and read here without a lock: a stale read costs one refill's
 * worth of delay.  Threads that cross the threshold together all ask; the
 * first collects and the others find it no longer due (tur_gc_collect_now). */
static void tur_gc_maybe_collect(void) {
    tur_gc_state *G = tur_gc_G;
    if (TUR_GC_LOAD(&G->off)) return;
    if (G->torture) {
        unsigned long n = __atomic_add_fetch(&G->torture_count, 1, __ATOMIC_RELAXED);
        if (G->torture_scale ? n >= TUR_GC_LOAD(&G->torture_due) : n % G->torture == 0)
            tur_gc_collect_now(false);
        return;
    }
    if (TUR_GC_LOAD(&G->since) > TUR_GC_LOAD(&G->threshold)) tur_gc_collect_now(true);
}

static uint32_t tur_gc_class_of(size_t n) {
    uint32_t lo = 0, hi = TUR_GC_NCLASS - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (tur_gc_class_size[mid] >= n) hi = mid; else lo = mid + 1;
    }
    return lo;
}

/* The allocator's slow path: move a batch of slots of one class from the
 * shared lists (or the class's bump page) into the calling thread's cache,
 * marking each allocated in its page's bitmap -- to the cache, from the
 * heap's point of view, until the thread hands it out.  Returns the first. */
static void *tur_gc_refill(tur_gc_thread *t, uint32_t cls) {
    tur_gc_state *G = tur_gc_G;
    size_t sz = tur_gc_class_size[cls];
    unsigned want = sz <= 256 ? 32 : sz <= 4096 ? 8 : 2, got = 0;
    void *head = NULL;
    pthread_mutex_lock(&G->heap);
    while (got < want) {
        tur_gc_page *pg;
        uint32_t idx;
        void *p = G->freelist[cls];
        if (p) {
            G->freelist[cls] = *(void **)p;
            pg = tur_gc_map_get((uintptr_t)p >> 16);
            idx = (uint32_t)(((uintptr_t)p - pg->base) / pg->size);
        } else {
            pg = G->bump[cls];
            if (!pg || pg->used >= pg->nslots) pg = G->bump[cls] = tur_gc_new_small(cls);
            idx = pg->used++;
            p = (void *)(pg->base + (uintptr_t)idx * pg->size);
        }
        pg->alloc[idx / 64] |= (uint64_t)1 << (idx % 64);
        *(void **)p = head;
        head = p;
        got++;
    }
    __atomic_add_fetch(&G->since, got * sz, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&G->heap);
    t->cache[cls] = *(void **)head;
    return head;
}

/* The fast path: pop the calling thread's cache.  No lock; a collection can
 * stop the thread anywhere in here, and the slot in flight is then in its
 * registers or on its stack, which the collector scans. */
static void *tur_gc_alloc_small(tur_gc_thread *t, size_t n) {
    uint32_t cls = tur_gc_class_of(n ? n : 1);
    void *p = t->cache[cls];
    if (p) t->cache[cls] = *(void **)p;
    else p = tur_gc_refill(t, cls);
    memset(p, 0, tur_gc_class_size[cls]);
    return p;
}

/* The object's address is kept in a local from before the page is
 * published: once G->heap is released a collection can stop this thread,
 * and the object must be reachable from its registers or stack then, not
 * only through the page descriptor (metadata, never scanned). */
static void *tur_gc_alloc_large(size_t n) {
    tur_gc_state *G = tur_gc_G;
    /* A request within a chunk of SIZE_MAX rounded to ZERO chunks, which
     * mapped nothing and handed back a live-looking pointer: malloc's
     * failure answer was lost and every `if (!p)` guard with it (security
     * audit WP5, M-5).  The page records its chunk count as a uint32_t too. */
    if (n > SIZE_MAX - (TUR_GC_CHUNK - 1)) return NULL;
    size_t nch = (n + TUR_GC_CHUNK - 1) / TUR_GC_CHUNK;
    if (nch > UINT32_MAX) return NULL;
    uintptr_t base = tur_gc_chunks(nch);
    tur_gc_page *pg = (tur_gc_page *)tur_gc_meta(sizeof *pg);
    memset(pg, 0, sizeof *pg);
    pg->base = base;
    pg->size = n;
    pg->nslots = 1;
    pg->nchunks = (uint32_t)nch;
    pg->cls = TUR_GC_NCLASS;
    pg->used = 1;
    pg->alloc = (uint64_t *)tur_gc_meta(8);
    pg->mark = (uint64_t *)tur_gc_meta(8);
    pg->alloc[0] = 1; pg->mark[0] = 0;
    pthread_mutex_lock(&G->heap);
    pg->next = G->pages; G->pages = pg;
    for (size_t i = 0; i < nch; i++) tur_gc_map_put((pg->base >> 16) + i, pg);
    tur_gc_note_bounds(pg->base, nch * TUR_GC_CHUNK);
    G->heap_bytes += nch * TUR_GC_CHUNK;
    if (G->heap_bytes > G->heap_peak) G->heap_peak = G->heap_bytes;
    __atomic_add_fetch(&G->since, nch * TUR_GC_CHUNK, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&G->heap);
    return (void *)base;   /* fresh mmap memory is already zero */
}

static void *tur_gc_malloc(size_t n) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_thread *t = tur_gc_check_thread("an allocation");
    tur_gc_maybe_collect();
    return n <= TUR_GC_MAXSMALL ? tur_gc_alloc_small(t, n) : tur_gc_alloc_large(n);
}

/* The slot `w` falls in on page `pg`.  A small page's offsets are below one
 * chunk (2^16) and its sizes at most 2^15, so the reciprocal is exact: the
 * error `off * (inv * size - 2^48) / 2^48` is under 2^-17, below the 1/size
 * a quotient would need to be off by one.  tests/check-r7gc-slot-of.sh
 * checks every class at every offset.  A large object has one slot. */
static inline uint32_t tur_gc_slot_of(const tur_gc_page *pg, uintptr_t w) {
    if (pg->nslots == 1) return 0;
    return (uint32_t)(((uint64_t)(w - pg->base) * pg->inv) >> 48);
}

/* The object `p` points into, or NULL: its start, size, page and slot.
 * Under G->heap. */
static bool tur_gc_find(uintptr_t w, uintptr_t *start, tur_gc_page **pgo, uint32_t *idxo) {
    tur_gc_state *G = tur_gc_G;
    if (w < G->lo || w >= G->hi) return false;
    tur_gc_page *pg = tur_gc_map_get(w >> 16);
    if (!pg || pg->dead) return false;
    uint32_t idx = tur_gc_slot_of(pg, w);
    if (idx >= pg->nslots) return false;
    if (!(pg->alloc[idx / 64] & ((uint64_t)1 << (idx % 64)))) return false;
    *start = pg->base + (uintptr_t)idx * (pg->nslots == 1 ? 0 : pg->size);
    *pgo = pg; *idxo = idx;
    return true;
}

/* Under G->heap. */
static void tur_gc_release_large(tur_gc_page *pg) {
    tur_gc_state *G = tur_gc_G;
    for (uint32_t i = 0; i < pg->nchunks; i++) tur_gc_map_put((pg->base >> 16) + i, NULL);
    munmap((void *)pg->base, (size_t)pg->nchunks * TUR_GC_CHUNK);
    G->heap_bytes -= (size_t)pg->nchunks * TUR_GC_CHUNK;
    pg->alloc[0] = 0;
    pg->dead = true;
}

static void tur_gc_free(void *p) {
    if (!p) return;
    tur_gc_state *G = tur_gc_G;
    if (!G) { free(p); return; }
    uintptr_t start; tur_gc_page *pg; uint32_t idx;
    pthread_mutex_lock(&G->heap);
    if (!tur_gc_find((uintptr_t)p, &start, &pg, &idx)) {
        /* Not ours: libc's, from before the redirect or from a library.
         * Ours but already free: a double free, ignored. */
        bool foreign = (uintptr_t)p < G->lo || (uintptr_t)p >= G->hi
                       || !tur_gc_map_get((uintptr_t)p >> 16);
        pthread_mutex_unlock(&G->heap);
        if (foreign) free(p);
        return;
    }
    if (start != (uintptr_t)p) { pthread_mutex_unlock(&G->heap); return; }   /* interior: not a malloc result */
    /* A thread the collector does not know -- a library's, or one of ours
     * past its start routine, in the key destructors that free its dynamic
     * bindings -- leaves the object to the collector.  Never freeing is
     * always safe here, and the destructor may still read what it frees. */
    if (!TUR_GC_SELF_FRESH()) { pthread_mutex_unlock(&G->heap); return; }
    tur_gc_check_thread("a free");
    if (pg->cls == TUR_GC_NCLASS) { tur_gc_release_large(pg); pthread_mutex_unlock(&G->heap); return; }
    pg->alloc[idx / 64] &= ~((uint64_t)1 << (idx % 64));
    *(void **)p = G->freelist[pg->cls];
    G->freelist[pg->cls] = p;
    pthread_mutex_unlock(&G->heap);
}

static void *tur_gc_calloc(size_t n, size_t m) {
    if (m && n > (size_t)-1 / m) return NULL;
    return tur_gc_malloc(n * m);   /* always zeroed */
}

static void *tur_gc_realloc(void *p, size_t n) {
    if (!p) return tur_gc_malloc(n);
    tur_gc_state *G = tur_gc_G;
    if (!G) return realloc(p, n);
    uintptr_t start; tur_gc_page *pg; uint32_t idx;
    pthread_mutex_lock(&G->heap);
    bool ours = tur_gc_find((uintptr_t)p, &start, &pg, &idx) && start == (uintptr_t)p;
    size_t have = ours ? pg->size : 0;
    bool keep = ours && n <= have && (pg->cls == TUR_GC_NCLASS || n > have / 2 || have <= 16);
    pthread_mutex_unlock(&G->heap);
    if (!ours) return realloc(p, n);   /* libc's block stays libc's */
    if (keep) return p;
    void *q = tur_gc_malloc(n);
    memcpy(q, p, have < n ? have : n);
    tur_gc_free(p);
    return q;
}

static char *tur_gc_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *)tur_gc_malloc(n);
    memcpy(r, s, n);
    return r;
}

static char *tur_gc_strndup(const char *s, size_t m) {
    size_t n = 0;
    while (n < m && s[n]) n++;
    char *r = (char *)tur_gc_malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* ---- marking ------------------------------------------------------------ */

/* The mark stack holds (start, size) pairs, so the drain does not look the
 * object's page up a second time. */
static void tur_gc_push(uintptr_t start, uintptr_t size) {
    tur_gc_state *G = tur_gc_G;
    if (G->mstack_n + 2 > G->mstack_cap) {
        size_t nc = G->mstack_cap * 2;
        uintptr_t *ns = (uintptr_t *)tur_gc_os(nc * sizeof(uintptr_t));
        memcpy(ns, G->mstack, G->mstack_n * sizeof(uintptr_t));
        munmap(G->mstack, G->mstack_cap * sizeof(uintptr_t));
        G->mstack = ns; G->mstack_cap = nc;
    }
    G->mstack[G->mstack_n++] = start;
    G->mstack[G->mstack_n++] = size;
}

/* The mark phase's chunk -> page lookup, with the last chunk remembered:
 * consecutive words of an object, and objects allocated together, mostly
 * point into one chunk.  Valid for one collection only -- no page is made,
 * released or reused while the world is stopped -- so tur_gc_collect_now
 * clears it before marking. */
static uintptr_t   tur_gc_mark_key = (uintptr_t)-1;
static tur_gc_page *tur_gc_mark_pg;

static void tur_gc_mark_word(uintptr_t w) {
    tur_gc_state *G = tur_gc_G;
    if (w < G->lo || w >= G->hi) return;
    uintptr_t key = w >> 16;
    tur_gc_page *pg;
    if (key == tur_gc_mark_key) pg = tur_gc_mark_pg;
    else { pg = tur_gc_map_get(key); tur_gc_mark_key = key; tur_gc_mark_pg = pg; }
    if (!pg || pg->dead) return;
    uint32_t idx = tur_gc_slot_of(pg, w);
    if (idx >= pg->nslots) return;
    uint64_t bit = (uint64_t)1 << (idx % 64);
    if (!(pg->alloc[idx / 64] & bit) || (pg->mark[idx / 64] & bit)) return;
    pg->mark[idx / 64] |= bit;
    tur_gc_push(pg->base + (uintptr_t)idx * (pg->nslots == 1 ? 0 : pg->size), pg->size);
}

TUR_GC_NOASAN static void tur_gc_scan(const void *p, size_t n) {
    uintptr_t a = ((uintptr_t)p + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t e = (uintptr_t)p + n;
    for (; a + sizeof(uintptr_t) <= e; a += sizeof(uintptr_t))
        tur_gc_mark_word(*(const volatile uintptr_t *)a);
}

static void tur_gc_scan_cb(const void *p, size_t n, void *ud) { (void)ud; tur_gc_scan(p, n); }

TUR_GC_NOASAN static void tur_gc_drain(void) {
    tur_gc_state *G = tur_gc_G;
    while (G->mstack_n) {
        uintptr_t size  = G->mstack[--G->mstack_n];
        uintptr_t start = G->mstack[--G->mstack_n];
        tur_gc_scan((const void *)start, size);
    }
}

/* The executable's writable data: every static the program and the linked
 * runtime keep. */
#if defined(__APPLE__)
TUR_GC_NOASAN static void tur_gc_scan_data(void) {
    const struct mach_header_64 *mh = (const struct mach_header_64 *)_dyld_get_image_header(0);
    if (!mh) return;
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)lc;
            /* __DATA, __DATA_CONST (relocated on load; read-only after, which
             * a read does not mind) and any other segment loaded writable.
             * __TEXT, __LINKEDIT and __PAGEZERO are not. */
            if ((sg->initprot & VM_PROT_WRITE) && sg->vmsize)
                tur_gc_scan((const void *)((uintptr_t)sg->vmaddr + (uintptr_t)slide), (size_t)sg->vmsize);
        }
        lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
    }
}
#else
extern char __data_start[] __attribute__((weak));
extern char _end[] __attribute__((weak));
TUR_GC_NOASAN static void tur_gc_scan_data(void) {
    uintptr_t ds = (uintptr_t)&__data_start[0], de = (uintptr_t)&_end[0];
    if (ds && de > ds) tur_gc_scan((const void *)ds, (size_t)(de - ds));
}
#endif
/* The region runtime: live and retired generations on every thread are
 * roots, through its ownership registry.  Declared here (the preamble's
 * region.h comes later) with the header's linkage.  The registry's lock is
 * tried, never waited for: a stopped thread may hold it (region.c). */
#ifndef TUR_RT_API
#define TUR_RT_API
#endif
TUR_RT_API bool tur_region_registry_trylock(void);
TUR_RT_API void tur_region_registry_unlock(void);
TUR_RT_API void tur_region_each_registered(void (*cb)(const void *p, size_t n, void *ud), void *ud);

/* A thread's stack from `sp` up.  A thread running a fiber has `sp` on the
 * fiber's stack, a heap object: the rest of that object is scanned from
 * `sp`, and the thread's own stack from the point it left it
 * (tur_gc_fiber_enter, called by tur_fiber_block_resume before its
 * swapcontext).  The fiber's saved registers are in its FiberBlock, reached
 * through tur_current_fiber. */
TUR_GC_NOASAN static void tur_gc_scan_stack(tur_gc_thread *t, unsigned char *sp) {
    if (t->os_sp) {
        uintptr_t start; tur_gc_page *pg; uint32_t idx;
        if (tur_gc_find((uintptr_t)sp, &start, &pg, &idx)) {
            uintptr_t end = start + pg->size;
            tur_gc_mark_word(start);
            if ((uintptr_t)sp < end) tur_gc_scan(sp, end - (uintptr_t)sp);
            sp = t->os_sp;
        } else if (sp > t->os_sp) {
            /* On its own stack with os_sp still set: the instants between
             * tur_gc_fiber_enter and the resume's swapcontext, or between
             * that swapcontext's return and tur_gc_fiber_leave.  The signal
             * frame the kernel built below sp holds the registers; keep
             * the scan from sp, which is below os_sp then. */
            sp = t->os_sp;
        }
    }
    if (t->stack_base && sp < t->stack_base) tur_gc_scan(sp, (size_t)(t->stack_base - sp));
}

/* A thread's cache slots are its, not garbage: marked, not scanned (their
 * contents are a free-list link and whatever the last owner left). */
TUR_GC_NOASAN static void tur_gc_mark_cache(tur_gc_thread *t) {
    for (int c = 0; c < TUR_GC_NCLASS; c++)
        for (void *p = t->cache[c]; p; p = *(void **)p) {
            uintptr_t start; tur_gc_page *pg; uint32_t idx;
            if (tur_gc_find((uintptr_t)p, &start, &pg, &idx))
                pg->mark[idx / 64] |= (uint64_t)1 << (idx % 64);
        }
}

/* A thread's pthread_setspecific values.  The count is read before the
 * arrays, and the writer publishes an entry before the count and a grown
 * array before its use (tur_gc_setspecific), so a thread stopped anywhere
 * in there is read consistently. */
TUR_GC_NOASAN static void tur_gc_mark_specific(tur_gc_thread *t) {
    size_t n = t->n_spec;
    void **vals = t->spec_vals;
    for (size_t i = 0; i < n; i++) tur_gc_mark_word((uintptr_t)vals[i]);
}

TUR_GC_NOASAN __attribute__((noinline)) static void tur_gc_mark_roots(void) {
    tur_gc_state *G = tur_gc_G;
    jmp_buf regs __attribute__((aligned(16)));   /* aligned for the scan, as in the record */
    setjmp(regs);                       /* callee-saved registers, onto this frame */
    uintptr_t fp; TUR_GC_FP(fp);        /* rbp unmangled (TUR_GC_FP) */
    volatile unsigned char here = 0;
    tur_gc_scan((const void *)&regs, sizeof regs);
    tur_gc_mark_word(fp);
    tur_gc_thread *self = tur_gc_self;
    for (tur_gc_thread *t = G->threads; t; t = t->next) {
        if (t == self) {
            tur_gc_scan_stack(t, (unsigned char *)&here);
        } else if (t->done) {
            /* Past its start routine, before its retirement: the result for
             * the join, and the key values its destructors will read. */
            tur_gc_mark_word((uintptr_t)t->result);
            tur_gc_mark_specific(t);
            continue;
        } else if (!t->started) {
            tur_gc_mark_word((uintptr_t)t->arg);
            continue;
        } else if (t->stop_state == 2) {
            /* Parked: its registers and stack are as it left them. */
            tur_gc_scan((const void *)&t->regs, sizeof t->regs);
            tur_gc_mark_word(t->regs_fp);
            tur_gc_scan_stack(t, t->stack_sp);
        } else if (t->stop_state == 1) {
            /* Stopped by the signal: the handler's spill, and the stack from
             * the handler's frame, which has the kernel's register save above it. */
            tur_gc_scan((const void *)&t->sig_regs, sizeof t->sig_regs);
            tur_gc_mark_word(t->sig_fp);
            tur_gc_scan_stack(t, t->sig_sp);
        }
        /* (stop_state 0 on a started, live thread: its tid was gone to
         * pthread_kill -- the child of a fork, say -- and it runs nothing.) */
        for (size_t i = 0; i < t->n_tls; i++) tur_gc_scan(t->tls_roots[i], t->tls_sizes[i]);
        tur_gc_mark_specific(t);
        tur_gc_mark_cache(t);
    }
    tur_gc_scan_data();
    tur_region_each_registered(tur_gc_scan_cb, NULL);
    tur_gc_drain();
}

/* ---- stopping the world -------------------------------------------------- */

/* On the thread being stopped: spill, say so, and wait on this stack for the
 * resume signal.  Everything but that signal is blocked meanwhile (the
 * handler's mask), so sigsuspend cannot miss it: sent before, it is pending
 * and delivered the moment sigsuspend unblocks it.  The collector clears
 * `stopped` before it sends the resume, so a thread that has not yet reached
 * the loop skips it.  Only async-signal-safe calls, and errno kept.
 *
 * macOS waits differently (docs/archive/r7rs-gc-darwin-stop-handler-reentered.md).
 * There the stop signal can arrive while the handler is still running --
 * the next collection's, landing before the thread has left this one's --
 * although the handler's mask blocks it: in the fixture that found this, a
 * quarter to two thirds of the handler's entries were nested ones, on
 * threads stopped while waiting on the heap mutex.  Nested sigsuspends
 * swallowed each other's resume signals, and the threads hung, or nested
 * until the kernel had no stack left to deliver a signal on (SIGILL).  So on
 * macOS no resume signal is sent: the handler polls `stopped`, and a nested
 * entry does not wait at all.  Since the outer entry spilled, the thread has
 * run nothing but this handler, so that spill is still the thread's state; a
 * nested entry marks it stopped for its own collection, acknowledges, and
 * returns to the outer entry's loop, which waits for that collection too.
 * The loop re-checks `stopped` after it leaves, since an entry landing
 * between the check and `in_stop` going to 0 is an outer one again, with a
 * wait of its own.  The depth stays at two or three frames. */
static void tur_gc_stop_handler(int sig) {
    (void)sig;
    int e = errno;
    tur_gc_thread *t = tur_gc_self;
    if (t) {
#if defined(__APPLE__)
        if (TUR_GC_LOAD(&t->in_stop)) {
            t->gc_intr = 1;
            TUR_GC_STORE(&t->stopped, 1);
            __atomic_fetch_add(&tur_gc_G->acks, 1, __ATOMIC_SEQ_CST);
            errno = e;
            return;
        }
        TUR_GC_STORE(&t->in_stop, 1);
#endif
        setjmp(t->sig_regs);
        TUR_GC_FP(t->sig_fp);
        volatile unsigned char here = 0;
        t->sig_sp = (unsigned char *)&here;
        t->gc_intr = 1;
        TUR_GC_STORE(&t->stopped, 1);
        __atomic_fetch_add(&tur_gc_G->acks, 1, __ATOMIC_SEQ_CST);
#if defined(__APPLE__)
        for (;;) {
            while (TUR_GC_LOAD(&t->stopped)) sched_yield();
            TUR_GC_STORE(&t->in_stop, 0);
            if (!TUR_GC_LOAD(&t->stopped)) break;
            TUR_GC_STORE(&t->in_stop, 1);
        }
#else
        sigset_t m;
        sigfillset(&m);
        sigdelset(&m, TUR_GC_SIG_RESUME);
        while (TUR_GC_LOAD(&t->stopped)) sigsuspend(&m);
#endif
    }
    errno = e;
}
static void tur_gc_resume_handler(int sig) { (void)sig; }

/* Under G->world and G->heap.  Every registered, running thread other than
 * the caller is either parked -- left alone, its roots are spilled -- or
 * signaled and waited for. */
static void tur_gc_stop_world(void) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_thread *self = TUR_GC_SELF_FRESH();
    long want = 0;
    TUR_GC_STORE(&G->acks, 0);
    for (tur_gc_thread *t = G->threads; t; t = t->next) {
        t->stop_state = 0;
        if (t == self || !t->started || t->done) continue;
        if (TUR_GC_LOAD(&t->parked)) { t->stop_state = 2; continue; }
        if (pthread_kill(t->tid, TUR_GC_SIG_STOP) == 0) { t->stop_state = 1; want++; }
    }
    while (TUR_GC_LOAD(&G->acks) < want) sched_yield();
}

static void tur_gc_start_world(void) {
    tur_gc_state *G = tur_gc_G;
    for (tur_gc_thread *t = G->threads; t; t = t->next) {
        if (t->stop_state != 1) continue;
        TUR_GC_STORE(&t->stopped, 0);
#if !defined(__APPLE__)
        pthread_kill(t->tid, TUR_GC_SIG_RESUME);   /* macOS polls instead */
#endif
        t->stop_state = 0;
    }
}

/* `if_due`: collect only if the threshold is still crossed once this thread
 * holds the world -- another thread that crossed it too may have collected
 * meanwhile, and a second collection back to back would find nothing. */
TUR_GC_NOASAN static void tur_gc_collect_now(bool if_due) {
    tur_gc_state *G = tur_gc_G;
    if (TUR_GC_LOAD(&G->off)) return;
    pthread_mutex_lock(&G->world);
    if (if_due && TUR_GC_LOAD(&G->since) <= TUR_GC_LOAD(&G->threshold)) {
        pthread_mutex_unlock(&G->world);
        return;
    }
    pthread_mutex_lock(&G->heap);
    /* Stop the world; if a thread was stopped inside the region registry's
     * short critical section, let it out and stop again. */
    for (;;) {
        tur_gc_stop_world();
        if (tur_region_registry_trylock()) break;
        tur_gc_start_world();
        sched_yield();
    }
    for (tur_gc_page *pg = G->pages; pg; pg = pg->next) {
        if (pg->dead) continue;
        memset(pg->mark, 0, ((pg->nslots + 63) / 64) * 8);
    }
    tur_gc_mark_key = (uintptr_t)-1;   /* pages may have changed since the last collection */
    tur_gc_mark_pg  = NULL;
    tur_gc_mark_roots();
    tur_region_registry_unlock();
    /* Sweep: rebuild every free list from the slots nobody reached. */
    for (int c = 0; c < TUR_GC_NCLASS; c++) G->freelist[c] = NULL;
    size_t live = 0, freed = 0, live_objs = 0;
    for (tur_gc_page *pg = G->pages; pg; pg = pg->next) {
        if (pg->dead) continue;
        if (pg->cls == TUR_GC_NCLASS) {
            if (pg->mark[0]) { live += pg->size; live_objs++; }
            else { freed += pg->size; tur_gc_release_large(pg); }
            continue;
        }
        /* A bitmap word at a time: the kept slots are allocated AND marked,
         * every other slot carved so far goes on the free list, lowest
         * first, as the slot-by-slot loop this replaced pushed them. */
        for (uint32_t w = 0; (size_t)w * 64 < pg->used; w++) {
            uint32_t n = pg->used - w * 64;
            uint64_t carved = n >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << n) - 1);
            uint64_t a = pg->alloc[w], keep = a & pg->mark[w];
            live_objs += (size_t)__builtin_popcountll(keep);
            live  += (size_t)__builtin_popcountll(keep) * pg->size;
            freed += (size_t)__builtin_popcountll(a & ~keep) * pg->size;
            pg->alloc[w] = keep;
            for (uint64_t f = carved & ~keep; f; f &= f - 1) {
                void *slot = (void *)(pg->base + ((uintptr_t)w * 64 + (uintptr_t)__builtin_ctzll(f)) * pg->size);
                *(void **)slot = G->freelist[pg->cls];
                G->freelist[pg->cls] = slot;
            }
        }
    }
    G->live = live;
    G->live_objs = live_objs;
    if (G->torture_scale) {
        unsigned long iv = (unsigned long)(live_objs / G->torture_scale);
        if (iv < G->torture) iv = G->torture;
        TUR_GC_STORE(&G->torture_due, TUR_GC_LOAD(&G->torture_count) + iv);
    }
    G->freed_total += freed;
    G->n_collect++;
    TUR_GC_STORE(&G->since, 0);
    TUR_GC_STORE(&G->threshold, 2 * live > G->floor ? 2 * live : G->floor);
    tur_gc_start_world();
    pthread_mutex_unlock(&G->heap);
    pthread_mutex_unlock(&G->world);
}

static void tur_gc_report(void) {
    tur_gc_state *G = tur_gc_G;
    if (!G || !G->stats) return;
    fprintf(stderr, "r7rs-gc: collections=%zu freed=%zu live-at-last=%zu heap=%zu peak-heap=%zu\n",
            G->n_collect, G->freed_total, G->live, G->heap_bytes, G->heap_peak);
}

/* Region fallbacks: outside a bracket the region allocator falls back to
 * malloc, which must be ours; memory a live generation owns stays the
 * region's. */
TUR_RT_API bool tur_region_active(void);
TUR_RT_API void *tur_region_alloc(size_t n);
TUR_RT_API bool tur_region_owns(const void *p);
TUR_RT_API void tur_region_free(void *p);
static __attribute__((unused)) void *tur_gc_region_alloc(size_t n) {
    if (tur_region_active()) { void *p = tur_region_alloc(n); if (p) return p; }
    return tur_gc_malloc(n);
}
static __attribute__((unused)) void tur_gc_region_free(void *p) {
    if (p && tur_region_owns(p)) { tur_region_free(p); return; }
    tur_gc_free(p);
}

/* Public for tests and the stats line. */
static __attribute__((unused)) void tur_gc_collect(void) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_collect_now(false);
}

/* The runtime archive's allocator hook (src/runtime/rt_alloc.h): with it
 * installed, a HAMT node or an rc<T> block the archive allocates is a
 * collected object too, so a Scheme value kept in a Turmeric map or cell is
 * scanned through it instead of freed under it.  A weak reference, because
 * the program links the archive only when something in it is used: with no
 * archive (or the bare-source autolink) there is nothing to hook, and the
 * symbol resolves to NULL. */
typedef struct tur_gc_rt_allocator {
    void *(*malloc)(size_t n);
    void *(*calloc)(size_t n, size_t m);
    void *(*realloc)(void *p, size_t n);
    void  (*free)(void *p);
} tur_gc_rt_allocator;
extern void tur_rt_set_allocator(const tur_gc_rt_allocator *a) __attribute__((weak));

/* Installing it, separately from the constructor below, because a constructor
 * is not always early enough.  The prelude split (src/compiler/emit_split.h)
 * pastes this collector into BOTH units of a program and arms the constructor
 * in only one of them -- and Mach-O ignores a constructor's priority ACROSS
 * object files, running initializers in link order instead.  The program unit
 * links first, so its __tur_static_init ran before the library unit's
 * constructor got here, and the HAMT that `__tur_module_def_init` builds and
 * the symbol table that `__tur_symtab_seed` grows were libc blocks the
 * collector could not trace through: a value kept only in a Turmeric map was
 * freed under it (docs/archive/r7rs-prelude-split-gc-seam-on-macos.md).
 *
 * So the emitter also calls this from the first statement of
 * __tur_static_init, in whichever unit has it, before anything that unit does
 * can allocate.  Calling it twice is a no-op: the table is the same one, and
 * every copy of these functions works the same shared heap (tur_gc_G is one
 * variable across the split).  It is safe before tur_gc_init too --
 * tur_gc_malloc initializes the collector on its first call.  Where TUR_GC_ON
 * is 0 the stub at the bottom of this file answers that call instead. */
static __attribute__((unused)) void tur_gc_install_rt_allocator(void) {
    if (!tur_rt_set_allocator) return;
    /* Filled field by field into a LOCAL, deliberately not built as a `static
     * const` and not even as an aggregate initializer.  tur_rt_set_allocator
     * COPIES the table, so it need not outlive this call -- and any data object
     * holding these four function pointers needs relocating, so ELF puts it in
     * `.data.rel.ro`, which `nm` classes `d`, indistinguishable from writable
     * data.  Both units of a split build call this, so such an object would be
     * defined in both and tests/check-r7rs-prelude-split.sh would read it as
     * the collector's state having forked.  That would be wrong -- it is
     * read-only after load -- but the check cannot see the difference, and four
     * stores to stack slots cost nothing to get right. */
    tur_gc_rt_allocator ours;
    ours.malloc  = tur_gc_malloc;
    ours.calloc  = tur_gc_calloc;
    ours.realloc = tur_gc_realloc;
    ours.free    = tur_gc_free;
    tur_rt_set_allocator(&ours);
}

/* ---- threads ------------------------------------------------------------ */

/* Before blocking: spill the registers and the stack pointer into this
 * thread's record, where the collector reads them, then say so; the
 * collector leaves a parked thread alone.  A park inside a park (a wrapped
 * call reached from a wrapped call) changes nothing.  Unparking takes
 * G->world for an instant, so it cannot happen in the middle of a
 * collection.  A thread the collector does not know parks nothing.
 *
 * The registers are spilled in the frame that makes the blocking call, so
 * tur_gc_park is a macro.  That frame is live until the unpark, and each
 * callee-saved register holds, at the setjmp, either the value its callers
 * left in it or a value of this frame's own, with the caller's saved in
 * this frame: the spill and the stack from below this frame have them all.
 * The stack pointer is taken in a call below the frame (tur_gc_park_at).
 * tur_gc_park used to be a function of its own, spilling its own
 * registers.  Those of its callers that it had saved to use the registers
 * itself were in its frame, which the blocking call's frames then wrote
 * over: a parked thread's root in such a register could be lost.
 *
 * On a fiber the record is read afresh (TUR_GC_SELF_FRESH), since the fiber
 * may have moved to another thread since the frame last read it: stale, a
 * fiber that parked on one worker unparked the other's record and stayed
 * parked, its stack scanned from a point it had long left. */
TUR_GC_NOASAN __attribute__((noinline)) static void tur_gc_park_at(tur_gc_thread *t) {
    volatile unsigned char here = 0;
    t->stack_sp = (unsigned char *)&here;
    TUR_GC_STORE(&t->parked, 1);
}
#define tur_gc_park() do {                                                  \
        tur_gc_thread *tpk_ = TUR_GC_SELF_FRESH();                          \
        if (tpk_ && tpk_->park_depth++ == 0) {                              \
            setjmp(tpk_->regs);         /* callee-saved registers */        \
            TUR_GC_FP(tpk_->regs_fp);   /* rbp unmangled (TUR_GC_FP) */      \
            tur_gc_park_at(tpk_);                                           \
        }                                                                   \
    } while (0)
static void tur_gc_unpark(void) {
    tur_gc_thread *t = TUR_GC_SELF_FRESH();
    if (!t || --t->park_depth > 0) return;
    pthread_mutex_lock(&tur_gc_G->world);
    TUR_GC_STORE(&t->parked, 0);
    pthread_mutex_unlock(&tur_gc_G->world);
}

/* A thread leaving its own stack for a fiber's (tur_fiber_block_resume,
 * around its swapcontext): remember where, so the collector scans the part
 * of this stack that is still live.  The point recorded is this function's
 * own frame, below every local of the resuming frame (the caller's `sp`
 * would be one local among them, with others laid out under it).  Nested
 * resumes keep the outermost point. */
__attribute__((noinline)) static void tur_gc_fiber_enter(void *sp) {
    (void)sp;
    tur_gc_thread *t = tur_gc_self;
    if (!t) return;
    volatile unsigned char here = 0;
    if (t->fiber_depth++ == 0) t->os_sp = (unsigned char *)&here;
}
static void tur_gc_fiber_leave(void) {
    tur_gc_thread *t = tur_gc_self;
    if (!t) return;
    if (--t->fiber_depth == 0) t->os_sp = NULL;
}
#define TUR_GC_FIBER_ENTER(sp) tur_gc_fiber_enter(sp)
#define TUR_GC_FIBER_LEAVE()   tur_gc_fiber_leave()

/* The end of a thread's start routine, by return or through the wrapped
 * pthread_exit: its stack and thread-locals stop being roots; its result and
 * its key values stay roots until the record is retired. */
static void tur_gc_thread_end(tur_gc_thread *t, void *r) {
    tur_gc_state *G = tur_gc_G;
    pthread_mutex_lock(&G->world);
    t->result = r;
    t->done = true;
    t->n_tls = 0;                        /* its thread-locals die with it */
    pthread_mutex_unlock(&G->world);
    tur_gc_self = NULL;
}

/* Moving a running program to another thread: the record GOES WITH it.
 * r7rs-deep-recursion-segfaults-silently runs a program's main on a
 * big-stack thread (emit_module.c, emit_deep_stack_runtime).  The thread
 * that ran static init keeps what it set there -- its thread-locals and its
 * pthread_setspecific values -- and the collector found those through its
 * record.  Retiring the record (the first version) dropped them as roots:
 * fine on Linux, a use-after-free under torture on macOS
 * (r7rs-sicp-metacircular-evaluator and r7rs-srfi-35 exited 139, an
 * address-dependent crash that did not reproduce with ASLR off).
 *
 * tur_gc_hand_over, on the old thread before it starts the new one: the old
 * thread's record, which it stops naming as its own; it touches the heap no
 * more.  tur_gc_take_over, first thing on the new thread: the record now
 * names this thread and this stack, and this thread's thread-locals join the
 * old thread's as roots (which stay: the old thread is alive, parked in a
 * join, and they may hold what static init made).  One record, so a count of
 * the program's threads does not change.  Other threads can collect between
 * the two calls -- a module init may already have started workers
 * (r7rs-threads-share hung here under torture) -- so the record is parked
 * for the window, as a thread blocked in a join would be.  The new thread is
 * started with the UNWRAPPED pthread_create (no record of its own) and the
 * old one waits in the unwrapped join (the wrapped one parks, which needs a
 * record). */
/* The unwrapped pthread_create, for the thread take_over moves a record to:
 * `pthread_create` is an object-like macro below, so a caller cannot reach
 * libc's by parenthesising the name the way it can `(pthread_join)`. */
static __attribute__((unused)) int tur_gc_raw_pthread_create(pthread_t *tp, const pthread_attr_t *a,
                                                             void *(*fn)(void *), void *arg) {
    return pthread_create(tp, a, fn, arg);
}
/* The caller parks first (tur_gc_park, in the frame that then waits in the
 * join), so a collection in the window leaves the record alone and scans
 * what the park spilled.  Clearing `tur_gc_self` under `world` keeps a stop
 * signal sent before the park was seen from landing on a thread whose
 * handler no longer knows its record, which would never acknowledge it. */
static __attribute__((unused)) void *tur_gc_hand_over(void) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_thread *t = tur_gc_self;
    if (!G || !t) return t;
    pthread_mutex_lock(&G->world);
    tur_gc_self = NULL;
    pthread_mutex_unlock(&G->world);
    return t;
}
static __attribute__((unused)) void tur_gc_take_over(void *raw) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_thread *t = (tur_gc_thread *)raw;
    if (!G || !t) return;
    unsigned char *base = tur_gc_stack_base_here();
    pthread_mutex_lock(&G->world);
    t->tid = pthread_self();
    t->has_tid = true;
    t->stack_base = base;
    if (!base) TUR_GC_STORE(&G->off, true);   /* a stack it cannot see: keep everything */
    t->park_depth = 0;                        /* the hand-over's park ends here */
    TUR_GC_STORE(&t->parked, 0);
    tur_gc_self = t;
    tur_rt_tls_roots(tur_gc_add_tls_root);
    pthread_mutex_unlock(&G->world);
}

/* Every OS thread the unit starts runs on this trampoline: it records its
 * identity, stack base and thread-local roots, runs what the program asked
 * for, and hands the result to the collector's record until the join.
 * Between pthread_create and its registration it runs none of the unit's
 * code, so `arg` is the record's root for it meanwhile.  `tid` is set here
 * as well as by the creator: which of the two gets there first is the OS's
 * choice, and a collection signals the thread from the moment it is
 * `started`. */
static void *tur_gc_thread_main(void *raw) {
    tur_gc_thread *t = (tur_gc_thread *)raw;
    tur_gc_state *G = tur_gc_G;
    unsigned char *base = tur_gc_stack_base_here();
    pthread_setspecific(G->exit_key, t);
    pthread_mutex_lock(&G->world);
    t->tid = pthread_self();
    t->has_tid = true;
    t->stack_base = base;
    if (!base) TUR_GC_STORE(&G->off, true);   /* a stack it cannot see: keep everything */
    tur_gc_self = t;
    t->started = true;
    tur_rt_tls_roots(tur_gc_add_tls_root);
    pthread_mutex_unlock(&G->world);
    void *r = t->fn(t->arg);
    tur_gc_thread_end(t, r);
    return r;
}

/* The collector's key destructor: the thread is past everything that reads
 * its key values.  The key was created before any of the program's, so libc
 * runs this first in a round; it re-arms once and does its work in the next
 * round, after every other destructor has run.  A thread that left without
 * returning or calling the wrapped pthread_exit -- a cancellation, a
 * library's own pthread_exit -- is finished here as well.  A detached
 * thread's record is retired now; a joinable one's waits for the join. */
static void tur_gc_thread_gone(void *raw) {
    tur_gc_thread *t = (tur_gc_thread *)raw;
    tur_gc_state *G = tur_gc_G;
    if (t->exit_rounds++ == 0) { pthread_setspecific(G->exit_key, t); return; }
    pthread_mutex_lock(&G->world);
    if (!t->done) { t->done = true; t->n_tls = 0; }
    t->gone = true;
    if (t->detached) tur_gc_retire(t);
    pthread_mutex_unlock(&G->world);
    tur_gc_self = NULL;
}

/* The record is linked before the thread exists, so its `arg` is a root
 * from the start.  After pthread_create the creator touches the record only
 * if it is still the same incarnation: a detached thread can finish and be
 * retired, and its record reused, before pthread_create returns. */
static int tur_gc_pthread_create(pthread_t *tp, const pthread_attr_t *a,
                                 void *(*fn)(void *), void *arg) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_state *G = tur_gc_G;
    tur_gc_check_thread("a thread start");
    int ds = PTHREAD_CREATE_JOINABLE;
    if (a && pthread_attr_getdetachstate(a, &ds) != 0) ds = PTHREAD_CREATE_JOINABLE;
    pthread_mutex_lock(&G->world);
    tur_gc_thread *t = tur_gc_thread_new();
    t->fn = fn; t->arg = arg;
    t->detached = ds == PTHREAD_CREATE_DETACHED;
    unsigned long gen = t->gen;
    t->next = G->threads; G->threads = t;
    pthread_mutex_unlock(&G->world);
    pthread_t tid;
    /* Parked across the create: macOS's pthread_create holds libpthread's
     * global thread-list lock while it links the new thread in, and
     * pthread_kill takes the same lock to find its target.  Stopped there by
     * the signal, this thread would hold the lock the collector's next
     * pthread_kill waits on -- a deadlock (docs/archive/
     * r7rs-gc-threads-lifecycle-macos-timeout.md).  Parked, it is never
     * signalled, and pthread_create touches nothing the collector scans. */
    tur_gc_park();
    int rc = pthread_create(&tid, a, tur_gc_thread_main, t);
    tur_gc_unpark();
    pthread_mutex_lock(&G->world);
    if (rc != 0) tur_gc_retire(t);
    else if (t->gen == gen) { t->tid = tid; t->has_tid = true; }
    pthread_mutex_unlock(&G->world);
    if (rc == 0) *tp = tid;
    return rc;
}

/* A thread leaving through pthread_exit never returns to the trampoline, so
 * the start routine's end is recorded here (the emitted tur_thread_do_cancel
 * exits a cancelled thread this way). */
static void tur_gc_pthread_exit(void *r) __attribute__((noreturn));
static void tur_gc_pthread_exit(void *r) {
    tur_gc_thread *t = TUR_GC_SELF_FRESH();
    if (t) tur_gc_thread_end(t, r);
    pthread_exit(r);
}

/* The joiner is parked while it waits; the joined thread is gone by the time
 * pthread_join returns, so its record is retired. */
static int tur_gc_pthread_join(pthread_t tid, void **out) {
    tur_gc_park();
    int rc = pthread_join(tid, out);
    tur_gc_unpark();
    if (rc == 0 && tur_gc_G) {
        tur_gc_state *G = tur_gc_G;
        pthread_mutex_lock(&G->world);
        for (tur_gc_thread *t = G->threads; t; t = t->next)
            if (t->has_tid && !t->detached && pthread_equal(t->tid, tid)) { tur_gc_retire(t); break; }
        pthread_mutex_unlock(&G->world);
    }
    return rc;
}

/* A detached thread is retired when it is gone (tur_gc_thread_gone); one
 * already gone -- finished, waiting for a join that will not come -- is
 * retired now.  Without this a detached thread's record, result and key
 * values would stay in the registry for the life of the process (stdlib
 * futures and task groups detach their timeout threads). */
static int tur_gc_pthread_detach(pthread_t tid) {
    tur_gc_state *G = tur_gc_G;
    if (G) {
        pthread_mutex_lock(&G->world);
        for (tur_gc_thread *t = G->threads; t; t = t->next)
            if (t->has_tid && !t->detached && pthread_equal(t->tid, tid)) {
                if (t->gone) tur_gc_retire(t); else t->detached = true;
                break;
            }
        pthread_mutex_unlock(&G->world);
    }
    /* Parked for the reason tur_gc_pthread_create is: macOS's pthread_detach
     * takes the thread-list lock too. */
    tur_gc_park();
    int rc = pthread_detach(tid);
    tur_gc_unpark();
    return rc;
}

/* pthread_setspecific, keeping the value as a root of the calling thread:
 * libc stores it where no scan reaches (the thread descriptor), and the
 * emitted runtime keeps heap blocks there -- a dynamic variable's conveyed
 * frames, the ^thread-local globals' block.  The value is still in the
 * caller's registers until it is recorded.  The stores are ordered for a
 * stop at any instruction: a grown array is filled before it is published,
 * and an entry is written before the count that covers it. */
static int tur_gc_setspecific(pthread_key_t k, const void *v) {
    int r = pthread_setspecific(k, v);
    tur_gc_thread *t = TUR_GC_SELF_FRESH();
    if (r != 0 || !t) return r;
    for (size_t i = 0; i < t->n_spec; i++)
        if (t->spec_keys[i] == k) { t->spec_vals[i] = (void *)v; return r; }
    if (t->n_spec == t->cap_spec) {
        size_t nc = t->cap_spec ? t->cap_spec * 2 : 8;
        pthread_key_t *nk = (pthread_key_t *)tur_gc_meta(nc * sizeof *nk);
        void **nv = (void **)tur_gc_meta(nc * sizeof *nv);
        if (t->n_spec) {
            memcpy(nk, t->spec_keys, t->n_spec * sizeof *nk);
            memcpy(nv, t->spec_vals, t->n_spec * sizeof *nv);
        }
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        t->spec_keys = nk;
        t->spec_vals = nv;
        t->cap_spec = nc;
    }
    t->spec_keys[t->n_spec] = k;
    t->spec_vals[t->n_spec] = (void *)v;
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    t->n_spec++;
    return r;
}

/* fork: the child has only the forking thread, and must not inherit a lock
 * another thread held -- the heap lock mid-refill would stop the child's
 * first allocation for good.  So every lock the collector uses is taken
 * before the fork, in the collector's own order (world, heap, metadata, the
 * region registry), and released on both sides after; in the child every
 * other thread's record is retired, its stack and its cache with it. */
static void tur_gc_atfork_prepare(void) {
    tur_gc_state *G = tur_gc_G;
    pthread_mutex_lock(&G->world);
    pthread_mutex_lock(&G->heap);
    pthread_mutex_lock(&G->meta_lock);
    while (!tur_region_registry_trylock()) sched_yield();
}
static void tur_gc_atfork_parent(void) {
    tur_gc_state *G = tur_gc_G;
    tur_region_registry_unlock();
    pthread_mutex_unlock(&G->meta_lock);
    pthread_mutex_unlock(&G->heap);
    pthread_mutex_unlock(&G->world);
}
static void tur_gc_atfork_child(void) {
    tur_gc_state *G = tur_gc_G;
    tur_gc_thread *self = tur_gc_self, *next;
    for (tur_gc_thread *t = G->threads; t; t = next) {
        next = t->next;
        if (t != self) tur_gc_retire(t);
    }
    tur_gc_atfork_parent();
}

/* The release points.  Each blocking libc call the unit can spell is routed
 * through a wrapper that parks around it; the function-like macros below do
 * the routing for every site in the stdlib, the emitted runtime and the
 * program, with no change to their text.  A contended mutex parks too (a
 * thread that spins on one a stopped thread holds would stop the world
 * forever); the uncontended lock stays a try.  A stop signal that lands in
 * the instant between the park and the call interrupts a call that does
 * not restart (nanosleep, poll, select, sem_wait ...): the wrapper retries
 * that EINTR, and only that one.  errno is what the call left. */
static int tur_gc_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    tur_gc_park(); int r = pthread_cond_wait(c, m); tur_gc_unpark(); return r;
}
static int tur_gc_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *ts) {
    tur_gc_park(); int r = pthread_cond_timedwait(c, m, ts); tur_gc_unpark(); return r;
}
static int tur_gc_mutex_lock(pthread_mutex_t *m) {
    if (pthread_mutex_trylock(m) == 0) return 0;
    /* The collector's own locks are taken as the collector takes them, never
     * parked around: unparking takes `world`, so a contended `world` taken
     * through here waited on itself for good (a thread retiring its record
     * held it at that instant -- r7rs-gc-threads-lifecycle-rare-hang), and
     * `heap` or `meta_lock` would have `world` taken under them, against the
     * collector's order.  Unparked, the wait is a stop point like any other
     * lock of the collector's: a collection signals it and it answers. */
    tur_gc_state *G = tur_gc_G;
    if (G && (m == &G->world || m == &G->heap || m == &G->meta_lock)) return pthread_mutex_lock(m);
    tur_gc_park(); int r = pthread_mutex_lock(m); tur_gc_unpark(); return r;
}
static inline void tur_gc_intr_clear(void) { tur_gc_thread *t = TUR_GC_SELF_FRESH(); if (t) t->gc_intr = 0; }
static inline bool tur_gc_intr_ours(void) { tur_gc_thread *t = TUR_GC_SELF_FRESH(); return t && t->gc_intr; }
/* The other blocking calls are wrapped at the call site, as a statement
 * expression around the call as written, whatever its signature.  The
 * macros are function-like, so a struct member of the same name is left
 * alone; and every header that declares one of these names is included at
 * the top of this file, so a later include is a guarded no-op and the macro
 * never meets a prototype it would mangle. */
#define TUR_GC_BLOCKING(call) __extension__ ({                           \
        tur_gc_park(); __typeof__(call) r_; int e_;                       \
        do { tur_gc_intr_clear(); r_ = (call); e_ = errno; }              \
        while (r_ == (__typeof__(call))-1 && e_ == EINTR && tur_gc_intr_ours()); \
        tur_gc_unpark(); errno = e_; r_; })
/* The thread calls: the lifecycle the registry follows, and the key store
 * whose values it roots. */
#define pthread_create tur_gc_pthread_create
#define pthread_join(t, o)              tur_gc_pthread_join((t), (o))
#define pthread_exit(r)                 tur_gc_pthread_exit(r)
#define pthread_detach(t)               tur_gc_pthread_detach(t)
#define pthread_setspecific(k, v)       tur_gc_setspecific((k), (v))
#define pthread_cond_wait(c, m)         tur_gc_cond_wait((c), (m))
#define pthread_cond_timedwait(c, m, t) tur_gc_cond_timedwait((c), (m), (t))
#define pthread_mutex_lock(m)           tur_gc_mutex_lock(m)
#define nanosleep(a, b)                 TUR_GC_BLOCKING(nanosleep((a), (b)))
#define usleep(u)                       TUR_GC_BLOCKING(usleep(u))
#define poll(f, n, ms)                  TUR_GC_BLOCKING(poll((f), (n), (ms)))
#define select(n, r, w, x, tv)          TUR_GC_BLOCKING(select((n), (r), (w), (x), (tv)))
#define accept(fd, a, l)                TUR_GC_BLOCKING(accept((fd), (a), (l)))
#define connect(fd, a, l)               TUR_GC_BLOCKING(connect((fd), (a), (l)))
#define recv(fd, b, n, f)               TUR_GC_BLOCKING(recv((fd), (b), (n), (f)))
#define recvfrom(fd, b, n, f, a, l)     TUR_GC_BLOCKING(recvfrom((fd), (b), (n), (f), (a), (l)))
#define read(fd, b, n)                  TUR_GC_BLOCKING(read((fd), (b), (n)))
#define waitpid(p, s, o)                TUR_GC_BLOCKING(waitpid((p), (s), (o)))
#define sem_wait(s)                     TUR_GC_BLOCKING(sem_wait(s))
#define epoll_wait(ep, ev, n, ms)       TUR_GC_BLOCKING(epoll_wait((ep), (ev), (n), (ms)))
#define kevent(kq, c, nc, ev, nev, ts)  TUR_GC_BLOCKING(kevent((kq), (c), (nc), (ev), (nev), (ts)))

/* Before every other constructor in the unit (101 is the first user
 * priority), so the archive's first allocation already goes through us.  The
 * priority holds only WITHIN an object file on Mach-O, which is why the
 * install is also called explicitly from __tur_static_init -- see
 * tur_gc_install_rt_allocator. */
static __attribute__((constructor(101))) void tur_gc_ctor(void) {
    if (!tur_gc_G) tur_gc_init();
    tur_gc_install_rt_allocator();
    pthread_mutex_lock(&tur_gc_G->world);
    tur_rt_tls_roots(tur_gc_add_tls_root);
    pthread_mutex_unlock(&tur_gc_G->world);
    atexit(tur_gc_report);
}

#else  /* !TUR_GC_ON: libc, and nothing is ever collected */

#define TUR_GC_FIBER_ENTER(sp) ((void)0)
#define TUR_GC_FIBER_LEAVE()   ((void)0)

static void *tur_gc_malloc(size_t n) { return malloc(n); }
static void *tur_gc_calloc(size_t n, size_t m) { return calloc(n, m); }
static void *tur_gc_realloc(void *p, size_t n) { return realloc(p, n); }
static void  tur_gc_free(void *p) { free(p); }
static char *tur_gc_strdup(const char *s) {
    size_t n = strlen(s) + 1; char *r = (char *)malloc(n); if (r) memcpy(r, s, n); return r;
}
static char *tur_gc_strndup(const char *s, size_t m) {
    size_t n = 0; while (n < m && s[n]) n++;
    char *r = (char *)malloc(n + 1); if (r) { memcpy(r, s, n); r[n] = 0; } return r;
}
#ifndef TUR_RT_API
#define TUR_RT_API
#endif
TUR_RT_API void *tur_region_alloc_or_malloc(size_t n);
TUR_RT_API void tur_region_free(void *p);
static void *tur_gc_region_alloc(size_t n) { return tur_region_alloc_or_malloc(n); }
static void  tur_gc_region_free(void *p) { tur_region_free(p); }
static __attribute__((unused)) void tur_gc_collect(void) { }
/* Nothing to install: these entry points ARE libc. */
static __attribute__((unused)) void tur_gc_install_rt_allocator(void) { }

#endif
