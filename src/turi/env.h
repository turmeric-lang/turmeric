#ifndef TURI_ENV_H
#define TURI_ENV_H

/* Platform macros before system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif
#if defined(__APPLE__)
#  ifndef _XOPEN_SOURCE
#    define _XOPEN_SOURCE 700
#  endif
#endif

#include <setjmp.h>
#include <stdbool.h>
#include "compiler/diag.h"

#if defined(_WIN32)
/* Windows: no <ucontext.h>.  Win32 Fibers are the real equivalent; the shims
 * live in platform_ucontext_win.h (shared with turi/fiber.h). */
#  include "platform_ucontext_win.h"
#elif !defined(__EMSCRIPTEN__)
#  if defined(__APPLE__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
#  endif
#  include <ucontext.h>
#  if defined(__APPLE__)
#    pragma clang diagnostic pop
#  endif
#else
/* WASM: ucontext.h not available; use Emscripten Fibers as the coroutine
 * primitive.  Requires -sASYNCIFY=1 at link time.
 * Full shims (getcontext/swapcontext/makecontext) live in turi/fiber.h. */
#  include <emscripten/fiber.h>
#  ifndef TURI_ASYNCIFY_STACK_SIZE
#    define TURI_ASYNCIFY_STACK_SIZE 65536
#  endif
#  ifndef TURI_UCONTEXT_STUB_DEFINED
#    define TURI_UCONTEXT_STUB_DEFINED
typedef struct {
    emscripten_fiber_t fiber;
    char asyncify_stack[TURI_ASYNCIFY_STACK_SIZE];
} ucontext_t;
#  endif
#endif

#include "runtime/arena.h"
#include "runtime/buf.h"
#include "compiler/symbols.h"
#include "turi/value.h"

/* Phase S7: forward-declare async scheduler types (defined in turi/fiber.h) */
typedef struct TuriFiber    TuriFiber;
typedef struct TuriFuture   TuriFuture;
typedef struct TuriTimer    TuriTimer;
typedef struct TuriIoPending TuriIoPending;

/* RP5: list node for env-retained spice images. The full layout
 * lives here so both env.c (teardown) and ffi_thunk.c (push) can
 * traverse it without an extra header dependency. The TurSpiceImage
 * struct itself is still opaque (declared via the forward decl on
 * the field below). */
struct TurSpiceImageNode {
    struct TurSpiceImage      *image;
    struct TurSpiceImageNode  *next;
};

/* A per-eval-call arena node, kept alive until TuriEnv is freed.
 * Closures may hold Expr* pointers into these arenas. */
typedef struct ArenaNode {
    Arena            arena;
    struct ArenaNode *next;
} ArenaNode;

/* PS5: one runtime global as it stood when the prelude was snapshotted
 * (TuriEnv.prelude_globals). */
typedef struct TuriPreludeGlobal {
    struct EnvBinding *binding;
    TuriValue          value;
} TuriPreludeGlobal;

/* PS1: one committed turn of an env's accumulated forms (TuriEnv.acc_turns). */
typedef struct TuriAccTurn {
    uint32_t end_form;        /* n_acc_forms once this turn committed */
    bool     stdlib_preload;  /* g_turi_stdlib_preload during the turn -- it
                               * marks typeclasses stdlib-owned, so a replay
                               * must elaborate under the same value */
} TuriAccTurn;

/* turi-value-pool-residual-sites: a coroutine execution stack (fiber/generator).
 * These back a ucontext_t, so they must be mmap'd (native) / malloc'd (WASM) at
 * a stable address rather than bump-allocated from the value pool. Each one is
 * tracked here so turi_env_free can munmap/free it instead of leaking it for the
 * process lifetime. The node itself is pool-allocated (reclaimed with the env). */
typedef struct TuriCoroStack {
    void                 *base;   /* mmap/malloc base pointer of the stack */
    size_t                size;   /* byte length (needed for munmap) */
    struct TuriCoroStack *next;
} TuriCoroStack;

/* interp-collections-never-freed: an interpreter-created collection buffer
 * (currently a Vec; see below re: Set/Map).  The tree-walker allocates these
 * with raw calloc/malloc and hands back a bare TURI_INT carrier, so neither the
 * rc-drop path nor turi_env_free reclaims them -- they leak for the process
 * lifetime unless the program explicitly calls vec-free.  Each buffer is
 * registered here at native_*_new time and its `destroy` runs at turi_env_free,
 * bounding the create/teardown leak (and the interpreter harness leak gate).  An
 * explicit vec-free / set-free / map-free tombstones its node (box = NULL) so
 * teardown skips an already-freed buffer -- no double free.  The node itself is
 * pool-allocated (reclaimed with the env).
 *
 * Vec and Set/Map are both wired up.  A Vec box uniquely owns its data buffer,
 * so freeing each at teardown is trivially safe.  Set/Map (HAMT-backed) share
 * nodes across boxes, but each box owns exactly one reference to its persistent
 * HAMT, so a per-box tur_hamt_free at teardown reclaims shared structure through
 * the node refcounts (correct now that the delete path retains pulled-up
 * siblings -- docs/archive/history/hamt-delete-sibling-refcount-report.md). */
