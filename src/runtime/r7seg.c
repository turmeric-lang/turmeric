/* r7seg.c -- the segmented stack of a compiled `#lang r7rs` program
 * (r7rs-heap-frames-plan, option C; --enable=r7rs-segmented-stack).
 *
 * One source, two consumers.  The emitter pastes it, comments stripped, into
 * the unit of every r7rs program built with the flag (emit_seg_stack_runtime,
 * src/compiler/emit_module.c; embedded by cmake/embed_region_runtime.cmake),
 * where it defines the check, the switch and the state.  And `tur` itself
 * compiles it with TUR_SEG_HOST for `tur jit` (H6), whose program is compiled
 * by c2mir: c2mir has no thread-locals and no inline asm, so under the JIT the
 * state and the trampoline stay in the host, and the program's check reaches
 * them through the calls at the end of this file.
 *
 * H0, the check.  A procedure of an r7rs file opens with TUR_SEG_CHECK --
 * every one of the program's, and those of the stdlib that a cycle of calls
 * needs (emit_fn_wants_seg_check, emit_fns.c): read the stack pointer, and
 * while `sp - lo` (unsigned) is within `span` (tur_seg_hot, one thread-local)
 * carry on.  One compare, which also fails for a stack pointer on a stack the
 * pair does not describe -- after a longjmp out of a segment.  The stack
 * pointer comes from an asm that is not volatile, not
 * __builtin_frame_address, which forces a frame pointer.  Below the limit the
 * procedure re-enters itself with the same arguments through tur_seg_tramp,
 * as a TAIL call, so the slow path needs no frame of its own.  The plan has
 * the measured cost.
 *
 * H1, the switch.  tur_seg_tramp (asm, x86-64 and arm64) saves the argument
 * registers and asks tur_seg_enter for a stack: a new segment (8 MiB mapped,
 * a guard at the bottom, one spare kept per thread against a boundary
 * crossed in a loop), the stack-passed arguments copied to its top -- the
 * trampoline is entered exactly as the procedure was, so they start just
 * above its return address either way -- or none, when the check passes again
 * where it is.  It calls the procedure there, and on the way back drops the
 * segment.  An entry check leaves TUR_SEG_HEADROOM under its limit, for inline
 * C, libc and signal frames that run without passing one.
 *
 * H2, the collector: r7gc.c walks the chain (tur_gc_scan_segments) from the
 * record it is told about at thread start (tur_gc_seg_attach).
 *
 * H3, call/cc: a capture on a segment copies the live part of each segment
 * down to the thread's own stack (tur_seg_r7k_capture), and a re-entry maps
 * each segment back at its address (tur_seg_r7k_restore).  Segment
 * addresses recycle only as whole slots (the pool), so the slot is found.
 *
 * H5, the cap: TUR_MAIN_STACK_MB, when set, is the total of the thread's own
 * stack and its segments; unset, half of physical memory.  The thread's own
 * stack is what it was without segments, up to TUR_SEG_BASE_MAX (1 GiB, the
 * default) -- so code no check covers (a CPS body's calls at -O0, inline C)
 * has every byte it had before -- and segments start past it.
 * TUR_SEG_BASE_MB makes it smaller, for a test that wants to reach them.
 * Past the cap, the overflow message and the SIGSEGV the guard page would
 * have given.
 *
 * Linux x86-64 and arm64.  The deep-stack thread arms itself, and every thread
 * the program starts is armed by tur_gc_thread_main (H4) with a chain of its
 * own; a fiber runs unchecked on its own stack (tur_fiber_block_resume).
 * Under `tur jit` (H6) the engine arms its entry thread, which runs main;
 * threads a JIT'd program starts run unchecked, on their own stacks as
 * before.  The r7k call/cc hooks (H3) are the host's there too.
 * Off in a --shared build, where no deep-stack thread arms it.  The split
 * writes the static helpers and the asm into every unit (each object's
 * trampoline calls its own tur_seg_enter) and keeps the state in the
 * library unit. */
