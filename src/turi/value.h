#ifndef TURI_VALUE_H
#define TURI_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* printf-style format checking for the wrappers below: under -Wformat=2 the
 * compiler checks every call's arguments against its format and refuses a
 * non-literal one (security audit WP5).  Off on Windows, where MinGW's printf
 * archetype disagrees with the C99 specifiers used here and -Werror is off. */
#ifndef TUR_PRINTF_FMT
#  if (defined(__GNUC__) || defined(__clang__)) && !defined(_WIN32)
#    define TUR_PRINTF_FMT(fmt_idx, first_arg) \
         __attribute__((format(printf, fmt_idx, first_arg)))
#  else
#    define TUR_PRINTF_FMT(fmt_idx, first_arg)
#  endif
#endif

/* Forward declarations */
typedef struct TuriClosure    TuriClosure;
typedef struct TuriEffectCont TuriEffectCont;  /* defined in eval.c */
typedef struct TuriGen        TuriGen;         /* defined in eval.c (TI2) */
typedef struct TuriHandlerVal TuriHandlerVal;  /* defined in eval.c (TI6) */
typedef struct TuriStruct     TuriStruct;      /* defined in eval.c */
typedef struct TuriThrow      TuriThrow;       /* defined in eval.c */
typedef struct TuriFuture     TuriFuture;      /* defined in turi/fiber.h */
typedef struct TuriEnv        TuriEnv;         /* from env.h */
struct Form;                                   /* compiler/forms.h -- syntax object payload */
struct Arena;                                  /* runtime/arena.h -- the value pools */

/* Tagged value type for the eval runtime. */
typedef enum TuriTag {
    TURI_NIL,
    TURI_BOOL,
    TURI_INT,
    TURI_FLOAT,
    TURI_CSTR,
    TURI_CLOSURE,
    TURI_ERROR,          /* parse/runtime error: as_error holds message */
    TURI_EFFECT_CONT,    /* live continuation from handle/perform (Phase S3) */
    TURI_STRUCT,         /* struct instance: TuriStruct* (Phase S4) */
    TURI_THROW,          /* in-flight exception: TuriThrow* (Phase S4) */
    TURI_FUTURE,         /* async future handle: TuriFuture* (Phase S7) */
    TURI_REF,            /* mutable borrow reference: void* to EvalBinding */
    TURI_STRUCT_TYPE,    /* struct type descriptor: as_cstr holds the name */
    TURI_GEN,            /* generator instance: TuriGen* (Phase TI2) */
    TURI_HANDLER,        /* first-class handler value: TuriHandlerVal* (TI6) */
    TURI_REJECTION,      /* async-task rejection: as_error holds message.
                          * Distinct from TURI_ERROR so the universal
                          * turi_is_error short-circuit does NOT propagate
                          * a rejected future value out of `(let [r (await
                          * ...)] ...)` -- callers observe it via (error?
                          * r) / (error-message r) and keep evaluating.
                          * (DEPR-R0; throw-deprecation-plan) */
    TURI_SYNTAX,         /* syntax object: as_syntax holds a compiler Form*
                          * (arena-resident -- the env's sym_arena or an
                          * eval arena; never scratch, never individually
                          * freed).  Stage 1 of
                          * docs/archive/macro-system-direction-plan.md.
                          * Appended at the end: "got tag %d" messages
                          * bake tag numbers into error text. */
} TuriTag;

typedef struct TuriValue {
    TuriTag tag;
    union {
        bool              as_bool;
        int64_t           as_int;
        void             *as_ref;   /* TURI_REF: EvalBinding* in interpreter */
        double            as_float;
        const char       *as_cstr;    /* borrowed or malloc'd string */
        TuriClosure      *as_closure;
        const char       *as_error;   /* malloc'd error message */
        TuriEffectCont   *as_cont;    /* live effect continuation */
        TuriStruct       *as_struct;  /* struct instance */
        TuriThrow        *as_throw;   /* in-flight exception */
        TuriFuture       *as_future;  /* async future handle (Phase S7) */
        TuriGen          *as_gen;     /* generator instance (Phase TI2) */
        TuriHandlerVal   *as_handler; /* first-class handler value (TI6) */
        struct Form      *as_syntax;  /* syntax object (compiler Form*) */
    };
} TuriValue;