typedef void (*TuriCollBufFreeFn)(void *box);

/* TR3 (turi-interp-incremental-reclamation): the eval-boundary sweep.
 *
 * With scratch promotion on, a successful rewind proves the live value graph
 * is exactly what is reachable from the eval result + globals.  The sweep
 * then conservatively marks every tracked box whose address appears as an
 * int64 carrier anywhere in that graph (or inside another marked box) and
 * frees the rest -- bounding a long-lived env's collection memory at the
 * eval boundary instead of at teardown.
 *
 * `mark` hands one contained value to the marker (typed where the box knows
 * the element tag, a bare turi_int carrier otherwise).  `scan` enumerates a
 * box's contents through it and returns true only if the enumeration was
 * COMPLETE -- i.e. no entry could be hiding a reference the marker cannot
 * see (a Set/Map's untyped entries cannot rule out a struct-valued entry
 * holding a handle, so its scan returns false when non-empty).  Any marked
 * box with an incomplete scan makes the whole cycle mark-only: nothing is
 * freed, matching the plan's leak-on-doubt rule. */
typedef void (*TuriCollBufMarkFn)(TuriValue v, void *ctx);
typedef bool (*TuriCollBufScanFn)(void *box, TuriCollBufMarkFn mark, void *ctx);

typedef struct TuriCollBuf {
    void                *box;      /* wrapper allocation; NULL once freed/tombstoned */
    TuriCollBufFreeFn    destroy;  /* frees box (and any heap buffer it owns) */
    TuriCollBufScanFn    scan;     /* enumerate contained values; NULL = opaque */
    bool                 marked;   /* per-sweep scratch bit */
    struct TuriCollBuf  *next;
} TuriCollBuf;

/* SB4: Capability bits -- controls which operations are permitted in a
 * sandboxed environment.  TURI_CAP_ALL grants every capability (unrestricted).
 * TURI_CAP_NONE grants nothing (fully sandboxed). */
typedef uint32_t TuriCaps;
#define TURI_CAP_IO       (1u << 0)  /* stdout/stdin, raw-fd read/write, pipes */
#define TURI_CAP_FFI      (1u << 1)  /* dlopen/dlsym/dlclose, extern-c */
#define TURI_CAP_INLINE_C (1u << 2)  /* inline-C expressions */
#define TURI_CAP_ASYNC    (1u << 3)  /* (async ...) forms and the scheduler natives */
#define TURI_CAP_UNSAFE   (1u << 4)  /* raw-malloc, ptr-deref, unsafe-cast, ... */
#define TURI_CAP_IMPORT   (1u << 5)  /* (import ...) module loading */
/* security-audit-plan WP3: the classes the native classification table
 * (native_caps.c) needed that the builtin set never did. */
#define TURI_CAP_FS       (1u << 6)  /* open/create/remove/stat by path */
#define TURI_CAP_PROC     (1u << 7)  /* spawn, wait, exit the host process */
#define TURI_CAP_ENV      (1u << 8)  /* getenv / environ */
/* Every bit defined above.  A native that hands its argument to an evaluator
 * holding all capabilities (r7rs-eval-c-eval__) requires all of them. */
#define TURI_CAP_EVERY    ((TuriCaps)((1u << 9) - 1u))
#define TURI_CAP_ALL      (~(TuriCaps)0)
#define TURI_CAP_NONE     ((TuriCaps)0)

/* Per-env diagnostic sink callback (libturi-per-embed-env-and-peripherals
 * Gap 3).  Declared here because the function-pointer field lives on TuriEnv;
 * the public setter turi_env_set_diag_sink is in turi/eval.h.  `level` matches
 * DiagLevel; see eval.h for the field contract. */
typedef void (*TuriDiagSinkFn)(struct TuriEnv *env, int level, const char *code,
                               const char *file, uint32_t line,
                               uint32_t col_start, uint32_t col_end,
                               const char *message, void *ud);

/* A name→value binding in the global environment. */
typedef struct EnvBinding {
    const char       *name;   /* points into sym_arena (permanent) */
    TuriValue         value;
    struct EnvBinding *next;
} EnvBinding;

/* Open-addressing hash table for O(1) global lookup.
 * Each slot holds a pointer to an EnvBinding in the linked list,
 * or NULL for an empty slot. Sized as a power-of-two capacity. */
typedef struct EnvHashTable {
    EnvBinding **slots;
    uint32_t     cap;    /* always a power of 2 */
    uint32_t     count;
} EnvHashTable;

/* Persistent evaluation environment.
 * All per-eval arenas are kept alive here; callers must not free TuriEnv
 * while any closures from it are still referenced. */