#if defined(TUR_SEG_HOST)
#pragma GCC diagnostic ignored "-Wunused-function"
#include "runtime/stack_overflow.h"
#define TUR_THREAD_LOCAL __thread
#define TUR_SEG_OVERFLOW_MSG TUR_STACK_OVERFLOW_MSG "\n"
#include "runtime/r7seg_host.h"
#include "runtime/region.h"
#define TUR_REGION_NOTE_WORDS(p, n) tur_region_note_escape_words((const void *)(p), (size_t)(n))
#endif
#if (defined(TUR_SEG_HOST) || !defined(__MIRC__)) && (defined(__GNUC__) || defined(__clang__)) && \
    defined(__linux__) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define TUR_SEG_ON 1
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
/* One stack: the thread's own (map NULL) or a segment.  The usable range is
 * [bot, top); a procedure's entry check fails below lo, which leaves
 * TUR_SEG_HEADROOM under it for inline C, libc and signal frames.  saved_sp is
 * where the frames on prev resume, so the collector scans prev from there. */
#ifndef TUR_SEG_REC_DEFINED
#define TUR_SEG_REC_DEFINED 1
typedef struct tur_seg_rec {
    struct tur_seg_rec *prev;
    unsigned char *map; size_t map_size;
    unsigned char *bot, *lo, *top;
    unsigned char *saved_sp;
} tur_seg_rec;
#endif
#define TUR_SEG_SIZE     ((size_t)8 << 20)
#define TUR_SEG_GUARD    ((size_t)64 << 10)
#define TUR_SEG_HEADROOM ((size_t)256 << 10)
#define TUR_SEG_TOPPAD   ((size_t)4 << 10)
#define TUR_SEG_SLACK    ((size_t)64 << 10)
#define TUR_SEG_BASE_MAX ((size_t)1 << 30)
/* The hot pair: a check passes while sp - lo <= span (unsigned), so a stack
 * pointer below lo, or on a stack the record does not describe, fails it.
 * Unarmed (every thread but the deep-stack one) the pair passes everything. */
typedef struct { unsigned char *lo; uintptr_t span; } tur_seg_pair;
static TUR_THREAD_LOCAL tur_seg_pair tur_seg_hot = { 0, (uintptr_t)-1 };
static TUR_THREAD_LOCAL tur_seg_rec *tur_seg_cur = 0;
static TUR_THREAD_LOCAL tur_seg_rec *tur_seg_spare = 0;
static TUR_THREAD_LOCAL void *tur_seg_fn = 0;
static TUR_THREAD_LOCAL size_t tur_seg_nb = 0;
static TUR_THREAD_LOCAL size_t tur_seg_used = 0;
static TUR_THREAD_LOCAL unsigned char *tur_seg_last = 0;
static size_t tur_seg_budget_v;
static TUR_THREAD_LOCAL tur_seg_rec tur_seg_base_rec;   /* the thread's own stack */
static TUR_THREAD_LOCAL unsigned char *tur_seg_alt = 0;   /* a started thread's signal stack */
static void tur_seg_budget_set(size_t sz, int asked) {
    size_t b = sz;
    if (!asked) {
        long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE);
        b = (pages > 0 && psz > 0) ? (size_t)pages * (size_t)psz / 2 : sz;
        if (b < sz) b = sz;
    }
    tur_seg_budget_v = b;
}
static void tur_seg_overflow(void) {
    static const char m[] = TUR_SEG_OVERFLOW_MSG;
    if (write(2, m, sizeof m - 1) < 0) { }
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
    _exit(139);
}
static void tur_seg_arm(const tur_seg_rec *r) {
    tur_seg_hot.lo = r->lo;
    tur_seg_hot.span = (uintptr_t)(r->top - r->lo);
}
static int tur_seg_holds(const tur_seg_rec *r, const unsigned char *sp) {
    return sp >= r->bot && sp < r->top;
}
/* H3: a released segment's address stays reserved -- PROT_NONE, its memory
 * given back -- in a pool new segments come from first.  So segment
 * addresses recycle only as whole slots, and a continuation captured on a
 * segment finds its slot again by address on a re-entry, however the
 * stack has moved since.  The pool is as large as the thread's deepest
 * recursion was; mmap'd, not malloc'd, which the collector would own. */