/* Value constructors */
static inline TuriValue turi_nil(void)          { return (TuriValue){.tag = TURI_NIL, .as_int = 0}; }
static inline TuriValue turi_bool(bool b)       { return (TuriValue){TURI_BOOL, .as_bool = b}; }
static inline TuriValue turi_int(int64_t i)     { return (TuriValue){TURI_INT, .as_int = i}; }
static inline TuriValue turi_float(double f)    { return (TuriValue){TURI_FLOAT, .as_float = f}; }
static inline TuriValue turi_cstr(const char *s){ return (TuriValue){TURI_CSTR, .as_cstr = s}; }
static inline TuriValue turi_closure(TuriClosure *cl) {
    return (TuriValue){TURI_CLOSURE, .as_closure = cl};
}
static inline TuriValue turi_effect_cont(TuriEffectCont *c) {
    return (TuriValue){TURI_EFFECT_CONT, .as_cont = c};
}
static inline TuriValue turi_struct_val(TuriStruct *s) {
    return (TuriValue){TURI_STRUCT, .as_struct = s};
}
static inline TuriValue turi_throw_val(TuriThrow *t) {
    return (TuriValue){TURI_THROW, .as_throw = t};
}
static inline TuriValue turi_struct_type_val(const char *name) {
    return (TuriValue){TURI_STRUCT_TYPE, .as_cstr = name};
}
static inline TuriValue turi_gen_val(TuriGen *g) {
    return (TuriValue){TURI_GEN, .as_gen = g};
}
static inline TuriValue turi_handler_val(TuriHandlerVal *h) {
    return (TuriValue){TURI_HANDLER, .as_handler = h};
}
static inline TuriValue turi_syntax_val(struct Form *f) {
    return (TuriValue){TURI_SYNTAX, .as_syntax = f};
}
static inline bool turi_is_syntax(TuriValue v) { return v.tag == TURI_SYNTAX; }
static inline bool turi_is_throw(TuriValue v) { return v.tag == TURI_THROW; }

/* ---------------------------------------------------------------------------
 * Env-owned value-payload pool (turi-env-owned-value-arena-pool-plan)
 *
 * All heap payloads that *escape* an eval -- closures, structs, captured
 * frames/bindings, cons cells, boxes, ... -- are bump-allocated from the env's
 * value pool instead of raw libc, so turi_env_free reclaims them en masse.
 * Pool memory is NEVER individually freed: do not pass a turi_val_* pointer to
 * free().  Transients that are explicitly freed within one C call keep using
 * malloc/free.
 *
 * turi-value-pool-scratch-promotion-plan splits the pool into a scratch region
 * (the default target here) and a permanent region (turi_val_perm_*).  With
 * scratch promotion disabled -- the default -- scratch is never rewound, so this
 * is a transparent rename of the old single value_arena.
 * --------------------------------------------------------------------------- */

/* Bump-allocate n uninitialised bytes from env's scratch value pool. */
void *turi_val_alloc(TuriEnv *env, size_t n);
/* Bump-allocate n zeroed bytes from env's scratch value pool. */
void *turi_val_calloc(TuriEnv *env, size_t n);
/* Bump-allocate n zeroed bytes from env's scratch value pool, aligned to
 * `align` (power of two).  For payloads whose leading member has a stricter
 * alignment than the default pointer size -- e.g. a TuriFiber leading with a
 * ucontext_t that requires 16-byte alignment. */
void *turi_val_calloc_aligned(TuriEnv *env, size_t n, size_t align);
/* Copy s into env's scratch value pool (returns NULL when s is NULL). */
char *turi_val_strdup(TuriEnv *env, const char *s);

/* turi-value-pool-scratch-promotion-plan: allocate into env's PERMANENT value
 * pool instead of scratch.  Reserved for the promotion walk, which deep-copies
 * escaping values here so they survive the scratch rewind.  Ordinary eval code
 * never calls these -- it allocates from scratch and lets promotion relocate
 * whatever escapes. */
void *turi_val_perm_alloc(TuriEnv *env, size_t n);
void *turi_val_perm_alloc_aligned(TuriEnv *env, size_t n, size_t align);
void *turi_val_perm_calloc(TuriEnv *env, size_t n);
char *turi_val_perm_strdup(TuriEnv *env, const char *s);

/* r7rs-callcc-memory-never-freed: the value pool's object map.  Once enabled,
 * every turi_val_* allocation records where it starts, so a conservative scan
 * (the interpreter's continuation-image sweep, eval.c) can take any word that
 * points into the pool to the object holding it, and mark it.  `complete`
 * says the map was on before the env evaluated anything, so no object it
 * cannot place exists.
 *
 * A mark pass: turi_val_mark_begin snapshots the pool's used ranges and
 * clears the marks (false when the map is off or incomplete), and
 * turi_val_mark_end drops the snapshot.
 * turi_val_starts_forget clears the starts in arena `a` before it is
 * rewound (a scratch promotion). */