typedef struct TuriEnv {
    Arena       sym_arena;       /* SymbolTable string storage — permanent */
    SymbolTable st;              /* Persistent symbol table */
    Buf         src_acc;         /* Accumulated prior source text */
    /* The leading region of src_acc that a reader-type change must NOT discard:
     * the stdlib preload every interpreter entry point evaluates into this same
     * env before handing it to a user.  A `#lang` switch resets src_acc because
     * prior input cannot be re-parsed under an incompatible reader -- but the
     * preload is not user input, and dropping it took `hamt-of` (and the rest of
     * the stdlib) with it, so the first collection literal after a switch failed
     * as "unknown function or operator".
     *
     * Captured by turi_env_pin_prelude once the preload sequence has run;
     * restored by turi_env_reset_to_prelude at every reader-switch site.  The
     * accumulation counters are pinned alongside the length so the prelude comes
     * back marked ALREADY-RUN rather than replayed: re-running it would re-execute
     * `(load "stdlib/...")` forms whose module registration is deduped by the
     * elaborator, which re-established the stdlib on a first switch and silently
     * failed to on the second.  src_pin_len == 0 means nothing is pinned, which
     * reproduces the historical "discard everything" behaviour. */
    size_t      src_pin_len;
    uint32_t    pin_toplevel;    /* prior_toplevel at pin time */
    uint32_t    pin_prog_items;  /* prior_prog_items at pin time */
    uint32_t    pin_acc_forms;   /* n_acc_forms at pin time */
    uint32_t    pin_next_line;   /* acc_next_line at pin time */
    uint32_t    prior_toplevel;  /* Count of top-level PARSED forms from prior evals */
    /* Count of already-evaluated non-file-scope-def PROGRAM items from prior
     * evals. Distinct from prior_toplevel: a (load ...) form expands inline to
     * many program items, so parsed-form count and program-item count diverge.
     * The evaluation range must skip already-run PROGRAM items, so it is keyed
     * off this, not the parsed count -- otherwise a re-elaborated (load ...)
     * shifts the boundary and previously-run top-level forms get evaluated
     * again (e.g. a prior (gen-next g) double-advances a suspended generator). */
    uint32_t    prior_prog_items;
    /* TR2 (turi-incremental-elaboration-design): opt-in incremental parse.
     * OFF by default -- the default path stays byte-identical (re-parse the
     * whole accumulated blob every eval). When on, turi_eval re-reads only the
     * newly appended source and reuses the prior evals' Forms, removing the
     * O(N^2) re-parse that dominates a long-lived session (Trowel / Try
     * Turmeric / Godot embeddings). Set via turi_env_set_incremental_elab. */
    bool          incremental_elab;
    /* Accumulated top-level Forms from all prior evals, in parse order. Each
     * Form* lives in the eval arena that parsed it (all retained in
     * eval_arenas) and is immutable after parse -- only forms.c constructors
     * ever write Form fields -- so reuse across evals is sound. The vector
     * itself is malloc/realloc'd, freed in turi_env_free. Committed only on a
     * successful eval, mirroring src_acc; reset to 0 whenever src_acc resets
     * (a reader-type change), since forms cannot mix across readers. */
    struct Form **acc_forms;
    uint32_t      n_acc_forms;
    uint32_t      cap_acc_forms;
    /* Line number at which the next appended source chunk begins, tracked
     * incrementally (counting newlines in the new text only) so resuming the
     * reader never rescans the prefix. 1-based; 0 means "not yet initialised". */
    uint32_t      acc_next_line;
    /* TR2.3: scratch buffer holding "accumulated source + this turn's source",
     * REUSED across evals. It is what the eval's SourceFile points at, so
     * diagnostics still see the whole session text -- but unlike the old
     * per-eval arena_strdup of the same blob it is not retained N times, which
     * was the last O(N^2) term once elaboration went incremental. Nothing holds
     * a pointer into it past its eval: Forms copy their bytes (form_str
     * arena_strdups), and the SourceFile is re-registered every turn. */
    Buf           src_combined;
    /* TR2.2b: persistent elaboration session (opaque ElabSession, elab.h). Holds
     * the accumulated scope / typeclass env / ADT+effect+module registries so a
     * new turn's forms elaborate against prior definitions WITHOUT re-elaborating
     * them -- the O(N^2) retained-elaboration term. `elab_session_forms` is how
     * many accumulated forms the session has already absorbed, so a turn hands
     * the elaborator only acc_forms[elab_session_forms .. n_acc_forms).
     * Discarded after any failed turn, since a partial program may have entered
     * its scope, and rebuilt on the next turn by replaying acc_turns below. */
    struct Elab  *elab_session;
    uint32_t      elab_session_forms;
    /* PS1 (playground-session-hygiene-plan): where each committed turn ENDS in
     * acc_forms, so a discarded session is rebuilt turn by turn -- the same
     * sequence of elaborate calls the session was originally built from, minus
     * the failed turn.  Without this the rebuild elaborated the whole
     * accumulated program in one call with stdlib_prefix = prior_toplevel,
     * which marks the user's own earlier turns as stdlib: re-running a program
     * after ANY failed turn then failed forever with "'main' is already
     * defined by an auto-loaded stdlib module".  Committed alongside src_acc;
     * pinned and reset alongside acc_forms. */
    struct TuriAccTurn *acc_turns;
    uint32_t      n_acc_turns;
    uint32_t      cap_acc_turns;
    uint32_t      pin_acc_turns;  /* n_acc_turns at pin time */
    /* PS5: the runtime half of the prelude -- every global binding and its
     * value once the preload and its native overrides are in place -- so
     * turi_env_rewind_to_prelude can put the RUNTIME back too, not just the
     * source and the elaboration session.  Sorted by binding address; NULL
     * until turi_env_snapshot_prelude runs.  Promotion roots (eval.c). */
    TuriPreludeGlobal *prelude_globals;
    uint32_t           n_prelude_globals;
    ArenaNode  *eval_arenas;     /* Linked list of per-call arenas (never freed) */
    /* turi-env-owned-value-arena-pool-plan: dedicated pools for TuriValue heap
     * payloads (closures, structs, captured frames/bindings, cons cells, ...),
     * distinct from eval_arenas (which holds AST/elaboration memory). Created in
     * turi_env_new, reclaimed wholesale in turi_env_free, so an embedding host
     * gets leak-clean teardown. Allocate via the turi_val_* helpers in value.h.
     *
     * turi-value-pool-scratch-promotion-plan splits the single pool into two:
     *  - value_scratch : default target of every turi_val_* allocation. When
     *    scratch promotion is OFF (the default) it is never rewound and behaves
     *    exactly like the old single value_arena -- everything lives until
     *    turi_env_free.  When promotion is ON, it is arena_reset at each
     *    top-level eval boundary after escapees are copied out.
     *  - value_perm : receives the promoted deep-copies of values that must
     *    survive a scratch reset (result + globals). Empty until promotion runs. */
    Arena       value_scratch;
    Arena       value_perm;
    /* turi-call-frames-never-reclaimed: free lists of call frames and frame
     * bindings handed back when an activation returns with nothing having
     * captured its frame (eval.c, frame_release).  Their nodes live in
     * value_scratch, so both lists are emptied whenever it is reset. */
    void       *frame_free;
    void       *binding_free;
    /* turi-call-pins-and-side-frames-not-reclaimed: the same for a released
     * frame's tyvar and dictionary pins (eval.c's TyvarBind / DictBind). */
    void       *tyvar_free;
    void       *dict_free;
    /* turi-value-pool-scratch-promotion-plan: opt-in bound on steady-state memory
     * for a single long-lived env (notebook-kernel pattern). When true, turi_eval
     * promotes every escaping value into value_perm and rewinds value_scratch at
     * each top-level boundary, so transient per-eval allocation does not
     * accumulate across evals. OFF by default: the per-unit-env embedding pattern
     * (create/eval/free) needs no promotion and the default path is unchanged.
     * Set via turi_env_set_scratch_promotion. */
    bool        scratch_promotion;
    /* TR0 measurement (turi-interp-incremental-reclamation-plan.md): per-env
     * scratch-promotion outcome tally, incremented once per top-level eval
     * boundary while scratch_promotion is on. Quantifies how often the
     * conservative walk actually rewinds vs declines, and why -- the signal
     * that decides whether the TR1 carrier-relocation work is load-bearing.
     * Zero-initialized (env is calloc'd); read directly by measurement harnesses. */
    uint64_t    promo_attempts;               /* promotion entered (feature on) */
    uint64_t    promo_rewinds;                /* reached arena_reset (scratch reclaimed) */
    uint64_t    promo_decline_busy;           /* bailed: env not quiescent (live control flow) */
    uint64_t    promo_decline_unrelocatable;  /* bailed: root set not relocatable (carrier/wscont) */
    EnvBinding *globals;         /* Global name→TuriValue map (linked list) */
    bool        sandboxed;       /* Deprecated alias: true when caps == TURI_CAP_NONE */
    TuriCaps    caps;            /* SB4: capability bitmask (TURI_CAP_ALL = unrestricted) */
    /* security-audit-plan S-5 (turi-sandbox-handles-are-forgeable-integers):
     * handle-provenance registry.  When `provenance_on` is set (a sandboxed or
     * macro env), the native dispatch records every interpreter handle a
     * constructor-classified native mints -- keyed by handle kind and pointer --
     * and refuses a later native call whose handle argument is not a live entry
     * of the right kind.  This closes the "handles are bare integers a native
     * casts to a pointer" forgery in a restricted env without gating collections
     * behind a capability.  `prov` is an open-addressing {kind, ptr} set; NULL
     * until the first mint (lazily allocated).  See src/turi/native_caps.c for
     * the per-native handle-signature column the dispatch reads. */
    bool        provenance_on;
    void       *prov;            /* TuriProvSet* (defined in eval.c); NULL = empty */
    /* Return-signal state: set by EX_RETURN, cleared by function application */
    bool        returning;
    TuriValue   return_value;
    /* Phase S3: effect handler stack (TuriHandlerFrame*, defined in eval.c) */
    void       *handler_stack;
    /* Phase S4: in-flight exception state (like returning, but for throw) */
    bool        throwing;
    TuriValue   throw_value;
    /* Phase S4: defer stack (DeferItem*, defined in eval.c) */
    void       *defer_stack;
    /* SR N4 (turi-cek-stackless-reentry): abortive-shift transfer signal.
     * An abortive (shift f _) sets these instead of longjmp-ing to a setjmp
     * reset boundary; the signal propagates like `throwing` (short-circuiting
     * the same `returning || throwing` guards) and is consumed by the nearest
     * matching reset boundary (a work-stack DK_RESET, or eval_reset_boundary
     * for the non-driver path).  This keeps delimited-control nesting on the
     * heap work-stack instead of one C frame per reset.  abort_prompt_kind is
     * a TuriPromptKind (int here; the enum lives in eval.c). */
    bool        aborting;
    TuriValue   abort_value;
    int         abort_prompt_kind;
    /* SR N4 Slice 2: when non-NULL, the abort is a call/cc *escape* targeting
     * this specific boundary (a TuriEscapeBoundary*), matched by pointer rather
     * than by abort_prompt_kind; reset boundaries pass it through.  NULL for a
     * plain shift abort (matched by prompt kind). */
    void       *abort_target;
    /* r7rs-lang-plan R6 (D7): the escape boundaries currently on the stack, so
     * `(k v)` on a continuation whose call/cc has already returned is the
     * named error the compiled path reports, not an abort nobody consumes. */
    void      **escape_live;
    uint32_t    n_escape_live, cap_escape_live;
    /* SB3: step-fuel resource limit (0 in both fields = unlimited).  Since C4
     * (turi-c-scoped-forms-heap-bounding) retired the eval_depth recursion
     * guard, step-fuel is the sole per-eval resource limit (it bounds total
     * work regardless of recursion shape; recursion itself is heap-bounded). */
    uint64_t    step_fuel;        /* remaining fuel units; decremented each eval step */
    uint64_t    step_fuel_limit;  /* initial limit set by turi_env_set_fuel */
    /* Panic state: set by EX_PANIC before firing defers; detects double-panic */
    bool        panicking;
    /* Set when currently inside a #[no-unwind] function call */
    bool        in_no_unwind;
    /* Phase S7: cooperative async scheduler */
    TuriFiber  *sched_ready_head;   /* ready queue head (FIFO) */
    TuriFiber  *sched_ready_tail;   /* ready queue tail */
    TuriFiber  *current_fiber;      /* fiber currently executing (NULL = main) */
    ucontext_t  sched_ctx;          /* context to swap back to from fibers */
    TuriTimer  *timers_head;        /* sorted timer list (ascending deadline) */
    TuriIoPending *io_pending_head; /* pending non-blocking I/O entries */
    uint32_t    io_pending_count;
    /* turi-session-types-plan (Slice C): env-slot analog of the compiled
     * `tur__rtv_` thread-local -- a successful `recv-timeout` stashes the
     * received value here; the recv-pair split's `tur__rtv_` inline-C reads it. */
    TuriValue   session_rtv;
    /* turi-value-pool-residual-sites: coroutine execution stacks (fiber +
     * generator), tracked so turi_env_free reclaims them. */
    TuriCoroStack *coro_stacks;
    /* interp-collections-never-freed: interpreter-created collection buffers
     * (currently Vec), tracked so turi_env_free reclaims the ones the program
     * never freed. */
    TuriCollBuf   *coll_bufs;
    /* TR3: recycled tracking nodes.  The eval-boundary sweep unlinks freed
     * AND tombstoned (explicitly vec-free'd) nodes here, and
     * turi_env_track_collection reuses them -- so node count is bounded by
     * peak simultaneous collections, not total ever created.  Nodes are
     * perm-pool allocations, so there is nothing to free(). */
    TuriCollBuf   *coll_bufs_free;
    /* TR3 observability, mirroring the promo_* counters: sweeps that ran a
     * free phase, sweeps declined as mark-only (an incomplete scan on a live
     * box), and total boxes freed by sweeps. */
    uint64_t       collsweep_runs;
    uint64_t       collsweep_markonly;
    uint64_t       collsweep_freed;
    /* All allocated futures (linked list for bulk free in turi_env_free) */
    TuriFuture *all_futures;
    /* Pipe fds for the built-in test I/O pipe (S7.7 tests) */
    int         test_pipe_rfd;
    int         test_pipe_wfd;
    /* Performance S8: hash table for O(1) global lookup */
    EnvHashTable globals_ht;
    /* Active reader syntax mode — settable via #lang in the REPL */
    ReaderType   reader_type;
    /* saffron-lang-plan S1: the LANGUAGE axis of the session, the twin of
     * reader_type above.  Sticky across an eval session the same way the reader
     * is: a `#lang saffron` blob keeps the dialect for the blobs that follow it
     * (the REPL is one long session), so it is set when a directive names one
     * and never silently reset to Turmeric by a later directive-free blob. */
    LangDialect  lang;
    /* Base directory for resolving module imports (NULL = ".").
     * Set this before turi_eval_file when the input uses (import ...). */
    const char  *module_base_dir;
    /* Extra module-search directories (`-I` flags plus the enclosing spice's
     * src/ and its :spices deps), searched after the importing file's
     * directory and the stdlib -- the same list the compiling per-file
     * commands hand the elaborator.  BORROWED: the embedder owns the array and
     * its strings and keeps them alive for the env's lifetime; turi_env_free
     * never touches them.  NULL/0 = none.
     * See docs/archive/interpret-takes-no-include-path-or-spice-discovery.md */
    const char **include_dirs;
    int          n_include_dirs;
    /* Phase R2: catch-unwind support — setjmp boundary for interpreter panic handling */
    jmp_buf     *catch_jmp;           /* active catch-unwind jmp_buf, or NULL */
    /* security-audit-plan S-5: while turi_eval runs a program in an env that
     * may not end the host (no TURI_CAP_PROC), the landing pad every panic
     * path jumps to instead of exit()/abort() -- including the uncatchable
     * ones (no-unwind, double panic).  NULL otherwise. */
    jmp_buf     *host_exit_jmp;
    char         catch_panic_msg[512]; /* copy of panic message when longjmp fires */
    /* Phase TI5: typed panic payload carried across the catch boundary so that
     * catch-panic-of can filter by type and the panic-payload-* accessors can
     * read the panicked value/file/line.  type_tag is a TypeKind (stored as int
     * to keep env.h free of the compiler type headers); 0 == TY_NIL/none. */
    int          catch_panic_type;    /* TypeKind tag of the in-flight panic */
    TuriValue    catch_panic_value;   /* the panicked value (cstr for plain panic) */
    const char  *catch_panic_file;    /* source file (best-effort; may be NULL) */
    int          catch_panic_line;    /* source line of the panic */
    /* SI4: TypeClassEnv* from latest turi_eval; used by turi_try_show for Show dispatch.
     * Points into an eval_arena (never freed). Cast to TypeClassEnv* in eval.c. */
    void        *last_tc_env;
    /* Session-scoped EffectEnv* for the per-turn effect-row check.  Turns are
     * elaborated INCREMENTALLY, so a `defeffect` from an earlier turn (the
     * stdlib preload, an earlier REPL line) is not in the program the check
     * pass sees this turn; a fresh env per turn resolved every such name to
     * nothing, so a caller's declared `#fx{Bt}` read as `#fx{}` while the
     * callee's, resolved on ITS turn, still carried Bt -- a spurious TUR-E0009
     * under --interpret only.  Registered effects accumulate here instead.
     * Points into an eval_arena (never freed).  Cast to EffectEnv* in eval.c. */
    void        *effect_env;
    /* Full elaborated Type of the last top-level result, as a `const Type *`
     * into the same eval_arena as last_tc_env (never freed; NULL when the turn
     * produced no new top-level expression).  The companion `type_tag` string
     * carries only the head constructor ("Map"), which is enough to FIND the
     * Show instance but not to show its ELEMENTS: a generic `Show [Map]` body
     * needs concrete K/V to re-resolve `(show (:: ... K))` away from the
     * int-carrier representative instance.  Kept as void* because env.h stays
     * free of the compiler type headers; cast to `const Type *` in eval.c.
     * See docs/archive/map-show-keyword-key-raw-int.md (root cause B). */
    void        *last_result_type;
    /* r7rs-repl-echoes-multiple-values-opaquely: the last new top-level item
     * of the turn was a definition (a `def`/`defn`).  A Scheme `define`
     * returns the value it bound, but its value is unspecified in R7RS, so
     * the R7RS prompt echoes nothing for it, as chibi and Racket do. */
    bool         last_result_is_def;
    /* RM Q#5: session-scoped reader-macro registry. Persists across REPL
     * turns so `(reader-macros/define ...)` on one line is visible to the
     * reader on the next. Allocated from sym_arena; entries' templates
     * point into eval_arenas (kept alive by TuriEnv until free). Opaque
     * to callers that don't include reader_macros.h. */
    struct ReaderMacroRegistry *reader_macros;
    /* RP3: loaded spice image (auto-discovered from cwd at REPL start).
     * NULL outside a project, or when --no-auto-spice was passed. Owned
     * by TuriEnv: freed by turi_env_free. Opaque to callers that don't
     * include spice_loader.h; the REPL's binding layer (RP4) walks it
     * to resolve `(import M :refer [...])`. */
    struct TurSpiceImage *spice_image;
    /* RP5: retired spice images held alive for the env's lifetime.
     * When (reload) swaps in a fresh image, the previous one stays
     * pinned because old TuriNativeFn bindings still reference its
     * strdup'd module/defn name strings via the globals hash table.
     * Freed in turi_env_free in reverse order. Opaque list node. */
    struct TurSpiceImageNode *retired_spice_images;
    /* Module-private name resolution (interpreter parity with the compiled
     * per-module mangling). `defining_mod` is the DefModule* whose body is
     * currently being evaluated (set by EX_DEFMODULE), so EX_FN_DEF can tell
     * exported from private defns. `current_module` is the module that owns the
     * closure currently executing (set by eval_apply), so a call to a private
     * name resolves to "<module>/<name>" before falling back to the bare name.
     * Both are borrowed Symbol/strdup'd strings with env (or longer) lifetime. */
    const void *defining_mod;     /* const DefModule* — opaque to avoid expr.h dep */
    const char *current_module;   /* owning module of the running closure, or NULL */
    /* Debugger Phase 2: when non-NULL, the eval loop yields to the interactive
     * debugger REPL on breakpoint / step. Allocated by turi_debug_enable,
     * reclaimed by turi_debug_disable / turi_env_free. Opaque (TuriDebugger* is
     * internal to turi/eval.c). A plain NULL check on the eval hot path keeps
     * non-debugger interp runs free of any per-node cost. */
    void       *debugger;
    /* libturi-per-embed-env-and-peripherals Gap 3: per-env diagnostic sink.
     * When non-NULL, turi_eval installs it into the process-global diag sink
     * for the duration of the call so this env's diagnostics route here instead
     * of stderr.  Set via turi_env_set_diag_sink. */
    TuriDiagSinkFn diag_sink;
    void          *diag_sink_ud;
    /* Gap 4: true when module_base_dir was set via turi_env_set_module_base_dir
     * (heap-owned, freed by turi_env_free).  Direct field assignments by the CLI
     * leave this false and retain their existing borrowed-pointer semantics. */
    bool        module_base_dir_owned;
    /* Gap 5: list of (free_fn, ud) finalizers for natives registered via
     * turi_env_register_native_ex.  Each fires once, in LIFO order, from
     * turi_env_free -- so an embedder can let a native's `ud` lifetime ride
     * along with the env it was registered on.  turi_env_reset does NOT fire
     * them (natives survive a reset).  Opaque node list (NativeFinalizer*,
     * defined in env.c). */
    void       *native_finalizers;
    /* Gap 7: per-env interpret-mode bit, snapshotted into the process-global
     * g_interpret_mode for the duration of each turi_eval on this env (restored
     * afterward).  Defaults to true (every turi_env_new caller is an interpreter
     * embedder).  Lets two co-resident libturi embedders -- one wanting
     * interpret-mode eval, one wanting compile-mode elaboration -- each see their
     * own setting per eval call without the deeper elaborator-threading refactor.
     * Set via turi_env_set_interpret_mode. */
    bool        interpret_mode;
    /* Gap 8: true when spice_image is BORROWED from another (prototype) env via
     * turi_env_set_shared_spice_image -- turi_env_free then leaves it alone
     * instead of calling tur_spice_image_free, so many per-script envs can share
     * one read-only loaded image rather than each carrying its own copy. */
    bool        spice_image_borrowed;
} TuriEnv;