static TUR_THREAD_LOCAL unsigned char **tur_seg_pool = 0;
static TUR_THREAD_LOCAL size_t tur_seg_pool_n = 0, tur_seg_pool_cap = 0;
static void tur_seg_pool_put(unsigned char *map) {
    if (tur_seg_pool_n == tur_seg_pool_cap) {
        size_t cap = tur_seg_pool_cap ? tur_seg_pool_cap * 2 : 512;
        void *a = mmap(NULL, cap * sizeof *tur_seg_pool, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a == MAP_FAILED) { munmap(map, TUR_SEG_SIZE); return; }
        if (tur_seg_pool) {
            memcpy(a, tur_seg_pool, tur_seg_pool_n * sizeof *tur_seg_pool);
            munmap(tur_seg_pool, tur_seg_pool_cap * sizeof *tur_seg_pool);
        }
        tur_seg_pool = (unsigned char **)a; tur_seg_pool_cap = cap;
    }
    mmap(map, TUR_SEG_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    tur_seg_pool[tur_seg_pool_n++] = map;
}
/* A slot from the pool, mapped usable again: `want`, or any when NULL. */
static unsigned char *tur_seg_pool_take(unsigned char *want) {
    for (size_t i = tur_seg_pool_n; i-- > 0;) {
        unsigned char *m = tur_seg_pool[i];
        if (want && m != want) continue;
        tur_seg_pool[i] = tur_seg_pool[--tur_seg_pool_n];
        if (mmap(m, TUR_SEG_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED) return NULL;
        mprotect(m, TUR_SEG_GUARD, PROT_NONE);
        return m;
    }
    return NULL;
}
static void tur_seg_release(tur_seg_rec *r) {
    tur_seg_used -= r->map_size;
    if (!tur_seg_spare) { tur_seg_spare = r; return; }
    tur_seg_pool_put(r->map);
}
/* The record holding sp, after dropping every newer segment: a longjmp out
 * of one never passes its trampoline's return.  NULL, dropping nothing, when
 * no record does -- a fiber's stack, entered from frames that are still live. */
static tur_seg_rec *tur_seg_resync(const unsigned char *sp) {
    tur_seg_rec *h = tur_seg_cur;
    while (h && !tur_seg_holds(h, sp)) h = h->prev;
    if (!h) return NULL;
    while (tur_seg_cur != h) {
        tur_seg_rec *r = tur_seg_cur;
        tur_seg_cur = r->prev;
        __asm__ volatile ("" ::: "memory");
        tur_seg_release(r);
    }
    return h;
}
static tur_seg_rec *tur_seg_new(void) {
    tur_seg_rec *r = tur_seg_spare;
    size_t base = tur_seg_base_rec.top ? (size_t)(tur_seg_base_rec.top - tur_seg_base_rec.bot) : 0;
    if (tur_seg_used + TUR_SEG_SIZE + base > tur_seg_budget_v) tur_seg_overflow();
    if (r) {
        tur_seg_spare = NULL;
    } else {
        unsigned char *m = tur_seg_pool_take(NULL);
        if (!m) {
            m = (unsigned char *)mmap(NULL, TUR_SEG_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
            if (m == (unsigned char *)MAP_FAILED) tur_seg_overflow();
            mprotect(m, TUR_SEG_GUARD, PROT_NONE);
        }
        r = (tur_seg_rec *)(m + TUR_SEG_SIZE - sizeof(tur_seg_rec));
        r->map = m; r->map_size = TUR_SEG_SIZE;
        r->bot = m + TUR_SEG_GUARD;
        r->lo = r->bot + TUR_SEG_HEADROOM;
        r->top = (unsigned char *)((uintptr_t)(m + TUR_SEG_SIZE - TUR_SEG_TOPPAD) & ~(uintptr_t)63);
    }
    tur_seg_used += r->map_size;
    return r;
}
typedef struct { unsigned char *sp; void *fn; } tur_seg_go;
/* From the trampoline: `sp` is the bottom of its register save, everything
 * live on this stack is at or above it; `args` is where the procedure's
 * stack-passed arguments start.  Returns the stack pointer to call it at on a
 * new segment, or NULL to call it where it is (the check passes there now). */
__attribute__((used)) static tur_seg_go tur_seg_enter(unsigned char *sp, const unsigned char *args) {
    tur_seg_go g = { NULL, tur_seg_fn };
    size_t nb = tur_seg_nb;
    tur_seg_rec *c = tur_seg_resync(sp);
    if (!c) {
        /* No record describes this stack: stop checking rather than re-enter
         * forever.  A fiber never gets here -- tur_fiber_block_resume runs it
         * with the pair unarmed and re-arms the thread when it comes back. */
        tur_seg_hot.lo = 0; tur_seg_hot.span = (uintptr_t)-1;
        return g;
    }
    /* Room left: the check failed on a stale pair (a longjmp out of a segment).
     * The procedure's check may run below its entry, after its prologue, so
     * room means TUR_SEG_SLACK of it; and a second failure at the same point
     * means even that was not enough -- grow, rather than re-enter forever. */
    if (sp >= c->lo + TUR_SEG_SLACK && sp != tur_seg_last) {
        tur_seg_last = sp;
        tur_seg_arm(c);
        return g;
    }
    tur_seg_last = NULL;
    if (nb > TUR_SEG_TOPPAD / 2) tur_seg_overflow();
    tur_seg_rec *n = tur_seg_new();
    n->prev = c; n->saved_sp = sp;
    __asm__ volatile ("" ::: "memory");
    tur_seg_cur = n;
    tur_seg_arm(n);
    unsigned char *nsp = (unsigned char *)((uintptr_t)(n->top - nb) & ~(uintptr_t)15);
    memcpy(nsp, args, nb);
    g.sp = nsp;
    return g;
}
/* From the trampoline, back on the stack it left. */
__attribute__((used)) static void tur_seg_leave(unsigned char *sp) {
    tur_seg_rec *c = tur_seg_resync(sp);
    if (c) tur_seg_arm(c);
}
static int tur_seg_off_base(const void *p);
static void tur_seg_thread_arm(void);
static void tur_seg_thread_end(void);
/* Arm the calling thread: its own stack is the chain's first record.  `lo`
 * is that stack's low end, or NULL to ask pthreads for it. */
static void tur_seg_thread_start(unsigned char *lo) {
    pthread_attr_t a; void *addr = 0; size_t sz = 0;
    extern int pthread_getattr_np(pthread_t, pthread_attr_t *);
    if (pthread_getattr_np(pthread_self(), &a) != 0) return;
    if (pthread_attr_getstack(&a, &addr, &sz) != 0) sz = 0;
    pthread_attr_destroy(&a);
    if (!sz) return;
    if (!lo) lo = (unsigned char *)addr;
    tur_seg_rec *b = &tur_seg_base_rec;
    b->prev = 0; b->map = 0; b->map_size = 0;
    b->bot = lo; b->lo = lo + TUR_SEG_HEADROOM; b->top = (unsigned char *)addr + sz;
    tur_seg_cur = b;
#if defined(TUR_GC_ON) && TUR_GC_ON
    tur_gc_seg_attach((void **)&tur_seg_cur);
    /* H4: each thread the program starts gets a chain of its own. */
    tur_gc_thread_start_hook = tur_seg_thread_arm;
    tur_gc_thread_end_hook = tur_seg_thread_end;
#endif
    tur_seg_arm(b);
}
/* A started thread, before its function runs (tur_gc_thread_main): arm it,
 * and give it a signal stack, so an overflow on it prints the message too. */
static void tur_seg_thread_arm(void) {
    unsigned char *m = (unsigned char *)mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m != (unsigned char *)MAP_FAILED) {
        stack_t ss; ss.ss_sp = m; ss.ss_size = 65536; ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) == 0) tur_seg_alt = m; else munmap(m, 65536);
    }
    tur_seg_thread_start(NULL);
}
/* The thread's function has returned, so every trampoline under it has: it
 * is back on its own stack, and a segment still on the chain was left by a
 * longjmp no check saw.  Unmap them, and the spare.  A thread that leaves
 * through pthread_exit from a segment never gets here, and keeps its
 * mappings until the process ends. */