void turi_val_starts_enable(TuriEnv *env, bool complete);
bool turi_val_starts_complete(const TuriEnv *env);
void turi_val_starts_forget(TuriEnv *env, struct Arena *a);
void turi_val_starts_free(TuriEnv *env);
/* The interpreter's free-listed kinds -- frames, bindings, closures and
 * structs -- which a sweep must tell apart: one at an address a stale word
 * names may since have been handed out again (eval.c, frame_release), and
 * the pool sweep hands back each one nothing reached
 * (turi-dropped-closures-kept-until-teardown).  Noted once, at allocation. */
enum { TURI_VAL_PLAIN = 0, TURI_VAL_FRAME = 1, TURI_VAL_BINDING = 2, TURI_VAL_CLOSURE = 3,
       TURI_VAL_STRUCT = 4 };
void turi_val_note_kind(TuriEnv *env, const void *p, int kind);   /* 0: no longer one */
/* Raise env->pool_sweep_due once `bytes` more have been allocated from the
 * scratch pool (0: never), and lower it now.  The interpreter sweeps at its
 * next safe point and sets the next budget. */
void turi_val_sweep_budget(TuriEnv *env, size_t bytes);
/* Call fn on every noted (non-plain) object of the last mark pass's snapshot
 * that the pass did not mark, with its kind; between turi_val_mark_begin and
 * turi_val_mark_end.  Returns how many. */
size_t turi_val_each_unmarked(TuriEnv *env, void (*fn)(void *obj, int kind, void *ud),
                              void *ud);
bool turi_val_mark_begin(TuriEnv *env);
/* The object `w` points into: its kind (TURI_VAL_*), and its extent
 * [*start, *end); -1 for a word outside the pool's allocated objects. */
int  turi_val_object_of(TuriEnv *env, uintptr_t w, uintptr_t *start, uintptr_t *end);
/* Mark the object starting at `start`; true the first time. */
bool turi_val_mark(TuriEnv *env, uintptr_t start);
/* Whether the pass marked the object starting at `start` (true for an
 * address the map does not know as a start). */
bool turi_val_marked(TuriEnv *env, uintptr_t start);
/* False when the pass could not record everything (out of memory): its
 * marks are incomplete and nothing may be freed on them. */
bool turi_val_mark_end(TuriEnv *env);

/* Env-less fallback pool for the error/rejection constructors below, whose
 * signatures carry no env.  Backed by a process-global arena that turi_env_free
 * adopts and reclaims (the single-env embedding pattern the plan targets). */
char *turi_val_strdup_global(const char *s);
/* Reclaim the process-global fallback pool.  Called from turi_env_free. */
void  turi_val_global_pool_free(void);

/* Create an error value (message is strdup'd) */
TuriValue turi_error(const char *msg);
TuriValue turi_errorf(const char *fmt, ...) TUR_PRINTF_FMT(1, 2);

/* Create a rejection value (message is strdup'd).  Distinct from
 * turi_error so the interpreter does not short-circuit through it. */
TuriValue turi_rejection(const char *msg);

/* Predicates */
static inline bool turi_is_error(TuriValue v)   { return v.tag == TURI_ERROR; }
static inline bool turi_is_rejection(TuriValue v) { return v.tag == TURI_REJECTION; }

/* A value is truthy if it is not nil and not false */
static inline bool turi_is_truthy(TuriValue v) {
    if (v.tag == TURI_NIL)  return false;
    if (v.tag == TURI_BOOL) return v.as_bool;
    return true;
}

/* Accessors (return zero-value if wrong type) */
static inline bool        turi_as_bool(TuriValue v)  { return v.as_bool; }
static inline int64_t     turi_as_int(TuriValue v)   { return v.as_int; }
static inline double      turi_as_float(TuriValue v) { return v.as_float; }
static inline const char *turi_as_cstr(TuriValue v)  { return v.as_cstr; }
static inline const char *turi_error_message(TuriValue v) { return v.as_error; }

/* Print a TuriValue in REPL repr format (e.g. "42", "\"hello\"", "#<fn foo>") */
void turi_print_value(FILE *out, TuriValue v);

/* Phase S7: native function pointer type (for async builtins registered at runtime).
 * Must come after TuriValue and TuriEnv are fully declared. */
typedef TuriValue (*TuriNativeFn)(TuriEnv *env, TuriValue *args, uint32_t n, void *ud);

#endif /* TURI_VALUE_H */