/* Create a new unrestricted environment. */
TuriEnv *turi_env_new(void);

/* Create a sandboxed environment (I/O and FFI builtins disabled). */
TuriEnv *turi_env_new_sandboxed(void);

/* turi_env_new_with_natives is declared in turi/eval.h (it depends on
 * TuriNativeSpec / TuriNativeFn). */

/* Free all resources owned by env. Any closures captured from it become
 * dangling after this call. */
void turi_env_free(TuriEnv *env);

/* turi-value-pool-residual-sites: register a coroutine execution stack (a
 * fiber's or generator's mmap'd/malloc'd stack) so it is reclaimed by
 * turi_env_free instead of leaking for the process lifetime. The node is
 * allocated from the env value pool; `base`/`size` are the dealloc args. */
/* Returns the tracking node so callers (async fibers) can hold a back-pointer
 * for early reclaim; generators may ignore the return value. */
TuriCoroStack *turi_env_track_coro_stack(TuriEnv *env, void *base, size_t size);

/* interp-collections-never-freed: register an interpreter-created collection
 * buffer (a wrapper allocation, e.g. a Vec box) so turi_env_free reclaims it via
 * `destroy` if the program never freed it explicitly.  Returns the tracking
 * node; callers stash it inside the wrapper so an explicit free (e.g. vec-free)
 * can tombstone it in O(1) via turi_env_untrack_collection.  Returns NULL
 * (buffer untracked, no crash) when env or box is NULL. */
