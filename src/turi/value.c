#include "value.h"
#include "env.h"     /* TuriEnv layout + value_scratch/value_perm pools */
#include "arena.h"
#include "buf.h"     /* TURI_SYNTAX repr */
#include "forms.h"   /* form_print for TURI_SYNTAX */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TuriClosure is an incomplete type here; value.c only uses it as a pointer. */

/* ---------------------------------------------------------------------------
 * The value pool's object map (r7rs-callcc-memory-never-freed)
 *
 * A bump arena keeps no object boundaries, so a word pointing into the pool
 * says nothing about which object holds it.  This map records them: a bit per
 * 8-byte granule of every pool page that starts an allocation (strings start
 * on any byte, and take their granule).  An object runs from its start to the
 * next start, or to the end of its slab's used bytes.  Three more bit arrays
 * on the same pages give a start's kind -- a frame, a binding, a closure or a
 * struct, the kinds the interpreter hands out again (turi-dropped-closures-
 * kept-until-teardown: the pool sweep needs the kind exactly, not guessed
 * from the extent) -- and a fifth is the mark set of one sweep; those four
 * are allocated for a page when it first needs one.  Pages live in an
 * open-addressing table keyed by page number, each its own allocation so the
 * one-entry cache the allocation path consults stays valid as the table
 * grows.  About 2% of the pool, up to 8% where frames and structs are made,
 * and only for an env that runs `#lang r7rs`.
 * --------------------------------------------------------------------------- */

#define STARTS_PAGE_SHIFT 12u
#define STARTS_WORDS      ((1u << (STARTS_PAGE_SHIFT - 3u)) / 64u)   /* 8 */

/* `kind0`..`kind2` and `marks` are allocated for a page when it first needs
 * them: when an object of a kind with that bit is made on it, when a sweep
 * marks something on it.  A start's kind is kind0 + 2 * kind1 + 4 * kind2
 * (the TURI_VAL_* kinds). */
typedef struct StartsPage {
    uintptr_t  page;
    uint64_t   starts[STARTS_WORDS];
    uint64_t  *kind0;    /* TURI_VAL_FRAME and TURI_VAL_CLOSURE starts */
    uint64_t  *kind1;    /* TURI_VAL_BINDING and TURI_VAL_CLOSURE starts */
    uint64_t  *kind2;    /* TURI_VAL_STRUCT starts */
    uint64_t  *marks;    /* the current sweep's marks */
} StartsPage;

typedef struct { uintptr_t lo, hi; } StartsRange;

struct TuriObjStarts {
    StartsPage **tab;
    size_t       cap, n;
    StartsPage  *last;
    bool         complete;   /* on before the env evaluated anything */
    bool         broken;     /* a page could not be allocated: never complete */
    StartsRange *ranges;     /* a mark pass's snapshot of the used bytes */
    size_t       n_ranges, cap_ranges;
};

static size_t starts_slot(const struct TuriObjStarts *s, uintptr_t page) {
    uint64_t x = (uint64_t)page;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (size_t)x & (s->cap - 1);
}

static StartsPage *starts_find(const struct TuriObjStarts *s, uintptr_t page) {
    if (!s->cap) return NULL;
    for (size_t j = starts_slot(s, page); s->tab[j]; j = (j + 1) & (s->cap - 1))
        if (s->tab[j]->page == page) return s->tab[j];
    return NULL;
}

static StartsPage *starts_page(struct TuriObjStarts *s, uintptr_t page) {
    StartsPage *p = starts_find(s, page);
    if (p) return p;
    if (!s->cap || (s->n + 1) * 10 >= s->cap * 7) {
        size_t ncap = s->cap ? s->cap * 2 : 1024;
        StartsPage **nt = (StartsPage **)calloc(ncap, sizeof *nt);
        if (!nt) return NULL;
        StartsPage **old = s->tab;
        size_t ocap = s->cap;
        s->tab = nt;
        s->cap = ncap;
        for (size_t i = 0; i < ocap; i++) {
            if (!old[i]) continue;
            size_t j = starts_slot(s, old[i]->page);
            while (nt[j]) j = (j + 1) & (ncap - 1);
            nt[j] = old[i];
        }
        free(old);
    }
    p = (StartsPage *)calloc(1, sizeof *p);
    if (!p) return NULL;
    p->page = page;
    size_t j = starts_slot(s, page);
    while (s->tab[j]) j = (j + 1) & (s->cap - 1);
    s->tab[j] = p;
    s->n++;
    return p;
}