static void tur_seg_thread_end(void) {
    tur_seg_hot.lo = 0; tur_seg_hot.span = (uintptr_t)-1;
#if defined(TUR_GC_ON) && TUR_GC_ON
    tur_gc_seg_attach(NULL);
#endif
    tur_seg_rec *r = tur_seg_cur;
    tur_seg_cur = 0;
    while (r && r->map) { tur_seg_rec *p = r->prev; munmap(r->map, r->map_size); r = p; }
    if (tur_seg_spare) { munmap(tur_seg_spare->map, tur_seg_spare->map_size); tur_seg_spare = 0; }
    for (size_t i = 0; i < tur_seg_pool_n; i++) munmap(tur_seg_pool[i], TUR_SEG_SIZE);
    if (tur_seg_pool) munmap(tur_seg_pool, tur_seg_pool_cap * sizeof *tur_seg_pool);
    tur_seg_pool = 0; tur_seg_pool_n = tur_seg_pool_cap = 0;
    tur_seg_used = 0;
    if (tur_seg_alt) {
        stack_t ss; memset(&ss, 0, sizeof ss); ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        munmap(tur_seg_alt, 65536); tur_seg_alt = 0;
    }
}
#define TUR_SEG_THREAD_START(lo) tur_seg_thread_start((unsigned char *)(lo))
/* The fault handler's test: an address in a live segment's guard. */
static int tur_seg_fault_is_overflow(const unsigned char *a) {
    for (const tur_seg_rec *r = tur_seg_cur; r && r->map; r = r->prev)
        if (a >= r->map && a < r->bot + 65536) return 1;
    return 0;
}
/* 1 when `p` is on a segment, not the thread's own stack. */
static int tur_seg_off_base(const void *p) {
    return tur_seg_cur && tur_seg_cur->map && !tur_seg_holds(&tur_seg_base_rec, (const unsigned char *)p);
}
/* H3: a continuation captured on a segment.  The image is the live part of
 * each segment from the capture down to the thread's own stack, with the
 * segment's record; the caller copies the thread's own stack from `*resume`
 * up, as for any capture.  NULL when `mark` is on no segment. */