TuriCollBuf *turi_env_track_collection(TuriEnv *env, void *box,
                                       TuriCollBufFreeFn destroy,
                                       TuriCollBufScanFn scan);

/* interp-collections-never-freed: tombstone a tracking node whose buffer is
 * about to be freed explicitly, so the turi_env_free teardown walk skips it and
 * does not double-free.  NULL-safe. */
void turi_env_untrack_collection(TuriCollBuf *node);

/* Gap 8 (libturi-per-embed-env-and-peripherals): share one loaded spice image
 * across many per-script envs read-only, instead of each env auto-discovering
 * and owning its own copy.  Point every per-script env at a single prototype
 * env's already-loaded image; the borrowing env will NOT free it in
 * turi_env_free (the prototype owns it and must outlive every borrower).
 *
 * Pass `image` obtained from the prototype env's `spice_image` field (or NULL to
 * detach).  Borrowing replaces any image the env currently owns (that owned
 * image is freed first).  This is a measure-first hook: the report flags the
 * per-env image cost as speculative, so this gives embedders the sharing knob
 * without committing to copy-on-write arena machinery.  Safe only when no
 * borrower triggers `(reload)` on the shared image. */
void turi_env_set_shared_spice_image(TuriEnv *env, struct TurSpiceImage *image);