static inline void starts_note(TuriEnv *env, const void *ptr, size_t n) {
    struct TuriObjStarts *s = env->obj_starts;
    if (!s) return;
    turi_pool_count(env, n);
    uintptr_t a = (uintptr_t)ptr, page = a >> STARTS_PAGE_SHIFT;
    StartsPage *p = s->last;
    if (!p || p->page != page) {
        p = starts_page(s, page);
        if (!p) { s->broken = true; return; }
        s->last = p;
    }
    uint32_t g = (uint32_t)(a >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    p->starts[g >> 6] |= (uint64_t)1 << (g & 63u);
}

static uint64_t *starts_bits(struct TuriObjStarts *s, uint64_t **slot) {
    if (!*slot && !(*slot = (uint64_t *)calloc(STARTS_WORDS, sizeof(uint64_t))))
        s->broken = true;
    return *slot;
}

void turi_val_note_kind(TuriEnv *env, const void *ptr, int kind) {
    struct TuriObjStarts *s = env->obj_starts;
    if (!s) return;
    uintptr_t a = (uintptr_t)ptr, page = a >> STARTS_PAGE_SHIFT;
    StartsPage *p = s->last && s->last->page == page ? s->last : starts_find(s, page);
    if (!p) { s->broken = true; return; }
    uint32_t g = (uint32_t)(a >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    uint64_t bit = (uint64_t)1 << (g & 63u);
    uint64_t **slots[3] = { &p->kind0, &p->kind1, &p->kind2 };
    for (int k = 0; k < 3; k++) {
        if (kind & (1 << k)) {
            uint64_t *bits = starts_bits(s, slots[k]);
            if (!bits) return;
            bits[g >> 6] |= bit;
        } else if (*slots[k]) {
            (*slots[k])[g >> 6] &= ~bit;   /* TURI_VAL_PLAIN: forget it */
        }
    }
}

void turi_val_sweep_budget(TuriEnv *env, size_t bytes) {
    if (!env) return;
    env->pool_sweep_due = false;
    env->pool_since     = 0;
    env->pool_budget    = env->obj_starts ? bytes : 0;
}

void turi_val_starts_enable(TuriEnv *env, bool complete) {
    if (!env || env->obj_starts) return;
    struct TuriObjStarts *s = (struct TuriObjStarts *)calloc(1, sizeof *s);
    if (!s) return;
    s->complete = complete;
    env->obj_starts = s;
}

bool turi_val_starts_complete(const TuriEnv *env) {
    return env && env->obj_starts && env->obj_starts->complete && !env->obj_starts->broken;
}

static void starts_clear_range(const void *p, size_t n, void *ud) {
    struct TuriObjStarts *s = (struct TuriObjStarts *)ud;
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)7, end = (uintptr_t)p + n;
    const uintptr_t page_bytes = (uintptr_t)1 << STARTS_PAGE_SHIFT;
    while (a < end) {
        uintptr_t page = a >> STARTS_PAGE_SHIFT;
        uintptr_t stop = (page + 1) << STARTS_PAGE_SHIFT;
        if (stop > end) stop = end;
        StartsPage *pg = starts_find(s, page);
        if (pg) {
            uint32_t g0 = (uint32_t)((a & (page_bytes - 1)) >> 3);
            uint32_t g1 = (uint32_t)(((stop - 1) & (page_bytes - 1)) >> 3) + 1;
            for (uint32_t g = g0; g < g1; g++) {
                uint64_t keep = ~((uint64_t)1 << (g & 63u));
                pg->starts[g >> 6]   &= keep;
                if (pg->kind0) pg->kind0[g >> 6] &= keep;
                if (pg->kind1) pg->kind1[g >> 6] &= keep;
                if (pg->kind2) pg->kind2[g >> 6] &= keep;
            }
        }
        a = stop;
    }
}

void turi_val_starts_forget(TuriEnv *env, Arena *a) {
    if (env && env->obj_starts) arena_each_used(a, starts_clear_range, env->obj_starts);
}

void turi_val_starts_free(TuriEnv *env) {
    struct TuriObjStarts *s = env ? env->obj_starts : NULL;
    if (!s) return;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->tab[i]) {
            free(s->tab[i]->kind0);
            free(s->tab[i]->kind1);
            free(s->tab[i]->kind2);
            free(s->tab[i]->marks);
        }
        free(s->tab[i]);
    }
    free(s->tab);
    free(s->ranges);
    free(s);
    env->obj_starts = NULL;
}