typedef struct {
    unsigned char *map; size_t map_size;
    unsigned char *bot, *lo, *top, *saved_sp;
    unsigned char *from; size_t n; unsigned char *data;
} tur_seg_piece;
typedef struct { int n; tur_seg_piece p[]; } tur_seg_image;
__attribute__((noinline))
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
__attribute__((no_sanitize_address))
#endif
#endif
static void tur_seg_copy(unsigned char *dst, const unsigned char *src, size_t n) {
    volatile uintptr_t *d = (volatile uintptr_t *)dst;
    const volatile uintptr_t *s = (const volatile uintptr_t *)src;
    for (size_t i = 0; i < n / sizeof(uintptr_t); i++) d[i] = s[i];
}
__attribute__((weak)) int tur_seg_r7k_off_base(const void *p) { return tur_seg_off_base(p); }
__attribute__((weak)) void *tur_seg_r7k_capture(const unsigned char *mark, unsigned char **resume) {
    tur_seg_rec *r = tur_seg_cur;
    while (r && !tur_seg_holds(r, mark)) r = r->prev;
    if (!r || !r->map) return NULL;
    int k = 0;
    for (const tur_seg_rec *q = r; q && q->map; q = q->prev) k++;
    tur_seg_image *im = (tur_seg_image *)calloc(1, sizeof(tur_seg_image) + (size_t)k * sizeof(tur_seg_piece));
    if (!im) return NULL;
    im->n = k;
    unsigned char *from = (unsigned char *)(((uintptr_t)mark - 64) & ~(uintptr_t)15);
    for (int i = 0; i < k; i++, r = r->prev) {
        tur_seg_piece *p = &im->p[i];
        if (from < r->bot) from = r->bot;
        p->map = r->map; p->map_size = r->map_size;
        p->bot = r->bot; p->lo = r->lo; p->top = r->top; p->saved_sp = r->saved_sp;
        p->from = from; p->n = (size_t)(r->top - from) & ~(sizeof(uintptr_t) - 1);
        p->data = (unsigned char *)malloc(p->n);
        if (!p->data) return NULL;
        tur_seg_copy(p->data, p->from, p->n);
        /* The region store-hook rule: stack words copied out past any bracket. */
        TUR_REGION_NOTE_WORDS(p->data, p->n);
        from = (unsigned char *)((uintptr_t)r->saved_sp & ~(uintptr_t)15);
    }
    if (r != &tur_seg_base_rec) return NULL;
    *resume = from;
    return im;
}
/* H3, the re-entry.  Each piece's segment has to be at its old address
 * again -- the frames hold pointers into it.  One the chain or the spare
 * still maps, or the pool holds, is reused; any other (a capture made
 * before the pool held the slot cannot be one) is mapped again with
 * MAP_FIXED_NOREPLACE, or the re-entry stops with a message when
 * something else took the address.  The work runs on the thread's own
 * stack below the image (`base_lo`, where the caller copies it), so no
 * stack it rewrites is under it; then `jump` copies the thread's own part
 * and longjmps.  The chain is just the thread's own stack while the pieces
 * go in, so a collection meanwhile scans only that. */