/* turi-value-pool-scratch-promotion-plan: opt into bounded steady-state memory
 * for a single long-lived env shared across many top-level evals (a notebook
 * kernel / long-lived REPL service).  With promotion ON, turi_eval deep-copies
 * every value that escapes a top-level eval (its result plus every global) into
 * a permanent pool and rewinds the scratch pool, so transient per-eval
 * allocations are reclaimed instead of accumulating.
 *
 * OFF by default -- the create-per-unit-of-work embedding pattern needs no
 * promotion and behaves exactly as before.  Enable only for the immortal-env
 * pattern.  Safe to toggle between top-level eval cycles, not from inside an
 * async/handler frame.  The promotion is conservative: when an eval leaves live
 * state the walk cannot prove safe to relocate (carrier-encoded pointer values,
 * live continuations/generators/fibers, pending async work), that eval's scratch
 * is left intact rather than corrupted -- it simply does not shrink that cycle. */
void turi_env_set_scratch_promotion(TuriEnv *env, bool enable);

/* TR2 (turi-incremental-elaboration-design): control incremental parsing +
 * elaboration for a long-lived env.  ON by default since 2026-07-25 (set
 * TUR_NO_INCREMENTAL_ELAB=1, or call this with false, to restore the
 * whole-program path).
 *
 * When enabled, turi_eval parses only the newly appended source each turn and
 * reuses the Forms parsed by earlier evals, instead of re-parsing the entire
 * accumulated session source every time (which is O(N^2) in both time and
 * retained AST over a session -- see docs/archive/history/turi-repl-quadratic-reparse.md).
 * The full accumulated blob is still handed to diagnostics, so spans and error
 * snippets render exactly as before.
 *
 * Results are identical to the default path: the same forms array is elaborated
 * either way, since parsed Forms are immutable and the reader-macro registry
 * persists on the env.  The interpreter automatically falls back to a whole-blob
 * re-parse for any turn it cannot handle incrementally (a sweet-exp reader, a
 * reader-type change, a `define` rewrite), so correctness never depends on the
 * fast path applying.  Safe to toggle between top-level eval cycles. */