static void starts_range_cb(const void *p, size_t n, void *ud) {
    struct TuriObjStarts *s = (struct TuriObjStarts *)ud;
    if (!n) return;
    if (s->n_ranges == s->cap_ranges) {
        size_t ncap = s->cap_ranges ? s->cap_ranges * 2 : 64;
        StartsRange *nr = (StartsRange *)realloc(s->ranges, ncap * sizeof *nr);
        if (!nr) { s->broken = true; return; }
        s->ranges = nr;
        s->cap_ranges = ncap;
    }
    s->ranges[s->n_ranges].lo = (uintptr_t)p;
    s->ranges[s->n_ranges].hi = (uintptr_t)p + n;
    s->n_ranges++;
}

static int starts_range_cmp(const void *a, const void *b) {
    uintptr_t x = ((const StartsRange *)a)->lo, y = ((const StartsRange *)b)->lo;
    return x < y ? -1 : x > y;
}

bool turi_val_mark_begin(TuriEnv *env) {
    if (!turi_val_starts_complete(env)) return false;
    struct TuriObjStarts *s = env->obj_starts;
    for (size_t i = 0; i < s->cap; i++)
        if (s->tab[i] && s->tab[i]->marks)
            memset(s->tab[i]->marks, 0, STARTS_WORDS * sizeof(uint64_t));
    s->n_ranges = 0;
    arena_each_used(&env->value_scratch, starts_range_cb, s);
    arena_each_used(&env->value_perm, starts_range_cb, s);
    if (s->broken) return false;
    qsort(s->ranges, s->n_ranges, sizeof *s->ranges, starts_range_cmp);
    return true;
}

bool turi_val_mark_end(TuriEnv *env) {
    if (!env || !env->obj_starts) return false;
    env->obj_starts->n_ranges = 0;
    return !env->obj_starts->broken;
}

static const StartsRange *starts_range_of(const struct TuriObjStarts *s, uintptr_t w) {
    size_t lo = 0, hi = s->n_ranges;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s->ranges[mid].hi <= w) lo = mid + 1;
        else if (s->ranges[mid].lo > w) hi = mid;
        else return &s->ranges[mid];
    }
    return NULL;
}