typedef struct { tur_seg_image *im; void (*jump)(void *); void *c; } tur_seg_reentry;
static TUR_THREAD_LOCAL tur_seg_reentry tur_seg_re;
static tur_seg_rec *tur_seg_rec_of(unsigned char *map, size_t map_size) {
    return (tur_seg_rec *)(map + map_size - sizeof(tur_seg_rec));
}
__attribute__((noinline, noreturn, used)) static void tur_seg_reenter_here(void) {
    tur_seg_image *volatile im = tur_seg_re.im;
    void (*volatile jump)(void *) = tur_seg_re.jump;
    void *volatile c = tur_seg_re.c;
    tur_seg_rec *old = tur_seg_cur;
    tur_seg_cur = &tur_seg_base_rec;
    __asm__ volatile ("" ::: "memory");
    for (int i = 0; i < im->n; i++) {
        tur_seg_piece *p = &im->p[i];
        int have = 0;
        for (tur_seg_rec *r = old; r && r->map; r = r->prev) if (r->map == p->map) have = 1;
        if (!have && tur_seg_spare && tur_seg_spare->map == p->map) { tur_seg_spare = 0; have = 1; }
        if (!have && tur_seg_pool_take(p->map)) have = 1;
        if (!have) {
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
            unsigned char *m = (unsigned char *)mmap(p->map, p->map_size, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
            if (m != p->map) {
                static const char msg[] = "call/cc: cannot re-enter the continuation: the address of a stack segment it was captured on is in use\n";
                if (write(2, msg, sizeof msg - 1) < 0) { }
                _exit(70);
            }
            mprotect(m, TUR_SEG_GUARD, PROT_NONE);
        }
    }
    /* Every segment of the old chain the image does not use is dead now. */
    for (tur_seg_rec *r = old; r && r->map;) {
        tur_seg_rec *prev = r->prev;
        int used = 0;
        for (int i = 0; i < im->n; i++) if (im->p[i].map == r->map) used = 1;
        if (!used) {
            if (!tur_seg_spare) tur_seg_spare = r; else tur_seg_pool_put(r->map);
        }
        r = prev;
    }
    size_t used_sz = 0;
    for (int i = im->n - 1; i >= 0; i--) {
        tur_seg_piece *p = &im->p[i];
        tur_seg_copy(p->from, p->data, p->n);
        tur_seg_rec *r = tur_seg_rec_of(p->map, p->map_size);
        r->prev = i + 1 < im->n ? tur_seg_rec_of(im->p[i + 1].map, im->p[i + 1].map_size) : &tur_seg_base_rec;
        r->map = p->map; r->map_size = p->map_size;
        r->bot = p->bot; r->lo = p->lo; r->top = p->top; r->saved_sp = p->saved_sp;
        used_sz += p->map_size;
    }
    tur_seg_used = used_sz;
    tur_seg_last = NULL;
    __asm__ volatile ("" ::: "memory");
    tur_seg_cur = tur_seg_rec_of(im->p[0].map, im->p[0].map_size);
    tur_seg_arm(tur_seg_cur);
    jump(c);
    __builtin_unreachable();
}
__attribute__((weak, noreturn)) void tur_seg_r7k_restore(void *img, unsigned char *base_lo,
                                                         void (*jump)(void *), void *c) {
    unsigned char *w = (unsigned char *)(((uintptr_t)base_lo - 8192) & ~(uintptr_t)15);
    if (w < tur_seg_base_rec.bot + 32768) {
        static const char msg[] = "call/cc: cannot re-enter the continuation: no room below it on the thread's stack\n";
        if (write(2, msg, sizeof msg - 1) < 0) { }
        _exit(70);
    }
    tur_seg_re.im = (tur_seg_image *)img; tur_seg_re.jump = jump; tur_seg_re.c = c;
#if defined(__x86_64__)
    __asm__ volatile ("movq %0, %%rsp\n\tcall tur_seg_reenter_here\n\tud2" :: "r"(w) : "memory");
#else
    __asm__ volatile ("mov sp, %0\n\tbl tur_seg_reenter_here\n\tbrk #0" :: "r"(w) : "memory");
#endif
    __builtin_unreachable();
}
/* The asm below defines it, in every unit; an object, so that calling it
 * through the procedure's own type is a cast no compiler warns about. */
#if defined(TUR_SEG_HOST)
#define TUR_SEG_TRAMP_GLOBAL ".globl tur_seg_tramp\n.type tur_seg_tramp, %function\n"
#else
#define TUR_SEG_TRAMP_GLOBAL ""
#endif
extern char tur_seg_tramp_code[] __asm__("tur_seg_tramp");
#if defined(__x86_64__)
__asm__ (
    ".text\n"
    ".p2align 4\n"
    TUR_SEG_TRAMP_GLOBAL
    "tur_seg_tramp:\n"
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    subq $176, %rsp\n"
    "    movq %rdi, 0(%rsp)\n    movq %rsi, 8(%rsp)\n    movq %rdx, 16(%rsp)\n"
    "    movq %rcx, 24(%rsp)\n    movq %r8, 32(%rsp)\n    movq %r9, 40(%rsp)\n"
    "    movaps %xmm0, 48(%rsp)\n    movaps %xmm1, 64(%rsp)\n    movaps %xmm2, 80(%rsp)\n"
    "    movaps %xmm3, 96(%rsp)\n    movaps %xmm4, 112(%rsp)\n    movaps %xmm5, 128(%rsp)\n"
    "    movaps %xmm6, 144(%rsp)\n    movaps %xmm7, 160(%rsp)\n"
    "    movq %rsp, %rdi\n"
    "    leaq 16(%rbp), %rsi\n"
    "    call tur_seg_enter\n"
    "    movq %rax, %r10\n"
    "    movq %rdx, %r11\n"
    "    movq 0(%rsp), %rdi\n    movq 8(%rsp), %rsi\n    movq 16(%rsp), %rdx\n"
    "    movq 24(%rsp), %rcx\n    movq 32(%rsp), %r8\n    movq 40(%rsp), %r9\n"
    "    movaps 48(%rsp), %xmm0\n    movaps 64(%rsp), %xmm1\n    movaps 80(%rsp), %xmm2\n"
    "    movaps 96(%rsp), %xmm3\n    movaps 112(%rsp), %xmm4\n    movaps 128(%rsp), %xmm5\n"
    "    movaps 144(%rsp), %xmm6\n    movaps 160(%rsp), %xmm7\n"
    "    testq %r10, %r10\n"
    "    jz 1f\n"
    "    movq %r10, %rsp\n"
    "    call *%r11\n"
    "    leaq -176(%rbp), %rsp\n"
    "    movq %rax, 0(%rsp)\n    movq %rdx, 8(%rsp)\n"
    "    movaps %xmm0, 16(%rsp)\n    movaps %xmm1, 32(%rsp)\n"
    "    movq %rsp, %rdi\n"
    "    call tur_seg_leave\n"
    "    movq 0(%rsp), %rax\n    movq 8(%rsp), %rdx\n"
    "    movaps 16(%rsp), %xmm0\n    movaps 32(%rsp), %xmm1\n"
    "    leave\n"
    "    ret\n"
    "1:  leave\n"
    "    jmp *%r11\n");
#elif defined(__aarch64__)
__asm__ (
    ".text\n"
    ".p2align 4\n"
    TUR_SEG_TRAMP_GLOBAL
    "tur_seg_tramp:\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    sub sp, sp, #208\n"
    "    stp x0, x1, [sp, #0]\n    stp x2, x3, [sp, #16]\n"
    "    stp x4, x5, [sp, #32]\n    stp x6, x7, [sp, #48]\n"
    "    str x8, [sp, #64]\n"
    "    stp q0, q1, [sp, #80]\n    stp q2, q3, [sp, #112]\n"
    "    stp q4, q5, [sp, #144]\n    stp q6, q7, [sp, #176]\n"
    "    mov x0, sp\n"
    "    add x1, x29, #16\n"
    "    bl tur_seg_enter\n"
    "    mov x9, x0\n"
    "    mov x10, x1\n"
    "    ldp x0, x1, [sp, #0]\n    ldp x2, x3, [sp, #16]\n"
    "    ldp x4, x5, [sp, #32]\n    ldp x6, x7, [sp, #48]\n"
    "    ldr x8, [sp, #64]\n"
    "    ldp q0, q1, [sp, #80]\n    ldp q2, q3, [sp, #112]\n"
    "    ldp q4, q5, [sp, #144]\n    ldp q6, q7, [sp, #176]\n"
    "    cbz x9, 1f\n"
    "    mov sp, x9\n"
    "    blr x10\n"
    "    sub sp, x29, #208\n"
    "    stp x0, x1, [sp, #0]\n"
    "    stp q0, q1, [sp, #16]\n    stp q2, q3, [sp, #48]\n"
    "    mov x0, sp\n"
    "    bl tur_seg_leave\n"
    "    ldp x0, x1, [sp, #0]\n"
    "    ldp q0, q1, [sp, #16]\n    ldp q2, q3, [sp, #48]\n"
    "    mov sp, x29\n"
    "    ldp x29, x30, [sp], #16\n"
    "    ret\n"
    "1:  mov sp, x29\n"
    "    ldp x29, x30, [sp], #16\n"
    "    br x10\n");
#endif
#if defined(TUR_SEG_HOST)
/* H6: what a JIT'd program's check calls (the JIT branch below). */
tur_seg_jit_pair *tur_seg_jit_hot(void) { return (tur_seg_jit_pair *)&tur_seg_hot; }
void tur_seg_jit_aim(void *fn, size_t nb) { tur_seg_fn = fn; tur_seg_nb = nb; }
/* The engine's entry thread, before main and after it returns. */
void tur_seg_jit_arm(size_t total, int asked) {
    tur_seg_budget_set(total, asked);
    tur_seg_thread_start(NULL);
}
void tur_seg_jit_end(void) { tur_seg_thread_end(); }
int tur_seg_jit_fault_is_overflow(const void *a) { return tur_seg_fault_is_overflow((const unsigned char *)a); }
#else
#if defined(__x86_64__)
#define TUR_SEG_SP(sp) __asm__ ("movq %%rsp, %0" : "=r"(sp))
#else
#define TUR_SEG_SP(sp) __asm__ ("mov %0, sp" : "=r"(sp))
#endif
/* clang guarantees the tail call; gcc makes it at -O2 all the same, and a
 * plain call reaches the trampoline with the arguments laid out alike. */
#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(musttail)
#define TUR_SEG_MUSTTAIL __attribute__((musttail))
#endif
#endif
#ifndef TUR_SEG_MUSTTAIL
#define TUR_SEG_MUSTTAIL
#endif
#define TUR_SEG_WORDS(x) ((sizeof(x) + 7) & ~(size_t)7)
#define TUR_SEG_CHECK(f, nb, args) do { \
    unsigned char *tur_seg_sp_; TUR_SEG_SP(tur_seg_sp_); \
    if (__builtin_expect((uintptr_t)(tur_seg_sp_ - tur_seg_hot.lo) > tur_seg_hot.span, 0)) { \
        tur_seg_fn = (void *)(f); tur_seg_nb = (nb); \
        TUR_SEG_MUSTTAIL return ((__typeof__(&f))(void *)tur_seg_tramp_code) args; \
    } } while (0)