void turi_env_set_incremental_elab(TuriEnv *env, bool enable);

/* Pin everything accumulated in env->src_acc so far as the stdlib prelude, so a
 * later reader-type change keeps it instead of emptying src_acc.  Call once,
 * after the turi_env_preload_* sequence and before the env is handed to a user;
 * all three interpreter entry points (native REPL, `--interpret`, WASM) do.
 * Only reader-agnostic plain s-expressions may be pinned -- the pinned text is
 * re-read under the NEW reader after a switch, a property src/main.c's file-eval
 * pre-detect already relies on.  Idempotent; a second call re-pins at the
 * current position. */
void turi_env_pin_prelude(TuriEnv *env);

/* Perform a reader-switch reset: drop accumulated USER source and the
 * elaboration session built from it, rewinding to the pinned prelude (or to
 * empty when nothing is pinned).  The caller sets env->reader_type itself --
 * this only handles the accumulation state, which every switch site
 * (turi_eval_impl, turi_eval_file, the REPL's `#lang` handler, and the WASM
 * set-lang entry point) previously open-coded and had to keep in sync. */
void turi_env_reset_to_prelude(TuriEnv *env);

/* PS5 (playground-session-hygiene-plan): record the prelude's runtime globals
 * for turi_env_rewind_to_prelude.  Call once the preload is complete --
 * AFTER turi_env_register_interpreter_natives, so the snapshot holds the
 * native overrides rather than the inline-C bodies they replace.  A second
 * call re-snapshots. */