/* The highest start at or below granule address `a`, no lower than `floor`. */
static bool starts_prev(const struct TuriObjStarts *s, uintptr_t a, uintptr_t floor,
                        uintptr_t *out, StartsPage **out_page) {
    uintptr_t page = a >> STARTS_PAGE_SHIFT, fpage = floor >> STARTS_PAGE_SHIFT;
    int g = (int)((a >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u));
    for (;;) {
        StartsPage *p = starts_find(s, page);
        if (p) {
            for (int wi = g >> 6; wi >= 0; wi--) {
                uint64_t bits = p->starts[wi];
                if (wi == (g >> 6) && (g & 63) != 63)
                    bits &= ((uint64_t)1 << ((g & 63) + 1)) - 1;
                if (bits) {
                    int bit = 63 - __builtin_clzll(bits);
                    uintptr_t at = (page << STARTS_PAGE_SHIFT) | ((uintptr_t)(wi * 64 + bit) << 3);
                    if (at + 8 <= floor) return false;
                    *out = at;
                    *out_page = p;
                    return true;
                }
            }
        }
        if (page == fpage || page == 0) return false;
        page--;
        g = (int)((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    }
}

/* The lowest start above granule address `a`, or `ceil` when there is none
 * below it. */
static uintptr_t starts_next(const struct TuriObjStarts *s, uintptr_t a, uintptr_t ceil) {
    uintptr_t page = a >> STARTS_PAGE_SHIFT;
    int g = (int)((a >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u)) + 1;
    while ((page << STARTS_PAGE_SHIFT) < ceil) {
        StartsPage *p = starts_find(s, page);
        if (p) {
            for (int wi = g >> 6; wi < (int)STARTS_WORDS; wi++) {
                uint64_t bits = p->starts[wi];
                if (wi == (g >> 6)) bits &= ~(((uint64_t)1 << (g & 63)) - 1);
                if (bits) {
                    uintptr_t at = (page << STARTS_PAGE_SHIFT) |
                                   ((uintptr_t)(wi * 64 + __builtin_ctzll(bits)) << 3);
                    return at < ceil ? at : ceil;
                }
            }
        }
        page++;
        g = 0;
    }
    return ceil;
}

int turi_val_object_of(TuriEnv *env, uintptr_t w, uintptr_t *start, uintptr_t *end) {
    struct TuriObjStarts *s = env->obj_starts;
    if (!s || !s->n_ranges || w < s->ranges[0].lo || w >= s->ranges[s->n_ranges - 1].hi)
        return -1;
    const StartsRange *r = starts_range_of(s, w);
    if (!r) return -1;
    uintptr_t at;
    StartsPage *p;
    if (!starts_prev(s, w & ~(uintptr_t)7, r->lo, &at, &p)) return -1;
    uint32_t g = (uint32_t)(at >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    uint64_t bit = (uint64_t)1 << (g & 63u);
    *start = at;
    *end   = starts_next(s, at, r->hi);
    return (p->kind0 && (p->kind0[g >> 6] & bit) ? 1 : 0) |
           (p->kind1 && (p->kind1[g >> 6] & bit) ? 2 : 0) |
           (p->kind2 && (p->kind2[g >> 6] & bit) ? 4 : 0);
}

size_t turi_val_each_unmarked(TuriEnv *env, void (*fn)(void *obj, int kind, void *ud),
                              void *ud) {
    struct TuriObjStarts *s = env->obj_starts;
    if (!s || !s->n_ranges || s->broken) return 0;
    size_t n = 0;
    for (size_t i = 0; i < s->cap; i++) {
        StartsPage *p = s->tab[i];
        if (!p || (!p->kind0 && !p->kind1 && !p->kind2)) continue;
        /* A page wholly inside one snapshot range needs no check per object
         * (the usual case: only a slab's last page holds its end). */
        uintptr_t plo = p->page << STARTS_PAGE_SHIFT;
        const StartsRange *pr = starts_range_of(s, plo);
        bool whole = pr && pr->hi >= plo + ((uintptr_t)1 << STARTS_PAGE_SHIFT);
        for (uint32_t w = 0; w < STARTS_WORDS; w++) {
            uint64_t k0 = p->kind0 ? p->kind0[w] : 0, k1 = p->kind1 ? p->kind1[w] : 0;
            uint64_t k2 = p->kind2 ? p->kind2[w] : 0;
            uint64_t cand = p->starts[w] & (k0 | k1 | k2) & ~(p->marks ? p->marks[w] : 0);
            while (cand) {
                int bit = __builtin_ctzll(cand);
                cand &= cand - 1;
                uintptr_t at = (p->page << STARTS_PAGE_SHIFT) | ((uintptr_t)(w * 64 + bit) << 3);
                /* Only what the pass's snapshot holds: nothing made since was
                 * there to be marked. */
                if (!whole && !starts_range_of(s, at)) continue;
                fn((void *)at, (int)(((k0 >> bit) & 1) | (((k1 >> bit) & 1) << 1) |
                                     (((k2 >> bit) & 1) << 2)), ud);
                n++;
            }
        }
    }
    return n;
}

bool turi_val_marked(TuriEnv *env, uintptr_t start) {
    StartsPage *p = env->obj_starts ? starts_find(env->obj_starts, start >> STARTS_PAGE_SHIFT) : NULL;
    uint32_t g = (uint32_t)(start >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    /* An address the map never recorded as a start answers "marked": the
     * caller is about to hand back what is NOT marked. */
    if (!p || !((p->starts[g >> 6] >> (g & 63u)) & 1u)) return true;
    return p->marks && ((p->marks[g >> 6] >> (g & 63u)) & 1u);
}

bool turi_val_mark(TuriEnv *env, uintptr_t start) {
    StartsPage *p = starts_find(env->obj_starts, start >> STARTS_PAGE_SHIFT);
    if (!p) return false;
    uint32_t g = (uint32_t)(start >> 3) & ((1u << (STARTS_PAGE_SHIFT - 3u)) - 1u);
    uint64_t bit = (uint64_t)1 << (g & 63u);
    uint64_t *marks = starts_bits(env->obj_starts, &p->marks);
    if (!marks) return false;   /* broken: turi_val_mark_end reports it */
    if (marks[g >> 6] & bit) return false;
    marks[g >> 6] |= bit;
    return true;
}

/* ---------------------------------------------------------------------------
 * Env-owned value-payload pool
 * --------------------------------------------------------------------------- */

void *turi_val_alloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_scratch, n);
    starts_note(env, p, n);
    return p;
}

void *turi_val_calloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_scratch, n);
    starts_note(env, p, n);
    if (n) memset(p, 0, n);
    return p;
}

void *turi_val_calloc_aligned(TuriEnv *env, size_t n, size_t align) {
    void *p = arena_alloc_aligned(&env->value_scratch, n, align);
    starts_note(env, p, n);
    if (n) memset(p, 0, n);
    return p;
}