#define TUR_SEG_CHECK_V TUR_SEG_CHECK
/* Where only MIR needs a check: a cycle gcc and clang close with sibling
 * calls (dk_run_impl, emit_dk_runtime.c). */
#define TUR_SEG_CHECK_JIT(f, nb, args) ((void)0)
#endif
#elif defined(__MIRC__) && defined(TUR_SEG_JIT)
/* H6, the program half under `tur jit`.  The pair is the host's thread-local
 * (c2mir has none of its own), read through one call; the stack pointer is
 * a local's address (c2mir has no asm).  The slow path is a plain call, not a
 * tail call -- c2mir has no musttail -- and reaches the host's trampoline with
 * the arguments laid out as the procedure's own call had them.  c2mir has no
 * typeof either, so the emitter writes the procedure's type ahead of it as
 * tur_seg_fnty_<name> (emit_fn_def_body, emit_fns.c). */
typedef struct { unsigned char *lo; uintptr_t span; } tur_seg_pair;
extern tur_seg_pair *tur_seg_jit_hot(void);
extern void tur_seg_jit_aim(void *fn, size_t nb);
extern void tur_seg_tramp(void);
#define TUR_SEG_JIT_ON 1
#define TUR_SEG_WORDS(x) ((sizeof(x) + 7) & ~(size_t)7)
#define TUR_SEG_CHECK(f, nb, args) do { \
    unsigned char tur_seg_probe_; const tur_seg_pair *tur_seg_h_ = tur_seg_jit_hot(); \
    if ((uintptr_t)(&tur_seg_probe_ - tur_seg_h_->lo) > tur_seg_h_->span) { \
        tur_seg_jit_aim((void *)(f), (nb)); \
        return ((tur_seg_fnty_##f *)(void *)tur_seg_tramp) args; \
    } } while (0)
#define TUR_SEG_CHECK_JIT TUR_SEG_CHECK
#define TUR_SEG_CHECK_V(f, nb, args) do { \
    unsigned char tur_seg_probe_; const tur_seg_pair *tur_seg_h_ = tur_seg_jit_hot(); \
    if ((uintptr_t)(&tur_seg_probe_ - tur_seg_h_->lo) > tur_seg_h_->span) { \
        tur_seg_jit_aim((void *)(f), (nb)); \
        ((tur_seg_fnty_##f *)(void *)tur_seg_tramp) args; \
        return; \
    } } while (0)
#endif