void turi_env_snapshot_prelude(TuriEnv *env);

/* PS5: put the session back to the prelude, runtime included:
 * turi_env_reset_to_prelude, then every global binding added since the
 * snapshot is dropped and every prelude binding a later turn reassigned gets
 * its prelude value back.  Native bindings are always kept.  What the
 * playground's Run does before running the editor's program, so a name
 * deleted from the program stops resolving.  Without a snapshot this is
 * exactly turi_env_reset_to_prelude. */
void turi_env_rewind_to_prelude(TuriEnv *env);

/* Full language switch: assign the base reader, resetting the session when it
 * changes.  The session reader-macro registry is re-initialized too, and the
 * built-in dispatches are reinstalled on the next read.  A no-op when nothing
 * changes.  Used by the REPL `#lang` handler and the WASM set-lang entry
 * point.
 *
 * saffron-lang-plan S8: apply a reader AND language switch.  The plain
 * turi_env_apply_lang below keeps the session's current dialect, so an
 * existing caller is unchanged. */
void turi_env_apply_lang_dialect(TuriEnv *env, ReaderType reader_type,
                                 LangDialect dialect);
void turi_env_apply_lang(TuriEnv *env, ReaderType reader_type);

/* Look up a global binding by name.  Returns TURI_ERROR if not found. */
TuriValue turi_env_get(TuriEnv *env, const char *name);

/* Set (or replace) a global binding by name.  A new binding keeps the `name`
 * POINTER for env's lifetime, so it must be a literal or live in env's
 * sym_arena -- never a stack buffer. */
void turi_env_set(TuriEnv *env, const char *name, TuriValue value);

/* Helper for debugger: find an EnvBinding in env's globals. */
struct EnvBinding *turi_env_find_binding(TuriEnv *env, const char *name);

/* Helper for debugger: rebuild env's globals hash table after local overrides are cleared. */
void turi_env_rebuild_hash_table(TuriEnv *env);

#endif /* TURI_ENV_H */