char *turi_val_strdup(TuriEnv *env, const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = arena_strdup(&env->value_scratch, s, n);
    starts_note(env, p, n + 1);
    return p;
}

/* turi-value-pool-scratch-promotion-plan: permanent-pool variants used only by
 * the promotion walk to relocate escaping values out of scratch. */
void *turi_val_perm_alloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_perm, n);
    starts_note(env, p, 0);
    return p;
}

void *turi_val_perm_alloc_aligned(TuriEnv *env, size_t n, size_t align) {
    void *p = arena_alloc_aligned(&env->value_perm, n, align);
    starts_note(env, p, 0);
    return p;
}

void *turi_val_perm_calloc(TuriEnv *env, size_t n) {
    void *p = arena_alloc(&env->value_perm, n);
    starts_note(env, p, 0);
    if (n) memset(p, 0, n);
    return p;
}

char *turi_val_perm_strdup(TuriEnv *env, const char *s) {
    if (!s) return NULL;
    char *p = arena_strdup(&env->value_perm, s, strlen(s));
    starts_note(env, p, 0);
    return p;
}

/* Process-global fallback pool for the env-less error/rejection constructors.
 * Lazily initialised on first use; reclaimed by turi_val_global_pool_free,
 * which turi_env_free calls (single-env embedding pattern). */
static Arena g_turi_val_pool;
static bool  g_turi_val_pool_init = false;

char *turi_val_strdup_global(const char *s) {
    if (!s) return NULL;
    if (!g_turi_val_pool_init) {
        arena_init(&g_turi_val_pool, 0);
        g_turi_val_pool_init = true;
    }
    return arena_strdup(&g_turi_val_pool, s, strlen(s));
}

void turi_val_global_pool_free(void) {
    if (g_turi_val_pool_init) {
        arena_free(&g_turi_val_pool);
        g_turi_val_pool_init = false;
    }
}

TuriValue turi_error(const char *msg) {
    return (TuriValue){TURI_ERROR, .as_error = turi_val_strdup_global(msg)};
}

TuriValue turi_errorf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return (TuriValue){TURI_ERROR, .as_error = turi_val_strdup_global(buf)};
}

TuriValue turi_rejection(const char *msg) {
    return (TuriValue){TURI_REJECTION, .as_error = turi_val_strdup_global(msg ? msg : "")};
}

void turi_print_value(FILE *out, TuriValue v) {
    switch (v.tag) {
    case TURI_NIL:
        fprintf(out, "nil");
        break;
    case TURI_BOOL:
        fprintf(out, "%s", v.as_bool ? "true" : "false");
        break;
    case TURI_INT:
        fprintf(out, "%lld", (long long)v.as_int);
        break;
    case TURI_FLOAT:
        fprintf(out, "%g", v.as_float);
        break;
    case TURI_CSTR:
        fprintf(out, "\"%s\"", v.as_cstr ? v.as_cstr : "");
        break;
    case TURI_CLOSURE: {
        /* Print as #<fn name> if we can access the function name */
        fprintf(out, "#<fn>");
        break;
    }
    case TURI_ERROR:
        fprintf(out, "#<error: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_EFFECT_CONT:
        fprintf(out, "#<continuation>");
        break;
    case TURI_STRUCT:
        fprintf(out, "#<struct>");
        break;
    case TURI_THROW:
        fprintf(out, "#<exception>");
        break;
    case TURI_FUTURE:
        fprintf(out, "#<future>");
        break;
    case TURI_REF:
        fprintf(out, "#<ref>");
        break;
    case TURI_STRUCT_TYPE:
        fprintf(out, "#<struct-type %s>", v.as_cstr ? v.as_cstr : "?");
        break;
    case TURI_GEN:
        fprintf(out, "#<generator>");
        break;
    case TURI_HANDLER:
        fprintf(out, "#<handler>");
        break;
    case TURI_REJECTION:
        fprintf(out, "#<rejection: %s>", v.as_error ? v.as_error : "");
        break;
    case TURI_SYNTAX: {
        /* Show the wrapped form's text: `#<syntax (+ 1 2)>`. */
        if (!v.as_syntax) { fprintf(out, "#<syntax>"); break; }
        Buf b;
        buf_init(&b);
        form_print(&b, v.as_syntax);
        fprintf(out, "#<syntax %.*s>", (int)b.len, b.data);
        buf_free(&b);
        break;
    }
    }
}
