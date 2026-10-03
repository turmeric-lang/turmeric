#ifndef TUR_ELAB_INTERNAL_H
#define TUR_ELAB_INTERNAL_H

/* Shared internals for the elab_*.c translation units. This header is not
 * installed; the public elaborator API lives in elab.h. It exposes the Elab
 * state struct, supporting types, global linting flags, and prototypes for
 * every elaborator helper so the split translation units can call across
 * file boundaries. */

#include "elab.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "builtins.h"
#include "diag.h"
#include "reader.h"    /* Phase M2: read_all for module loading */
#include "typeclass.h"  /* Phase 15 */
#include "types.h"
#include "effect.h"    /* Phase 19 */
#include "globals.h"   /* ET4: g_effect_types_enabled */
#include "refine_collect.h" /* RT1: refinement proof obligations */
#include "scheme_lower.h"   /* SchemeGlobalKind (elab_scheme_global_kind) */
/* Phase U5: External declarations for global unsafe linting configuration */
extern uint32_t g_unsafe_max_lines;
extern bool g_unsafe_warn_nested;
extern bool g_unsafe_require_safety;
extern bool g_unsafe_stats_enabled;
extern bool g_lint_unsafe_enabled;
extern uint32_t g_unsafe_block_count;
extern uint32_t g_unsafe_total_lines;
/* Phase R6: Result/panic linting flags */
extern bool g_warn_unused_result;
/* F4 (cross-plan-followups): --Werror=deprecated promotes ^deprecated
 * warnings to errors so a clean build can gate against new uses. */
extern bool g_werror_deprecated;
extern bool g_lint_panic;
/* Phase C2: --no-contracts -- strip contract checks during elaboration */
extern bool g_no_contracts;
/* Phase G1: GADT feature flag */
extern bool g_gadt_enabled;
/* LT0: Linear types feature flag */
extern bool g_linear_enabled;
/* UT0: Uniqueness types feature flag */
extern bool g_unique_enabled;
/* ST0: Substructural types feature flag */
extern bool g_substructural_enabled;
/* SS0a: Session types feature flag */
extern bool g_sessions_enabled;
/* IT0: Union types feature flag */
extern bool g_union_types_enabled;
extern bool g_intersection_types_enabled;
#define ELAB_MAX_MACRO_EXPANSION_DEPTH 256

/* ---- shared file-scope state ---- */
/* MS2: Track whether we're inside an atomically block for TUR-E0502 checking.
 * Declared here (before elab_resume) and also set in elab_atomically below. */
extern bool elab_in_atomically;
/* ============================================================================
 * Phase 20: Software Transactional Memory - Elaboration
 * ============================================================================ */

/* Track whether we're inside an stm block for TUR-E0009 checking */
extern bool elab_in_stm;

/* ---- shared type definitions ---- */

/* SS5: Global protocol interaction tree (compile-time only, arena-allocated).
 * Represents the multi-party interaction structure of a defprotocol. */
typedef enum GlobalInteractionKind {
    GI_MSG,      /* (-> From To T) -- From sends T to To */
    GI_CHOICE,   /* (choice From [label branch] ...) -- From selects a labelled branch */
    GI_LOOP,     /* (loop label body...) -- recursive global protocol */
    GI_CONTINUE, /* (continue label) -- jump back to loop label */
    GI_END,      /* end of protocol */
    GI_TIMEOUT,  /* (timeout (-> From To T) [ok ...] [expired ...]) -- a timed
                  * receive: To may give up on From's message after a deadline
                  * supplied at the op (recv-timeout-from) */
} GlobalInteractionKind;

typedef struct GlobalBranch {
    const char               *label;
    struct GlobalInteraction *body;
} GlobalBranch;

typedef struct GlobalInteraction {
    GlobalInteractionKind kind;
    union {
        struct {
            const char               *from;  /* interned role name */
            const char               *to;    /* interned role name */
            struct Type              *msg;   /* message type */
            struct GlobalInteraction *rest;  /* next step */
        } msg;
        struct {
            const char               *decider;   /* role making the choice */
            GlobalBranch             *branches;  /* arena-allocated array */
            int                       n_branches;
            struct GlobalInteraction *rest;      /* steps after all branches (NULL if diverge) */
        } choice;
        struct {
            const char               *label;
            struct GlobalInteraction *body;
            struct GlobalInteraction *rest;  /* steps after loop (if any) */
        } loop;
        struct {
            const char *label; /* loop label to continue to */
        } cont;
        struct {
            const char               *from;     /* interned role name (sender) */
            const char               *to;       /* interned role name (timed receiver) */
            struct Type              *msg;      /* message type */
            /* Both continuations already carry the steps that follow the
             * timeout form: the parser appends the rest to each branch body,
             * so a role cursor never needs a continuation stack. */
            struct GlobalInteraction *ok;       /* the message arrived in time */
            struct GlobalInteraction *expired;  /* the receiver gave up */
        } timed;
    };
} GlobalInteraction;

/* SS5: Registry entry for a declared global protocol. */
typedef struct GlobalProtocol {
    const char  *name;       /* interned protocol name */
    struct Type *type;       /* TY_GLOBAL type node */
} GlobalProtocol;

/* ---- scope ---- */

/* Phase 12: Borrow tracking in Scope */
typedef enum BorrowKind {
    BK_IMMUT,   /* &T - immutable borrow */
    BK_MUT,     /* &mut T - mutable borrow */
} BorrowKind;

typedef struct ScopeBorrow {
    Binding *binding;       /* The binding being borrowed */
    BorrowKind kind;        /* BK_IMMUT or BK_MUT */
    struct ScopeBorrow *next; /* Next in the list */
} ScopeBorrow;

typedef struct Scope {
    struct Scope *parent;
    Binding     **bindings;
    uint32_t      n;
    uint32_t      cap;
    /* Phase 12: Active borrows in this scope */
    ScopeBorrow  *borrows;
    /* A name -> newest-binding index, built once the scope passes
     * SCOPE_INDEX_MIN bindings (in practice: the global scope, which holds
     * every stdlib and program definition).  Maintained by scope_add, the only
     * way bindings enter a scope; slots hold `index + 1`, 0 = empty.  See
     * scope_lookup. */
    uint32_t     *idx;
    uint32_t      idx_cap;
    uint32_t      idx_n;     /* distinct names indexed */
} Scope;

/* ---- elaborator state ---- */

/* Phase M2: A loaded module's exported bindings. */
typedef struct ElabModule {
    const Symbol *name;          /* module name (interned) */
    Binding     **exports;       /* exported bindings (each has .name for lookup) */
    uint32_t      n_exports;
    /* Phase M4: exported macros from this module */
    struct MacroDef **exported_macros;
    uint32_t         n_exported_macros;
    /* PR5-3-D: Effects exported by this module */
    Effect      **exported_effects;
    uint32_t      n_exported_effects;
    bool          is_loading;    /* circular import detection */
} ElabModule;

/* Forward-declared so this header doesn't drag in reader_macros.h. */
struct ReaderMacroRegistry;

typedef struct Elab {
    Arena       *arena;
    SymbolTable *st;
    Scope       *scope;     /* current */
    Scope        global;

    /* PS4 (playground-session-hygiene-plan): how big each redefinable registry
     * was when this elaborate call began.  Set only when the call continues a
     * live ElabSession -- a REPL or playground turn after the first -- and zero
     * otherwise, so a compile never treats anything as redefinable.  A
     * definition below its watermark came from an EARLIER turn, and a turn may
     * replace it: re-running a program must not fail on the definitions the
     * previous run left behind.  A duplicate within one turn is still the
     * error it always was.  See elab_prior_turn_* in elab_core.c. */
    bool      turn_continues_session;
    uint32_t  turn_start_n_globals;
    uint32_t  turn_start_n_macros;
    uint32_t  turn_start_n_adt_defs;
    uint32_t  turn_start_n_effects;
    struct TypeClassInstance *turn_start_instances;
    /* Same watermark, for the deferred RT1 crossings rather than a redefinable
     * registry: where THIS turn's call-site crossings begin.  The deferral is
     * per-UNIT and a turn is a unit, so an earlier turn's crossings are already
     * resolved and must not be walked again -- see refine_resolve_call_sites. */
    uint32_t  turn_start_n_refine_call_sites;
    uint32_t     next_id;
    /* r7rs-programs-compile-slowly: ids drawn inside the auto-loaded stdlib's
     * window (`in_stdlib_load`, or `ids_stdlib` for a pass that walks the
     * stdlib forms outside that bracket) come from their own counter,
     * starting at ELAB_STDLIB_ID_BASE.  The two passes over the forms each
     * visit the stdlib first and the program second, so from one counter the
     * program's own top-level names were numbered BETWEEN the stdlib's two
     * passes -- and every stdlib name after them (`r7rs_hyhandlers_un_un_3442`,
     * `__fn_1755`) moved with the size of the program.  A split build caches
     * the stdlib's C by a hash of its text, so it has to be the same text for
     * every program.  Draw an id through elab_fresh_id, never `next_id++`. */
    uint32_t     next_stdlib_id;
    bool         ids_stdlib;
    uint32_t     next_gensym_id;  /* Phase 6: for generating unique symbol names */
    /* Transitive-RM: shared reader-macro registry, set by the driver
     * before elaborate_program runs. Module loaders (elab_module.c,
     * elab_toplevel.c) read with this so imported files see the same
     * user macros the entry file did. May be NULL. */
    struct ReaderMacroRegistry *user_macros;

    /* Stage 1 (macro-system-direction-plan): lazily-created macro-time turi
     * env (see elab_macro_env_get in elab.h / src/turi/macro_env.c).  NULL
     * until first use; freed via elab_macro_env_dispose at the same three
     * sites as the malloc'd registries (both elaborate_program teardown
     * branches and elab_session_free). */
    struct TuriEnv *macro_env;

    /* Phase 3: Collect file-scope definitions (FN_DEF) from nested contexts */
    Expr       **file_scope_defs;
    uint32_t    n_file_scope_defs;
    uint32_t    cap_file_scope_defs;
    /* Phase 15: Typeclass environment */
    TypeClassEnv typeclass_env;

    /* Cached symbols for special-form dispatch. */
    /* G3 (mutable-globals-plan §4.3): the head of an `(export (mut g))` entry,
     * which marks an exported global as writable from outside its module. */
    const Symbol *sym_export_mut;
    const Symbol *sym_def;
    const Symbol *sym_define; /* internal define -- body form only */
    const Symbol *sym_let;
    const Symbol *sym_letstar; /* let* -- sequential-binding let (each binding sees prior ones) */
    const Symbol *sym_letrec; /* letrec -- recursive/mutually-recursive let */
    const Symbol *sym_if;
    const Symbol *sym_do;
    const Symbol *sym_unsafe;
    const Symbol *sym_when;
    const Symbol *sym_unless;
    const Symbol *sym_case;
    const Symbol *sym_set;
    const Symbol *sym_while;
    const Symbol *sym_defn;     /* Phase 2 */
    const Symbol *sym_fn;       /* Phase 2 */
    const Symbol *sym_c_fn;     /* typed-c-abi-function-pointers: (c-fn [A...] R) bare C fn-ptr type */
    const Symbol *sym_lambda;   /* λ — Unicode alias for fn */
    const Symbol *sym_extern_c; /* Phase 2 */
    const Symbol *sym_caret_mut;     /* ^mut */
    const Symbol *sym_caret_private; /* ^private — module-private visibility annotation */
    /* Phase P3: HAMT lowering */
    const Symbol *sym_caret_persistent; /* ^persistent — immutable map annotation */
    /* LT0: Linear types */
    const Symbol *sym_caret_linear;     /* ^linear — linear value annotation */
    /* UT0: Uniqueness types */
    const Symbol *sym_caret_unique;     /* ^unique -- unique value annotation */
    /* ST0: Substructural types */
    const Symbol *sym_caret_affine;     /* ^affine -- affine value annotation */
    const Symbol *sym_caret_relevant;
    /* G4a (mutable-globals-plan §4.4): `^atomic` on a top-level `def`. */
    const Symbol *sym_caret_atomic;
    /* G4b (mutable-globals-plan §4.4, §11.4): `^thread-local` on a `def`. */
    const Symbol *sym_caret_thread_local;   /* ^relevant -- relevant value annotation */
    /* toplevel-def-initializers-run-before-toplevel-expressions: `^deferred-init`
     * on a `def` elaborates the initializer for the binding's TYPE but does not
     * run it there; `(__tur-deferred-init__ name)` later in the program is the
     * initializer, where the Scheme lowering assigns it in the body. */
    const Symbol *sym_caret_deferred_init;
    const Symbol *sym_deferred_init;
    /* LB1: ^borrow -- non-consuming parameter annotation for linear/affine handles */
    const Symbol *sym_caret_borrow;     /* ^borrow -- borrow (read without consuming) annotation */
    const Symbol *sym_caret_fat;        /* ^fat -- fat-closure-consuming parameter (A#1) */
    const Symbol *sym_caret_extends;    /* ^extends -- effect hierarchy parent annotation (ET4) */
    const Symbol *sym_caret_capability; /* ^capability -- coarse capability effect tag (stdlib-effect-rows) */
    /* MS1: multi-shot continuation annotation */
    const Symbol *sym_caret_multishot;        /* ^multishot -- MS1: safe multi-shot via snapshot semantics */
    /* F4 (cross-plan-followups): ^deprecated definition annotation */
    const Symbol *sym_caret_deprecated;
    const Symbol *sym_caret_reflect;   /* reflected-measures RF0: `^reflect` on a defn */
    const Symbol *sym_map_new;    /* map-new - create new map */
    const Symbol *sym_assoc;      /* assoc - insert/update key-value */
    const Symbol *sym_dissoc;     /* dissoc - delete key */
    const Symbol *sym_map_get;    /* get - get value by key */
    const Symbol *sym_map_has;    /* has? - check key exists */
    const Symbol *sym_map_count;  /* count - number of entries */
    const Symbol *sym_map_merge;  /* merge - merge two maps */
    /* HAMT function symbols for lowering */
    const Symbol *sym_hamt_new;   /* hamt/new */
    const Symbol *sym_hamt_set;   /* hamt/set */
    const Symbol *sym_hamt_del;   /* hamt/del */
    const Symbol *sym_hamt_get;   /* hamt/get */
    const Symbol *sym_hamt_has;   /* hamt/has? */
    const Symbol *sym_hamt_count; /* hamt/count */
    const Symbol *sym_hamt_merge; /* hamt/merge */
    const Symbol *sym_hamt_hash_ptr; /* hamt_hash_ptr */
    /* Content-keyed cstr entry points -- :cstr keys route here so runtime-
     * built keys (equal text, distinct pointers) behave like literals. */
    const Symbol *sym_hamt_set_cstr; /* hamt/set-cstr */
    const Symbol *sym_hamt_del_cstr; /* hamt/del-cstr */
    const Symbol *sym_hamt_get_cstr; /* hamt/get-cstr */
    const Symbol *sym_hamt_has_cstr; /* hamt/has-cstr? */
    const Symbol *sym_defer;      /* Phase 4 */
    const Symbol *sym_return;     /* return - early return with defer firing */
    /* proper-tail-calls T1: `^tailcall` -- the checked tail-call annotation. */
    const Symbol *sym_tailcall;
    /* Phase 5 */
    const Symbol *sym_ref;        /* ref */
    const Symbol *sym_deref;      /* @ (deref operator) - stored as symbol for parsing */
    const Symbol *sym_drop;       /* drop! - explicit drop for ref<T> */
    const Symbol *kw_else;         /* the symbol named "else" -- matches both
                                    * the keyword `:else` and the bare `else` */
    const Symbol *kw_derive;       /* :as (the symbol named "as") - for inline-C */
    /* LT3: lref<T> — linear owning pointer */
    const Symbol *sym_lref;        /* lref (type name in type expressions) */
    const Symbol *sym_lref_new;    /* lref/new */
    /* Phase 9: rc<T> + weak<T> */
    const Symbol *sym_rc_of;       /* rc/of */
    const Symbol *sym_rc_clone;    /* rc/clone */
    const Symbol *sym_rc_drop;     /* rc/drop */
    const Symbol *sym_rc_ptr;      /* rc->ptr */
    const Symbol *sym_rc_strong_count; /* rc/strong-count */
    const Symbol *sym_rc_from_ref; /* rc/from-ref */
    const Symbol *sym_ref_from_rc; /* ref/from-rc */
    const Symbol *sym_weak;        /* weak */
    const Symbol *sym_upgrade;     /* upgrade */
    const Symbol *sym_weak_pred;   /* weak? */
    const Symbol *sym_ref_pred;    /* ref? */
    /* Phase 18: Delimited continuations */
    const Symbol *sym_reset;      /* reset */
    const Symbol *sym_shift;      /* shift */
    const Symbol *sym_shift0;     /* shift0 */
    const Symbol *sym_call_cc;    /* call/cc (sugar: (reset (shift k (f k)))) */
    const Symbol *sym_escape;     /* escape (sugar: (reset (shift k (f (fn [_] k))))) */
    /* Phase B2: Cloneable continuations */
    const Symbol *sym_cloneable_reset;   /* cloneable-reset */
    const Symbol *sym_cloneable_shift;   /* cloneable-shift */
    const Symbol *sym_call_cc_star;       /* call/cc* (sugar for cloneable call/cc) */
    /* Phase 21: Serializable continuations */
    const Symbol *sym_serial_reset;      /* serial-reset */
    const Symbol *sym_serial_shift;      /* serial-shift */
    /* Phase 19: Algebraic effects */
    const Symbol *sym_defeffect;  /* defeffect */
    const Symbol *sym_perform;    /* perform */
    const Symbol *sym_handle;     /* handle */
    const Symbol *sym_handle_shallow; /* handle-shallow (F2: shallow effect handler) */
    const Symbol *sym_try_with;   /* try-with (sugar for handle) */
    const Symbol *sym_with_handler; /* with-handler (sugar for handle; async-friendly alias) */
    const Symbol *sym_with;         /* WITH-V0: (with src [field value ...]) functional struct update */
    const Symbol *sym_resume;     /* resume */
    const Symbol *sym_discontinue;/* discontinue */
    const Symbol *sym_k;          /* continuation parameter name k */
    const Symbol *sym_effect_unsafe; /* built-in effect name: Unsafe */
    /* ET3: handler type expression and compose-handlers */
    const Symbol *sym_handler_type;     /* "handler" type expression keyword */
    const Symbol *sym_compose_handlers; /* "compose-handlers" call form */
    /* Phase U3: Unsafe primitives - pointer operations */
    const Symbol *sym_ptr_deref;   /* ptr-deref */
    const Symbol *sym_ptr_write;  /* ptr-write */
    const Symbol *sym_ptr_add;     /* ptr-add */
    const Symbol *sym_ptr_sub;     /* ptr-sub */
    const Symbol *sym_ptr_nullq;   /* ptr-null? */
    const Symbol *sym_ptr_of;      /* ptr-of */
    /* Phase U3: Unsafe primitives - type casting */
    const Symbol *sym_unsafe_cast; /* unsafe-cast */
    const Symbol *sym_reinterpret; /* reinterpret */
    const Symbol *sym_transmute;   /* transmute */
    /* Phase U3: Unsafe primitives - unchecked array ops */
    const Symbol *sym_array_get_unchecked;  /* array-get-unchecked */
    const Symbol *sym_array_set_unchecked;  /* array-set-unchecked */
    /* Phase U3: Unsafe primitives - raw memory */
    const Symbol *sym_raw_malloc;  /* raw-malloc */
    const Symbol *sym_raw_free;    /* raw-free */
    const Symbol *sym_raw_realloc; /* raw-realloc */
    const Symbol *sym_raw_memcpy;  /* raw-memcpy */
    const Symbol *sym_raw_memset;  /* raw-memset */
    /* Phase U3: Unsafe primitives - FFI */
    const Symbol *sym_c_call;      /* c-call */
    const Symbol *sym_call_ptr;    /* call-ptr (jit-ffi-c2mir-plan F3) */
    const Symbol *sym_callback_ptr; /* callback-ptr (jit-ffi-c2mir-plan F5) */
    const Symbol *sym_dlopen;      /* dlopen */
    const Symbol *sym_dlsym;       /* dlsym */
    const Symbol *sym_dlclose;     /* dlclose */
    /* Phase 10: GC */
    const Symbol *sym_gc_force;    /* gc! */
    const Symbol *sym_gc_enable;   /* gc-enable! */
    const Symbol *sym_gc_disable;  /* gc-disable! */
    const Symbol *sym_gc_auto;     /* gc-auto! (CG5, cycle-gc experiment) */
    const Symbol *sym_gc_collections;   /* gc-collections (CG6) */
    const Symbol *sym_gc_objects_freed; /* gc-objects-freed (CG6) */
    const Symbol *sym_gc_live_blocks;   /* gc-live-blocks (CG6) */
    const Symbol *sym_gc_cand_hw;       /* gc-candidate-high-water (CG6) */
    /* Phase 6: Macro system */
    const Symbol *sym_defmacro;   /* defmacro */
    const Symbol *sym_defmacro_star; /* defmacro* (Stage 2 procedural macros) */
    const Symbol *sym_quote;      /* quote */
    const Symbol *sym_quasiquote; /* quasiquote */
    const Symbol *sym_unquote;    /* unquote */
    const Symbol *sym_unquote_splicing; /* unquote-splicing */
    const Symbol *sym_gensym;     /* gensym */
    const Symbol *sym_thread;    /* -> threading macro */
    const Symbol *sym_thread_last; /* ->> threading macro */
    /* Phase 11: defstruct */
    const Symbol *sym_defstruct;   /* defstruct */
    const Symbol *sym_make_struct; /* make-struct */
    const Symbol *sym_default_of;  /* M2b: default-of */
    /* SI4-C: defopaque -- named opaque int64_t newtype for REPL type tags */
    const Symbol *sym_defopaque;   /* defopaque */
    const Symbol *kw_copy;        /* :copy keyword for defstruct */
    const Symbol *kw_move;        /* :move keyword for defstruct */
    const Symbol *kw_linear;      /* LT4: :linear keyword for defstruct (exactly-once) */
    const Symbol *kw_affine;      /* :affine keyword for defopaque (at-most-once) */
    const Symbol *kw_non_null;    /* :non-null keyword for defopaque -- the author
                                   * declares the handle's valid values exclude 0,
                                   * which is the Option-niche soundness condition */
    const Symbol *kw_sealed;      /* :sealed keyword for defopaque -- `::` cannot
                                   * cross the type/representation boundary
                                   * outside the declaring module */
    const Symbol *kw_heap;        /* :heap keyword for defstruct (typed-pointer ABI) */
    const Symbol *kw_no_auto_ctor;/* CTOR-V0: :no-auto-ctor keyword for defstruct */
    /* Phase 12: Borrow traits */
    const Symbol *sym_borrow;      /* & symbol for immutable borrow */
    const Symbol *sym_borrow_mut;  /* &mut for mutable borrow */
    /* Phase 15: Typeclasses */
    const Symbol *sym_defclass;    /* defclass */
    const Symbol *sym_definstance; /* definstance */
    /* Phase HKT H5: defkind — kind alias declarations */
    const Symbol *sym_defkind;     /* defkind */
    /* Phase HKT-P2: defrec — recursive type binders */
    const Symbol *sym_defrec;      /* defrec */
    const Symbol *sym_deftype;      /* deftype */
    /* Phase TA1/TA2: defalias — transparent type alias declarations.
     * TA1 accepted primitive keywords only; TA2 accepts any type expression
     * (composites included), so `type_alias_types` carries the full resolved
     * target and `type_alias_kinds` is its `kind` field, kept as a fast
     * TypeKind-only view for the ladders that only need the kind. */
    const Symbol *sym_defalias;

    const Symbol **type_alias_names;  /* interned alias name symbols */
    TypeKind      *type_alias_kinds;  /* resolved target TypeKind (== types[i]->kind) */
    Type         **type_alias_types;  /* resolved target type, arena-allocated */
    uint32_t       n_type_aliases;    /* number of declared aliases */
    uint32_t       cap_type_aliases;  /* allocated capacity */
    /* Phase HKT-P1: type-app — type-level application */
    const Symbol *sym_type_app;    /* type-app */
    /* Phase HRT0: Higher-ranked type quantifiers (type-level only; reject in expression position) */
    const Symbol *sym_forall;      /* forall */
    const Symbol *sym_exists;      /* exists */
    const Symbol *sym_forall_u;    /* ∀ — Unicode alias for forall */
    const Symbol *sym_exists_u;    /* ∃ — Unicode alias for exists */
    /* Phase HRT1: Rank-2 type expression forms */
    const Symbol *sym_arrow;       /* -> (function type constructor in type expressions) */
    const Symbol *sym_ascribe;     /* :: (type ascription operator) */
    /* Phase HRT2: Existential type intro/elim */
    const Symbol *sym_pack;        /* pack (existential introduction) */
    const Symbol *sym_open;        /* open (existential elimination) */
    /* Phase HKT (v2): reserved typeclass names — user definitions rejected with diagnostic */
    const Symbol *sym_hkt_Functor;
    const Symbol *sym_hkt_Applicative;
    const Symbol *sym_hkt_Monad;
    const Symbol *sym_hkt_Traversable;
    const Symbol *sym_hkt_Foldable;
    /* Phase R2: Panic */
    const Symbol *sym_panic;
    const Symbol *sym_panic_with;
    const Symbol *sym_catch_unwind;
    const Symbol *sym_catch_panic_of;
    /* Phase R5: no-unwind attribute */
    const Symbol *sym_no_unwind_attr;
    /* #[used] attribute: retain a defn with external C linkage (see
     * Binding.retain_c_linkage). */
    const Symbol *sym_used_attr;
    /* Phase M6: (export-as "c_name") attribute for explicit C symbol naming */
    const Symbol *sym_export_as_attr;
    /* M2a (end-to-end-monomorphization-plan): `^construct` on a polymorphic
     * stdlib constructor defn, `(defn ^construct some ...)`.  Picked up with
     * the other pre-name attributes in elab_defn.  (Was `^construct`,
     * an attribute borrowing the effect row -- effect-row-honesty-plan W0.) */
    const Symbol *sym_caret_construct;
    /* M5 residual-straddle retirement: `^byval` (was `^byval`).  See the
     * `prefer_byvalue_spec` comment on Binding in expr.h for the rationale
     * and demolition date. */
    const Symbol *sym_caret_byval;
    /* Sum-types T6: `(match ^non-exhaustive x ...)`, and `NonExhaustive`
     * for the deprecated `(match #fx{NonExhaustive} x ...)` (TUR-D0004). */
    const Symbol *sym_caret_non_exhaustive;
    const Symbol *sym_non_exhaustive_legacy;
    const Symbol *sym_panic_payload_type;
    const Symbol *sym_panic_payload_value;
    const Symbol *sym_panic_payload_file;
    const Symbol *sym_panic_payload_line;
    const Symbol *sym_panic_payload_downcast;
    /* Phase R1: ? operator (reserved; not yet implemented) */
    const Symbol *sym_question;
    /* Phase T19-B: thread-spawn form — (thread-spawn closure) */
    const Symbol *sym_thread_spawn;
    /* Phase T21-F: async/await forms */
    const Symbol *sym_async;
    const Symbol *sym_await;
    /* Phase SEL1: fair multi-channel select */
    const Symbol *sym_select;
    const Symbol *sym_recv;   /* :recv keyword */
    const Symbol *sym_send;   /* :send keyword */
    /* Phase 20: Software Transactional Memory */
    const Symbol *sym_stm;           /* stm */
    const Symbol *sym_atomically;    /* atomically */
    const Symbol *sym_retry;         /* retry */
    const Symbol *sym_check;         /* check */
    const Symbol *sym_or_else;       /* or-else */
    const Symbol *sym_tvar;          /* tvar (type name) */
    const Symbol *sym_new;           /* new (for tvar/new, etc.) */
    const Symbol *sym_read;          /* read (for tvar/read, etc.) */
    const Symbol *sym_write;         /* write (for tvar/write, etc.) */
    const Symbol *sym_modify;        /* modify (for tvar/modify, etc.) */
    const Symbol *sym_swap;          /* swap (for tvar/swap, etc.) */
    const Symbol *sym_cas;           /* cas (for tvar/cas, etc.) */
    const Symbol *sym_tmvar;         /* tmvar (type name) */
    const Symbol *sym_tchan;         /* tchan (type name) */
    const Symbol *sym_tsem;          /* tsem (type name) */
    const Symbol *sym_dosync;        /* dosync (macro) */
    const Symbol *sym_with_tvar;     /* with-tvar (macro) */
    /* Legacy symbols for TVar:: syntax (kept for compatibility) */
    const Symbol *sym_tvar_new;      /* tvar/new */
    const Symbol *sym_tvar_read;     /* tvar/read */
    const Symbol *sym_tvar_write;    /* tvar/write */
    const Symbol *sym_tvar_modify;   /* tvar/modify */
    const Symbol *sym_tvar_swap;     /* tvar/swap */
    const Symbol *sym_tvar_cas;      /* tvar/cas */
    /* Phase N: (as TargetType expr) numeric cast */
    const Symbol *sym_as;
    /* IT4 gradual typing */
    const Symbol *sym_type_of;  /* (type-of x) — cstr name of an any-typed value's type */
    const Symbol *sym_cast;     /* (cast x T) — unsafe downcast from any to T */
    const Symbol *sym_is_q;     /* TY3: (is? x T) — runtime type test on an any value */
    /* Phase 13: Lifetime annotations */
    /* We recognize lifetime annotations as symbols starting with '\'' */
    /* No specific symbol needed - we check the symbol name at runtime */
    /* Macro storage: symbol -> MacroDef */
    /* For now, use a simple approach: store macros in a list */
    struct MacroDef **macros;
    uint32_t n_macros;
    uint32_t cap_macros;
    /* Phase 19: Algebraic effects */
    EffectEnv *effect_env;  /* Global effect registry */
    /* Phase 19 TUR-E0008: Effect-scope tracking.
     * handled_effect_names: stack of effect names covered by enclosing handle.
     * fn_body_depth: depth inside defn/fn bodies; TUR-E0008 only fires at 0. */
    const Symbol **handled_effect_names;
    uint32_t n_handled_effects;
    uint32_t cap_handled_effects;
    uint32_t fn_body_depth;
    const Symbol *current_fn_name;  /* Phase R6: track current function name for linting */
    /* typeclass-method-resolution-ignores-the-class: the constraint vector of
     * the defn whose BODY is currently being elaborated (not the callee's).
     *
     * A typeclass method called on an abstract type variable is only well
     * formed when the enclosing generic declares a constraint naming that
     * method's class -- the constraint is what tells monomorphization which
     * instance the call site needs.  Without one the elaborator used to pick
     * an arbitrary name-matching instance as a "representative" and emit a
     * direct call to it, which `tur check` accepted and `cc` then rejected
     * with a mangled-symbol type error.  Recording the ambient set here lets
     * the dispatch path reject it with a Turmeric diagnostic instead.
     *
     * Saved/restored around the body alongside the HKT ambient below, so a
     * nested defn's own constraints shadow the enclosing one's. */
    TypeConstraint *cur_fn_constraints;
    /* nullary-class-method-unresolvable-over-newtype-tyvar: bit `ci` is set
     * when constraint `ci`'s tyvar appears in at least one PARAMETER type of
     * the enclosing generic.
     *
     * The distinction matters because monomorphization splits specializations
     * on argument types: two instantiations at same-carrier `defopaque`
     * newtypes (`Sum` vs `Product`, both int64) are distinct `Type`s, so their
     * arg vectors differ, two specs are interned, and Gap H's `__h<n>`
     * discriminator names them apart.  A constraint whose tyvar reaches no
     * parameter has nothing to split on -- both call sites share one spec --
     * so a representative chosen for it can never be re-resolved, and picking
     * one would be a silent wrong answer rather than a deferred decision.
     *
     * `TypeConstraint.return_resolved` answers almost this question, but only
     * for the `where (Class tyvar)` syntax; the `[^Class A]` caret forms hard-
     * code it false, and their construction sites run before params exist.
     * This mask is computed where both are in scope. */
    uint32_t        cur_fn_constraint_param_mask;
    uint8_t         cur_fn_n_constraints;
    /* typeclass-method-resolution-ignores-the-class: nesting depth inside
     * elab_definstance.  An instance method body legitimately calls class
     * methods on the INSTANCE's own type parameter (`Eq [Option]`'s `eq?`
     * recursing on the element `A`), and that obligation is carried by the
     * instance's constraint list, not by any enclosing defn's.  The
     * unconstrained-call check skips while this is non-zero. */
    uint32_t definstance_depth;
    /* saffron-lang-plan S9 (D8 Q1): set while saffron_mint_dyn_any_instance
     * elaborates a synthesised `C [any]` definstance, so the registration
     * stamps `dyn_any_minted` and the orphan check stands aside (the class is
     * usually the stdlib's and `any` is nobody's, yet the instance belongs to
     * the program that needs it). */
    bool minting_dyn_any;
    /* van-laarhoven-lens-composition: while elaborating the body of a constrained
     * rank-2 (higher-kinded) fn -- `(defn f [^g] [^Functor g ...] ...)` -- these
     * hold that fn's single HKT constraint's class and the abstract type-variable
     * name it constrains (`g`).  A nested call to ANOTHER constrained rank-2 fn
     * whose constraint pins to this same abstract variable forwards this fn's dict
     * instead of deferring (which segfaults on a hardcoded/absent instance).  NULL
     * outside such a body. */
    struct TypeClass *cur_hkt_constraint_class;
    const char       *cur_hkt_constraint_tyvar;
    /* van-laarhoven-lens-composition (Gap B2): the synthetic ambient-dict binding
     * for the current constrained rank-2 fn (see Binding.is_ambient_dict).  A
     * nested forwarding call references it so an adapter lambda captures it. */
    struct Binding   *cur_hkt_dict_binding;
    uint32_t unsafe_depth;
    uint32_t macro_expand_depth;
    /* ambiguous-dispatch-error-quality: span of the OUTERMOST macro call site
     * currently being expanded (recorded when macro_expand_depth goes 0->1).
     * A diagnostic raised deep inside a macro expansion (e.g. a `.method` call a
     * derive macro emits) can attribute itself to where the USER wrote the macro
     * call, instead of the macro-body span in stdlib/macros.tur.  Only valid when
     * macro_expand_depth > 0. */
    Span     macro_call_site_span;
    /* Phase U5: Unsafe linting configuration */
    uint32_t unsafe_max_lines;      /* max lines in unsafe block before warning (0 = disabled) */
    bool     unsafe_warn_nested;     /* warn on nested unsafe blocks */
    bool     unsafe_require_safety; /* require ;;; SAFETY: comments in unsafe blocks */
    bool     unsafe_stats_enabled;  /* track unsafe block statistics */
    uint32_t unsafe_block_count;    /* count of unsafe blocks seen */
    uint32_t unsafe_total_lines;    /* total lines in unsafe blocks */
    /* Phase 19: cont? predicate */
    const Symbol *sym_cont_pred; /* cont? */
    /* Phase G0: ADT registry - maps ADT names to AdtDef */
    AdtDef **adt_defs;
    uint32_t n_adt_defs;
    uint32_t cap_adt_defs;
    /* Phase G0: ADT special symbols */
    const Symbol *sym_defdata;
    const Symbol *sym_match;
    /* Phase G1: GADT special symbols */
    const Symbol *sym_defgadt;
    const Symbol *sym_colon; /* bare ":" used as annotation separator in defgadt */
    /* IT0: Union type pipe separator "|" */
    const Symbol *sym_pipe;        /* "|" -- used in (A | B | C) union type expressions */
    /* IT2: Intersection type ampersand separator "&" */
    const Symbol *sym_ampersand;   /* "&" -- used in (A & B & C) intersection type expressions */
    /* LS1: the lifetime context of the signature currently being elaborated, so
     * that a borrow *type* annotation like &'a int can intern its 'a into stable
     * per-function LifetimeIds.  NULL outside defn signature parsing (a borrow
     * type encountered with no active context simply gets no lifetime). */
    LifetimeContext *cur_lifetime_ctx;
    /* Phase RT (return-type-directed dispatch): a narrow, opt-in expected-type
     * channel.  Pushed by elab_ascribe ((:: e T)) and typed let binders
     * (let [x : T e]) before elaborating the inner/value expression; read by
     * elab_call to resolve return-resolved typeclass constraints from the
     * expected result type.  NULL everywhere it is not explicitly pushed, so
     * the common path is unaffected.  This is a one-slot stack: the call
     * elaborator clears it for nested sub-calls so it applies only to the
     * outermost call of the ascribed expression. */
    Type *expected_type;
    /* saffron-dynamic-surface-pass H10 x call/cc: set while the RECEIVER of a
     * `call/cc` is being elaborated.  elab_fn reads and clears it at entry, so
     * only the immediate receiver lambda sees it: that lambda keeps its
     * inferred (scalar) return instead of the Saffron `any` default, because
     * the CPS call/cc emitter assigns its result through a C cast to the
     * binder's type and an escape delivers an int64 -- neither is a
     * tur_tagged_t. */
    bool in_callcc_receiver;
    /* saffron-dynamic-surface-pass M10: whether the TOP-LEVEL form being
     * elaborated comes from a dynamically typed file (lang_span_is_dynamic),
     * set beside toplevel_stmt in pass 2.  Per-form spans cannot see through
     * a macro expansion -- `(when (map-get m k) ...)` in a Saffron file is
     * an `if` whose form is macros.tur's and whose condition is map.tur's --
     * so the seam and the truthiness rule consult this as well as the spans
     * they have.  Was `toplevel_saffron`; r7rs-lang-plan R0 renamed it to the
     * trait it records. */
    bool toplevel_dynamic;
    /* r7rs-lang-plan R2: the same, for Scheme truthiness (lang_span_is_scheme)
     * -- a macro-expanded `if` in a Scheme file must decide by Scheme's rule. */
    bool toplevel_scheme;
    /* saffron-effect-row-lost-through-unannotated-call: a user `defeffect` has
     * been elaborated in this unit.  Gates the CALL half of the dynamic-node
     * operand hoist (elab_hoist_control_operands): a unit with no effects can
     * never need it, and gating keeps a transform that would otherwise touch
     * every Saffron program off the code that cannot benefit.  Deliberately
     * NOT the effect env's count -- built-in effects (`Unsafe`) are always
     * registered, so that count is never zero. */
    bool unit_has_user_effect;
    /* Phase G2: current per-arm skolem environment (NULL outside GADT match arms) */
    SkolemEnv *g2_skolem_env;
    /* Phase G2: GADT constructor whose arm is currently being elaborated.
     * NULL outside GADT match arms.  Used by diagnostics to name which
     * constructor's return-type annotation caused a type refinement. */
    const CtorDef *g2_current_ctor;
    /* KB-025: named type variables quantified by the enclosing fn/defn
     * signatures (params + return type), accumulated outermost-first.  A GADT
     * match arm whose result type is a named type variable NOT in this set is
     * a skolem that escapes the arm into a concrete return position. */
    const char *sig_tyvars[32];
    /* van-laarhoven-lens-composition: the constructor kind (hkt_kind) of each
     * `sig_tyvars` entry, recorded in lockstep.  Lets a free type variable in a
     * NESTED lambda's type annotation recover the enclosing signature's
     * higher-kinded quantifier (e.g. a composed lens body's `(f Point)` where
     * `f : * -> *` is bound by the outer rank-2 fn) instead of defaulting to
     * kind `*` and tripping TUR-E0012. */
    Kind         sig_tyvar_kinds[32];
    uint8_t      n_sig_tyvars;
    /* generic-ctor-over-sig-tyvar-erased: the defn being elaborated is a
     * `^construct` template (stdlib `some`/`ok`/`err`...), whose bare-ctor
     * body the emitter types from the spec's RESULT, not its bindings. */
    bool         in_construct_template;
    /* Phase G3: coerce special form */
    const Symbol *sym_coerce;
    /* Phase G3: (~ a b) equality constraint notation */
    const Symbol *sym_tilde;
    /* Phase M0: Module system */
    const Symbol *sym_defmodule;  /* defmodule */
    const Symbol *sym_export;     /* export */
    /* (export-from <mod> name ...) -- re-export names another module already
     * exports, without a forwarding wrapper. */
    const Symbol *sym_export_from;
    const Symbol *sym_effect;     /* effect — used to parse (effect Name) in export/refer lists */
    const Symbol *sym_import;     /* import */
    const Symbol *sym_load;       /* load */
    const Symbol *kw_as;          /* :as */
    const Symbol *kw_refer;       /* :refer */
    const Symbol *kw_for_macros;  /* :for-macros (Stage 3 macro-time imports) */
    bool has_defmodule;           /* whether defmodule has been seen in this file */
    /* Phase M1: Module namespace system */
    const Symbol    *current_module_name; /* name of module being elaborated, or NULL */
    const DefModule *current_module;      /* DefModule being elaborated, or NULL */
    /* Phase M2: Module registry */
    const char      *module_base_dir;     /* base dir for resolving module file paths */
    const char      *module_stdlib_dir;   /* stdlib fallback dir (e.g. "stdlib") */
    const char     **module_include_dirs; /* extra search dirs from -I flags / spices */
    int              n_module_include_dirs;
    /* LS2 (local-spice-dev-workflow-plan): per-include-dir provenance for
     * the workspace-sibling warning. Each entry is parallel to
     * module_include_dirs[i]:
     *   - module_include_workspace_producer[i]: producer's workspace-member
     *     directory path (e.g. "spices/alpha") when this include dir came
     *     from auto-added workspace siblings; NULL otherwise.
     *   - module_include_warned[i]: set after the first warning is emitted
     *     for this dir so we never re-warn for the same (consumer, producer)
     *     pair within one tur invocation.
     * These arrays may be NULL when no workspace context applies. */
    const char     **module_include_workspace_producer;
    bool            *module_include_warned;
    /* LS2: names this consumer declares in its own :spices map, used to
     * decide whether a workspace-sibling resolution warrants a warning. */
    const char     **module_consumer_declared_spices;
    int              n_module_consumer_declared_spices;
    struct ElabModule *loaded_modules;    /* registry of loaded modules */
    uint32_t         n_loaded_modules;
    uint32_t         cap_loaded_modules;
    uint16_t         next_import_file_id; /* file_id counter for imported source files */
    /* load-not-expanded-in-imported-or-project-modules: compilation-global set
     * of canonical (load "path") paths already spliced+elaborated, shared by the
     * entry preprocessor (elaborate_program) and every imported module's
     * preprocessor (elab_load_module) so a path is expanded exactly once across
     * the whole build -- no duplicate symbols when the entry and an imported
     * module both `(load ...)` the same file. */
    const Symbol   **load_expanded_paths;
    uint32_t         n_load_expanded_paths;
    uint32_t         cap_load_expanded_paths;
    /* Phase M3: Separate compilation — skip inlining imported modules */
    bool             separate_compilation;
    /* load-not-expanded-in-imported-or-project-modules: true while elaborating
     * the forms of a module pulled in via `import` (inside elab_load_module).
     * Under separate compilation an imported module's definitions belong to its
     * own translation unit; registering them for emission in the importer's TU
     * re-emits them (e.g. a typeclass instance's method body referencing the
     * owner's internal ADT) where the supporting typedefs are absent.  The
     * emission-registration sites consult this flag (together with
     * separate_compilation) to skip re-registering imported defs. */
    bool             in_imported_module;
    /* class-and-generic-in-an-instance-less-module: a defn of an IMPORTED
     * module whose body failed only because a class it dispatches through
     * has no instance yet -- the class and a constrained generic over it
     * live in a module that holds none, the instances beside the types in
     * the importer.  Such a defn is parked (not reported) and retried at the
     * importer's next statement boundary after a new instance registers
     * (elab_noinst_retry), with its module context restored; the retried
     * definition is appended to that module's body.  Whatever is still
     * parked when the program ends is elaborated once more for real, so its
     * diagnostics still reach the user. */
    struct NoInstPending *noinst_pending;
    uint32_t         n_noinst_pending;
    uint32_t         cap_noinst_pending;
    /* The instance-list head at the last retry: an unchanged head means no
     * instance registered since, so a retry could not succeed. */
    const struct TypeClassInstance *noinst_seen_head;
    /* Bumped by the "declares no '<Class>' instance at all" diagnostic, so a
     * speculative attempt can tell that failure from any other. */
    uint32_t         noinst_failures;
    bool             noinst_retrying;
    /* SB2: When true, (import ...) is forbidden (sandboxed environment). */
    bool             sandboxed;
    /* MF3: true while elaborating the auto-loaded stdlib prefix; new global
     * bindings created during this window are marked is_from_stdlib so
     * user code that later shadows them gets a hard diagnostic instead
     * of broken C output. */
    bool             in_stdlib_load;
    /* The top-level form currently being elaborated by elaborate_program's
     * Pass 2, or NULL when not at file scope.
     *
     * `e->scope == &e->global` is true both for a `def` that IS a top-level
     * form and for a `def` buried in a top-level EXPRESSION's subforms
     * (`(if c (def x 1) (def y 2))`), so the scope pointer alone cannot tell
     * statement position from expression position.  elab_def needs that
     * distinction: the second shape elaborates as a global but codegen emits
     * it as a local, so a later reference fails in the emitted C with an
     * `undeclared identifier`.  See def_form_is_statement_position() and
     * docs/archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md. */
    const Form      *toplevel_stmt;
    /* Phase M4: During macro expansion, the defining module of the currently
     * expanding macro (so private helper macros from that module are visible).
     * Cross-module wrapper-macro bug fix: when an outer macro M (defined in
     * module A) emits a form referencing a stdlib wrapper W and an inner
     * helper H (also in A), and the form expands like
     * `(W (H ...))`, the inner W expansion overwrites macro_expansion_module
     * with W's defining module (NULL for stdlib), hiding H from the inner
     * lookup. We track the *stack* of in-progress expansion modules instead;
     * elab_lookup_macro walks the stack so any module in the active expansion
     * chain contributes its visibility. macro_expansion_module remains the
     * innermost entry for back-compat with sites that read it directly. See
     * docs/archive/history/cross-module-wrapper-macro-vec-arg-elaborated-as-expression.md. */
    const Symbol    *macro_expansion_module;
    const Symbol   **macro_expansion_stack;
    uint32_t         n_macro_expansion_stack;
    uint32_t         cap_macro_expansion_stack;
    /* Phase B2 CPS-CL7: tracks nesting depth of cloneable-reset for
     * detecting cloneable-shift outside any reset boundary. */
    int              cloneable_reset_depth;
    /* Item B (resuming-shift plan): keyword collapse.  A plain `reset` becomes
     * the reified delimiter (EX_CLONEABLE_RESET) only when a RESUMING (resume-k)
     * shift binds to it; otherwise it stays the abortive EX_RESET (which
     * shift0 / nested / substrate shapes need).  `reified_shift_at_depth[d]` is
     * set by elab_cloneable_shift's reified path at depth d and read by the
     * enclosing reset on exit. */
    bool             reified_shift_at_depth[64];
    /* Capability-folding (cps-shift-reset-capability-folding-plan item 1):
     * a plain `reset` promotes to the SERIAL reified delimiter (EX_SERIAL_RESET)
     * instead of the cloneable one (EX_CLONEABLE_RESET) when the resuming shift
     * that bound at depth d had a `serial-cont` receiver.  Set alongside
     * reified_shift_at_depth[d] by elab_cont_shift_core's serial route, read by
     * the enclosing `reset` on exit to pick the delimiter flavor. */
    bool             reified_serial_at_depth[64];
    /* Capability-folding item 1: true at depth d when the delimiter that
     * established depth d is the literal `cloneable-reset` KEYWORD (which pins the
     * cloneable flavor), false when it is a plain `reset` (flavor-flexible).  Set
     * by elab_cloneable_reset / cleared by elab_reset at entry, read by
     * elab_cont_shift_core's serial route so a `serial-cont` shift under a pinned
     * cloneable-reset gets a clear diagnostic instead of the misleading downstream
     * TUR-E0706 "context not capturable". */
    bool             pinned_cloneable_at_depth[64];
    /* cps-backend-n6 cross-function resume: set true the first time a resuming
     * shift with no lexical reset is lowered onto the synthetic __Shift effect
     * (elab_cont_shift_core).  Read by the gated post-elaboration pass
     * (elab_wrap_resets_for_crossfn_resume) so it wraps each reset's body in a
     * __Shift handler ONLY when the program actually performs one -- keeping every
     * non-using program's reset codegen byte-for-byte unchanged. */
    bool             uses_crossfn_resume;
    /* Span of the first cross-function resuming shift that desugared onto __Shift
     * -- used by the post-pass to point the "no enclosing reset anywhere" error at
     * a real source location. */
    Span             crossfn_resume_span;
    /* cps-backend-n6 cross-function resume: every EX_RESET / EX_CLONEABLE_RESET
     * node created during elaboration is recorded here (by elab_reset /
     * elab_cloneable_reset).  After elaboration, IF uses_crossfn_resume is set,
     * elab_wrap_resets_for_crossfn_resume wraps each recorded node's body in a
     * __Shift handler in place.  Recording is unconditional (cheap, no codegen
     * impact); the wrap is gated, so a program with no cross-function resuming
     * shift leaves every reset node untouched -- byte-for-byte identical codegen.
     * Storing the exact node pointers avoids a fragile whole-program tree walk. */
    Expr           **pending_reset_nodes;
    uint32_t         n_pending_reset_nodes;
    uint32_t         cap_pending_reset_nodes;
    /* CF7.3: the scope that was active immediately before the current function
     * body's inner scope was pushed (i.e., e->scope just before scope_init in
     * elab_fn/elab_defn).  check_cloneable_capture stops here so bindings
     * from outer function scopes are not falsely flagged as needing Clone. */
    struct Scope    *fn_entry_outer_scope;
    /* Edge 1 (hkt-matcher-cata-...): the binding group of the `letrec`/named-let
     * init currently being lifted.  Set by elab_letrec immediately around each
     * init's elaboration and snapshotted+cleared at elab_fn entry, so the
     * init's OWN top-level fn excludes the group (a direct self/mutual call is
     * recursion, handled by the recursion machinery / env-ptr), while a NESTED
     * closure inside the init sees an empty group and therefore captures the
     * letrec-bound value through its env.  NULL/0 when not in a letrec init. */
    struct Binding **letrec_self_group;
    uint32_t         letrec_self_group_n;
    /* Phase 21: tracks nesting depth of serial-reset for detecting
     * serial-shift outside any serial-reset boundary. */
    int              serial_reset_depth;
    /* Phase EX1d: nesting depth of `open` forms.  Each open mints a fresh
     * skolem id (open_skolem_next++); the depth counter exists so nested
     * opens cannot confuse each other's escape checks. */
    int              open_skolem_depth;
    uint32_t         open_skolem_next;
    /* Phase P3: HAMT lowering - track if HAMT functions are used */
    bool             needs_hamt;
    /* Phase P3: set true while elaborating the RHS of a ^persistent let binding
     * so that map-new (which has no first arg to inspect) can be lowered to
     * hamt/new even though is_persistent_map would otherwise be false. */
    bool             in_persistent_let;
    /* PR5-3-D: Effects brought into scope via :refer [(effect Name)] imports */
    Effect      **referred_effects;
    uint32_t      n_referred_effects;
    uint32_t      cap_referred_effects;
    /* Phase RF0: forward-declared type symbols for mutual recursion support.
     * Names added here during the type pre-pass are allowed to be re-elaborated
     * by defstruct/defdata without triggering "already defined" errors. */
    const Symbol   **forward_type_syms;
    uint32_t         n_forward_type_syms;
    uint32_t         cap_forward_type_syms;
    /* forward-call-to-aggregate-result-types-as-carrier: defmodule forward
     * decls whose named return waits on a type the module has not registered
     * yet (arena-allocated FwdPendingResult list, elab_toplevel.c). */
    void            *fwd_pending_results;
    /* CT0: Contract keyword symbols */
    const Symbol    *kw_pre;                /* :pre */
    const Symbol    *kw_post;               /* :post */
    const Symbol    *kw_invariant;          /* :invariant (loop-invariants-plan LI0) */
    const Symbol    *sym_result;            /* "result" -- bound name in :post predicates */
    const Symbol    *sym_tur_contract_check; /* tur-contract-check */
    /* SS0b: Session type constructor symbols (used in type annotations) */
    const Symbol    *sym_session_type;      /* "Session" — type constructor */
    const Symbol    *sym_session_Send;      /* "Send"    — protocol Send[T, Q] */
    const Symbol    *sym_session_Recv;      /* "Recv"    — protocol Recv[T, Q] */
    const Symbol    *sym_session_Close;     /* "Close"   — terminal protocol */
    const Symbol    *sym_session_Choose;    /* "Choose"  — internal choice */
    const Symbol    *sym_session_Branch;    /* "Branch"  — external choice */
    const Symbol    *sym_session_Rec;       /* "Rec"     — recursive protocol */
    const Symbol    *sym_session_Timeout;   /* "Timeout" -- timeout protocol (SS3c) */
    /* SS0b: Session channel operation symbols (used in expression position) */
    const Symbol    *sym_close;             /* "close"        */
    const Symbol    *sym_offer;             /* "offer"        */
    const Symbol    *sym_choose_left;       /* "choose-left"  */
    const Symbol    *sym_choose_right;      /* "choose-right" */
    const Symbol    *sym_make_session;      /* "make-session" */
    const Symbol    *sym_recv_timeout;      /* "recv-timeout" -- SS3c */
    /* SS3a: Session Rec label stack -- used during type parsing to resolve
     * bare label references inside (Rec label body) expressions.
     * rec_labels[i] is the interned label symbol; rec_types[i] is the
     * arena-allocated TY_SESSION_REC node being constructed (body filled later). */
#define ELAB_MAX_REC_DEPTH 8
    const Symbol    *rec_labels[ELAB_MAX_REC_DEPTH]; /* active Rec labels */
    struct Type     *rec_types[ELAB_MAX_REC_DEPTH];  /* corresponding TY_SESSION_REC nodes */
    uint8_t          rec_depth;                       /* number of active Rec binders */
    /* SS5: Global protocol symbols */
    const Symbol    *sym_defprotocol;  /* "defprotocol" */
    const Symbol    *sym_make_protocol; /* "make-protocol" */
    const Symbol    *sym_send_to;       /* "send-to" */
    const Symbol    *sym_recv_from;     /* "recv-from" */
    const Symbol    *sym_recv_timeout_from; /* "recv-timeout-from" */
    /* SS5: Global protocol type symbols (appear in type annotations) */
    const Symbol    *sym_global_type;   /* "Global" -- type constructor */
    const Symbol    *sym_role_type;     /* "Role"   -- type constructor */
    /* SS6: Projection type annotation */
    const Symbol    *sym_project_type;  /* "project" -- SS6: (project G R) type annotation */
    /* SS5: Global protocol registry */
    GlobalProtocol  *global_protocols;
    uint32_t         n_global_protocols;
    uint32_t         cap_global_protocols;
    /* DV0: Dynamic vars (-Xdynamic-vars) */
    const Symbol    *sym_defdynamic;        /* "defdynamic" */
    const Symbol    *sym_binding;           /* "binding" (DV1: dynvar override form) */
    DynVarEntry    **dynvar_entries;        /* all registered dynamic vars */
    uint32_t         n_dynvars;
    uint32_t         cap_dynvars;
    /* DV1: Active binding frames (compile-time set! scope check) */
    const Symbol   **active_dynvar_bindings; /* dynvar names with a live binding frame */
    uint32_t         n_active_dynvar_bindings;
    uint32_t         cap_active_dynvar_bindings;
    /* GF1: Generator elaboration context */
    struct GenContext *gen_ctx;     /* non-NULL when inside a (gen ...) body */
    uint32_t          gen_counter; /* monotonically increasing, for unique struct names */
    const Symbol     *sym_gen;          /* "gen" */
    const Symbol     *sym_yield;        /* "yield" */
    const Symbol     *sym_gen_next;     /* "gen-next" */
    const Symbol     *sym_gen_done;     /* "gen-done?" */
    const Symbol     *sym_gen_unwrap;   /* "gen-unwrap" (generator-yield-payload-is-int64-only) */
    /* CF5 (control-flow-completeness-plan): set true while elaborating a match arm body. */
    bool              in_match_arm;
    /* bare-fat-result-monomorphization-plan (Phase B): per-call-site
     * specialization of a bare-^fat callee over the incoming closure's result
     * kind.  See elab_specialize_bare_fat (elab_call.c) and elab_defn. */
    bool              bare_fat_spec_active;     /* re-elaborating a clone now */
    bool              bare_fat_force_canonical;  /* sweep: emit the deferred error */
    TypeKind          bare_fat_spec_kind;       /* the bare-^fat param's result kind */
    const Symbol     *bare_fat_spec_name;       /* mangled name for the clone */
    Binding          *bare_fat_spec_result;     /* clone's binding, filled by elab_defn */
    /* Memo of (orig callee, result kind) -> specialized binding.  A NULL spec
     * with matching (orig,kind) marks an in-progress specialization, so a
     * recursive reference is caught instead of looping forever. */
    struct BareFatSpec { Binding *orig; TypeKind kind; Binding *spec; } *bare_fat_specs;
    uint32_t          n_bare_fat_specs;
    uint32_t          cap_bare_fat_specs;
    /* Lazy bare-^fat bindings whose canonical (int) body was deferred; swept
     * after top-level elaboration so a never-specialized one still surfaces its
     * real (deferred) diagnostic instead of silently vanishing. */
    Binding         **bare_fat_lazy_bindings;
    uint32_t          n_bare_fat_lazy_bindings;
    uint32_t          cap_bare_fat_lazy_bindings;
    /* L6 follow-up (strict-row-elements): when non-zero, the unknown-name
     * fallthrough in type_expr_from_form (F_SYM line ~484 and F_KEYWORD
     * line ~1756) emits a hard error instead of returning a NULL-def
     * opaque placeholder. Used inside the #row{...} element resolution
     * loop so a typo'd component name in `#row{Pos Velocityy}` becomes a
     * compile error rather than silently elaborating to opaque. Counter
     * (not bool) so nested rows can save/inc/dec/restore around their
     * element recursion. */
    uint8_t           strict_unknown_types;
    /* defstruct-as-defadt: a `(make-struct Name ...)` on a NON-parametric
     * lowered record ADT rewrites to the auto-bound ctor call `(Name ...)`,
     * which then hits elab_call's strict positional arg typecheck.  At default,
     * make-struct of a non-parametric struct does NO field typecheck (it accepts
     * the value as-is, e.g. `0`-as-NULL for a ptr<void> field, a NULL ptr<void>
     * for an rc<T> field).  Set true around that rewrite's elab_call so the ctor
     * call's OWN arg check relaxes to parity with default make-struct.  elab_call
     * reads-and-clears it at entry, so nested calls during arg elaboration do not
     * inherit the leniency. */
    bool              make_struct_lenient_args;
    /* structdef-retirement slice 2 (CTOR-V0): a `:no-auto-ctor` lowered record ADT
     * keeps its value-namespace constructor binding (make-struct rewrites
     * `(make-struct Name ...)` to the ctor call `(Name ...)` and needs it), but a
     * DIRECT `(Name ...)` call must still be rejected.  make-struct sets this flag
     * around its rewrite's elab_call so the no_auto_ctor guard there lets the
     * rewritten call through; elab_call reads-and-clears it at entry so a nested
     * user `(Name ...)` during arg elaboration is still rejected. */
    bool              make_struct_ctor_rewrite;
    /* RT1 (refinement-types-plan): proof obligations collected from
     * `#refine{...}` crossings during this compilation unit.  Only populated
     * when the `refined` experiment is on; the discharge pass (RT3) decides
     * each one and the runtime contract check is elided exactly for those a
     * backend proved. */
    RefineObligationVec refine_obs;
    /* RT1 call-site crossings, recorded during elaboration and RESOLVED AFTER
     * IT (refine_resolve_call_sites, elab_toplevel.c).  Deferral is what makes
     * the check independent of definition order: at the moment `(safe-div 10 0)`
     * is elaborated, `safe-div`'s parameter predicates may not be stamped yet
     * (the binding is a pass-1 forward declaration), but by the end of the unit
     * they always are -- and it is the SAME Binding object, because elab_defn
     * reuses the forward declaration rather than replacing it. */
    struct RefineCallSite *refine_call_sites;
    uint32_t               n_refine_call_sites;
    uint32_t               cap_refine_call_sites;
    /* WF2: `#writes`-annotated functions awaiting the deferred frame walk.  See
     * WriteFrameSite below for why this is deferred rather than checked inline. */
    struct WriteFrameSite *wf_frame_sites;
    uint32_t               n_wf_frame_sites;
    uint32_t               cap_wf_frame_sites;
    /* AI3.1 (application-image-dumps-plan): the `init` root of every
     * with-image-cache-after-init expansion, noted at macro expansion and
     * checked by wf_lint_image_globals after every defn's frame site exists
     * (init's callees may be defined later in the unit). */
    struct ImageCacheRoot *image_cache_roots;
    uint32_t               n_image_cache_roots;
    uint32_t               cap_image_cache_roots;
    /* R4 slice 2 (trusted-refinement-claims-plan): every `#reads`-annotated
     * function, recorded during elaboration for the deferred read-frame
     * verification pass (rf_resolve_read_frames).  Same deferral rationale
     * as WriteFrameSite: a callee's own frame verdict may not exist yet at
     * this defn's elaboration.  Registration is unconditional and one
     * pointer-triple cheap, and since R4 slice 3 the pass is unconditional
     * too. */
    struct RfReadsSite    *rf_reads_sites;
    uint32_t               n_rf_reads_sites;
    uint32_t               cap_rf_reads_sites;
    /* reflected-measures RF0: every `^reflect` defn, recorded during
     * elaboration (gate on) for the deferred totality pass
     * (rf_resolve_reflect_sites, elab_reflect.c).  Same deferral rationale as
     * WriteFrameSite: purity of a callee defined later in the unit is part of
     * the verdict, and the verdict must not depend on definition order. */
    struct ReflectSite    *reflect_sites;
    uint32_t               n_reflect_sites;
    uint32_t               cap_reflect_sites;
    /* Open-addressed (callee, call_form) -> index+1 set, so deduplicating a
     * re-elaborated call site stays O(1) instead of rescanning every crossing
     * recorded so far -- which would make an opted-in build quadratic in its
     * call count. */
    uint32_t              *refine_cs_htab;
    uint32_t               refine_cs_htab_cap;
    /* refine: macro-call form -> the expansion elaboration actually walked
     * (parallel arrays, arena-allocated).  The crossing path walk uses this to
     * traverse INTO an expansion, so a crossing or guard GENERATED by a macro
     * template -- absent from the source caller body -- is still reachable
     * from the caller-body walk.  A `~@body`-spliced user form never needed
     * this (it sits in the source as a macro-call argument, found by the
     * generic walk); a template-constructed `(get! ~w ~e)` exists only
     * post-expansion.  Recorded only under the `refined` experiment. */
    const struct Form    **refine_mexp_calls;
    const struct Form    **refine_mexp_bodies;
    uint32_t               n_refine_mexps;
    uint32_t               cap_refine_mexps;
    /* loop-invariants-plan: every `while` that carried a written `:invariant`
     * under the gate, recorded by elab_while and decided by the enclosing
     * defn once its whole body is elaborated (li_analyze_loops, elab_fns.c) --
     * the same deferral the return obligations use, for the same reason: the
     * facts that reach the loop live in the defn's Form tree, not in the
     * elaborator's scope at the moment the loop is built. */
    struct LoopInvSite    *loop_inv_sites;
    uint32_t               n_loop_inv_sites;
    uint32_t               cap_loop_inv_sites;
    /* The result checks of the function whose body is being elaborated, which
     * elab_return applies to each `(return v)` -- an early return leaves
     * before the whole-body wrap (rt_wrap_return_check) ever sees a value.
     * NULL when there are none, when contracts are not emitted, and inside any
     * nested function body (a lambda's `return` is the lambda's). */
    const struct RetContract *ret_contract;
} Elab;

/* See Elab.ret_contract.  Each predicate is NULL when absent; they are applied
 * in this order, the same order the whole-body wrap nests them. */
typedef struct RetContract {
    const struct Form *post;           /* `:post`, bound as `result` */
    const struct Form *ret;            /* a refined return type */
    const char        *ret_var;
    const struct Form *class_ret;      /* an instance method: its class's promise */
    const char        *class_ret_var;
} RetContract;

/* Wrap a returned value in the enclosing function's result checks (see
 * Elab.ret_contract); `value` unchanged when there are none. */
struct Expr *rt_check_returned_value(Elab *e, struct Expr *value, Span span);

/* loop-invariants-plan: one `(while c :invariant p body...)`.
 *
 * Everything the static side needs is Form-level (the refinement machinery
 * encodes Forms), so the record keeps the source shape plus two things only
 * elaboration can know: the SORT of every local in scope at the loop (a name
 * the refinement env does not declare defaults to Int, which for a float local
 * would let S2's integer tightening prove a falsehood), and the slots holding
 * the two injected runtime checks, which a proof overwrites with nil. */
typedef struct LoopInvSite {
    const struct Form  *while_form;   /* the whole `(while ...)` call form */
    const struct Form  *cond;         /* c */
    const struct Form  *inv;          /* p */
    uint32_t            body_start;   /* index of the first body form */
    struct Expr       **entry_check;  /* slot of the entry check, or NULL */
    struct Expr       **body_check;   /* slot of the re-establishment check, or NULL */
    Span                span;
    /* In-scope locals at the loop, innermost binding first, one per name. */
    const char        **local_names;
    VCSort             *local_sorts;
    uint32_t            n_locals;
    /* An elaboration-time decline (the invariant reads a mutable global...),
     * or NULL.  Decided where the bindings are still resolvable. */
    const char         *decline;
    /* The ELABORATED invariant predicate, back-filled by elab_while beside the
     * two check slots.  It exists so the elision veto can ask the one
     * contract-position gate (rt_pred_observably_impure) rather than the plain
     * purity walk: that gate's `#reads` carve-out is keyed on resolved callee
     * info, which the Form in `inv` cannot answer.  NULL when contracts are
     * not emitted (there is then no check to elide). */
    const struct Expr  *pred_e;
    /* Filled by li_analyze_loops. */
    bool                analyzed;
    bool                entry_proven;
    bool                pres_proven;
    /* The body can leave through `return`.  Initiation and preservation do
     * not depend on how the loop exits, so they are still decided (the paths
     * through a `return` are pruned); only the post-loop fact `p AND (not c)`
     * is withheld from what follows the loop. */
    bool                early_return;
    const char        **assigned;     /* names the loop assigns (valid when proven) */
    uint32_t            n_assigned;
} LoopInvSite;

/* elab_while's two calls into the refinement side (elab_fns.c).
 *
 * elab_loop_invariant_pred elaborates `pred` in the current scope and checks it
 * is a pure `bool`, returning NULL (diagnosed) when it is not.
 * li_contract_check builds `(tur-contract-check pred msg)`, or NULL when the
 * checker is not bound.  li_register_site records the loop for the defn-level
 * pass and snapshots the in-scope local sorts. */
struct Expr *elab_loop_invariant_pred(Elab *e, const struct Form *pred, Span span);
struct Expr *li_contract_check(Elab *e, struct Expr *pred_e, const char *msg, Span span);
LoopInvSite *li_register_site(Elab *e, const struct Form *call, const struct Form *cond,
                              const struct Form *inv, uint32_t body_start, Span span);

/* loop-invariants-plan LI2: decide every loop site recorded since `from`
 * (initiation + preservation), eliding the runtime checks a proof covers.
 * Called by elab_defn once its body is elaborated, with the same inputs as the
 * return-obligation environment, BEFORE the return obligations -- a proved
 * loop's post-fact is what lets them see past it (LI3). */
void li_analyze_loops(Elab *e, uint32_t from, Binding **params, uint32_t n_params,
                      const struct Form **ct_param_preds, const char **ct_param_varnames,
                      const uint32_t *ct_param_param_idx, uint32_t n_ct_param_preds,
                      const struct Form *ct_pre_form, const struct Form *body,
                      const char *fn_name);

/* The same for a `definstance` method: `mb` is the method's binding (its
 * refinement arrays are the entry facts), `impl_form` the method's form, and
 * `body_start` the index of its first body form. */
void li_analyze_method_loops(Elab *e, uint32_t from, Binding **params,
                             uint32_t n_params, const Binding *mb,
                             const struct Form *impl_form, uint32_t body_start);

/* After the whole unit: decline (TUR-W0372) every loop site no definition
 * analysed -- a top-level lambda's -- so none is silently unverified. */
void li_decline_unanalyzed(Elab *e);

/* CT0: a contract type in ANNOTATION position contributes its BASE type to the
 * signature; the predicate rides separately, as an entry check and (under
 * `refined`) as a hypothesis.  EVERY site that resolves a parameter or return
 * annotation must peel it -- leaving a TY_CONTRACT in a signature makes every
 * use of that value fail to type with `expected { _ : ? | ... }`.
 *
 * That defect reached three sites independently before this helper existed:
 * `defn` return types, `fn` parameters, and typeclass instance-method
 * parameters.  If you are adding a fourth annotation site, call this.
 *
 * `*out_pred` / `*out_var` receive the predicate and its bound variable when
 * one was peeled; both are left untouched otherwise. */
static inline Type *rt_peel_contract(Type *ann, const Form **out_pred,
                                     const char **out_var) {
    if (ann && ann->kind == TY_CONTRACT && ann->as.contract_.base_type) {
        if (out_pred) *out_pred = ann->as.contract_.predicate;
        if (out_var)  *out_var  = ann->as.contract_.var_name;
        return ann->as.contract_.base_type;
    }
    return ann;
}

/* Peel a contract in TYPE-ARGUMENT position (`(Box #refine{...})`) to its base
 * type, warning TUR-W0380 that the payload predicate is not enforced.  Called
 * from BOTH type-application loops -- type_expr_from_form's and
 * fn_type_from_form_impl's; see the definition in elab_types.c for why there
 * are two and why this warns rather than dropping the predicate silently. */
Type *rt_peel_type_arg_contract(Type *arg_type, Span at);

/* CT1: wrap a function body so its RESULT is checked against `pred`.  Shared by
 * `defn` and typeclass instance methods; see elab_fns.c for why it is not
 * hand-rolled per site. */
Expr *rt_wrap_return_check(Elab *e, Expr *body, Binding *check_fn,
                           const Form *pred, const char *var_name,
                           const char *fail_msg, Span span);

/* CT1: inject an entry check for each `{ v : T | pred }` parameter.  Shared by
 * `defn`, `fn`, and typeclass instance methods so a contract parameter is
 * enforced identically wherever it is written.  Must be called while the
 * function's own scope is current -- the predicate is elaborated in it. */
Expr *rt_inject_param_checks(Elab *e, Expr *body, Binding *check_fn,
                             Binding **params, uint32_t n_params,
                             const Form **ct_preds, const char **ct_vars,
                             const uint32_t *ct_idx, uint32_t n_ct, Span span);

/* True when contract checks are being emitted for this build. */
bool rt_contracts_emitted(void);

/* RT1 helpers shared by the annotation sites (defn / fn / typeclass methods). */
VCSort rt_sort_of_kind(TypeKind k);
bool rt_resolve_fn(void *ud, const char *name, RefineFnInfo *out);
RefineFnResolver rt_refine_resolver(Elab *e);

/* One pending call-site crossing: `call_form`'s arguments cross into
 * `callee`'s parameters.  Nothing is looked up here -- see the field comment
 * on Elab.refine_call_sites for why. */
typedef struct RefineCallSite {
    const Binding *callee;
    /* The callee's name AS WRITTEN at this site.  A typeclass method's binding
     * carries its mangled C symbol (`__inst_Scaler_scale_hyby_int`), which is
     * not something to put in a diagnostic; the source form's head is. */
    const char    *callee_display;
    const Form    *call_form;    /* the whole `(f a b)` form */
    uint32_t       arg_offset;   /* index of the first argument in call_form */
    RefineEnv     *env;          /* the caller's hypotheses (may be NULL) */
    const char    *caller_name;
    /* The caller's whole body, back-filled alongside `env`.  The crossing
     * needs it to recover its own PATH CONDITIONS: `call_form` is a pointer
     * into this tree, so walking down to it collects every branch that had to
     * be taken to reach the call.
     *
     * WHOLE is load-bearing: a multi-form body arrives as a synthetic
     * `(do ...)` (see rt_whole_body).  This used to be the defn's LAST body
     * form, which is the return obligation's subject and not the same thing --
     * a call in any earlier form was then never found by the walk and lost
     * every condition guarding it. */
    const Form    *caller_body;
    /* RT1: when this crossing is a STATICALLY-resolved typeclass dispatch whose
     * instance demands LESS than its class, the class's own parameter
     * predicates.  The obligation is still the instance's -- the resolved
     * instance is the more precise contract, and the argument is genuinely
     * acceptable to it -- but an argument the CLASS rejects is relying on that
     * instance's private leniency, which is not part of the interface and
     * evaporates the moment dispatch goes dynamic or a stricter instance
     * appears.  That is TUR-W0377.  NULL for every other crossing, and for a
     * dispatch whose instance restates its class predicate (nothing to
     * disagree about). */
    const Form   **class_param_preds;
    const char   **class_param_vars;
    uint32_t       n_class_params;
    /* C2 / #reads: the names of the bindings that are BORROWED (frozen) in
     * scope at this crossing -- captured from the borrow checker's scope at
     * crossing-creation time, when it authoritatively knows what is live.  A
     * `#reads w` measure is congruent here exactly when its world argument is
     * one of these.  NULL / 0 when nothing is frozen. */
    const char   **frozen_names;
    uint32_t       n_frozen;
    Span           loc;
} RefineCallSite;

/* Record a crossing (deduplicated on (callee, call_form)).  No-op unless the
 * `refined` experiment is on.  Returns the index it was stored at (or the
 * current count when deduplicated), so elab_defn can back-fill the caller's
 * hypotheses over the range its body produced. */
uint32_t refine_note_call_site(Elab *e, const Binding *callee,
                               const Form *call_form, uint32_t arg_offset);

/* WF2 (checked-write-frames-plan): one `#writes`-annotated function, recorded
 * during elaboration and verified AFTER it (wf_resolve_write_frames).  The
 * deferral is load-bearing for the same reason it is for refinement crossings:
 * "every callee's declared frame stays inside this one" is a question about
 * functions that may be defined later in the file, and a check that answered it
 * differently depending on definition order would be worthless. */
/* AI3.1: one with-image-cache-after-init expansion's cold-start root. */
typedef struct ImageCacheRoot {
    const Symbol *root;   /* the `init` argument (a top-level defn name) */
    Span          span;   /* the macro call, where TUR-W0706 is reported */
} ImageCacheRoot;

typedef struct WriteFrameSite {
    Binding      *fn;          /* the annotated function; where the verdict lands */
    Binding     **params;      /* its parameters, for arg -> frame-slot mapping */
    uint32_t      n_params;
    const Form   *defn_form;   /* the whole `(defn ...)`; the body is a suffix */
    uint32_t      body_start;  /* index of the first body form within defn_form */
    const Form   *annot;       /* the `#writes` form, for the diagnostic span */
} WriteFrameSite;

/* Record an annotated function for the deferred WF2 walk. */
void wf_note_frame_site(Elab *e, Binding *fn, Binding **params, uint32_t n_params,
                        const Form *defn_form, uint32_t body_start,
                        const Form *annot);

/* Verify every recorded frame against its body, stamping `writes_checked` on
 * the ones that hold and emitting TUR-E0382 on the ones that do not. */
void wf_resolve_write_frames(Elab *e);
/* AI3.1: record a with-image-cache-after-init `init` root / run TUR-W0706. */
void wf_note_image_cache_root(Elab *e, const Symbol *root, Span span);
void wf_lint_image_globals(Elab *e);

/* R4 slice 2 (trusted-refinement-claims-plan): one `#reads`-annotated
 * function, recorded during elaboration and verified AFTER it
 * (rf_resolve_read_frames).  Unlike WriteFrameSite this walk runs over the
 * ELABORATED body (Binding.source_fn_def->body) because its read leaves need
 * type information (receiver type kinds, builtin shapes); the params array is
 * arena-lived, same as WriteFrameSite's. */
typedef struct RfReadsSite {
    Binding  *fn;         /* the `#reads`-annotated function */
    Binding **params;     /* its parameters, for root -> frame-bit mapping */
    uint32_t  n_params;
    Span      span;       /* the `#reads` annotation (or name), for W0383 */
    /* A monomorphization clone: verified and evidence-stamped like its
     * original (the encoder's refusal must see the evidence whichever
     * binding a call resolves to, same rule as the defn-site stamp), but
     * silent -- no dump line, no repeated warning. */
    bool      is_clone;
} RfReadsSite;

/* reflected-measures RF0/RF1/RF2 (docs/upcoming/reflected-measures-plan.md):
 * one `^reflect`-annotated function.  The body is kept as FORMS (the
 * refinement encoder consumes Forms, and Binding::defn_form is only retained
 * on two unrelated paths), the same shape WriteFrameSite carries. */
typedef struct ReflectSite {
    Binding      *fn;          /* the annotated function; where the verdict lands */
    Binding     **params;      /* its parameters, in declaration order */
    uint32_t      n_params;
    const Form   *defn_form;   /* the whole `(defn ...)`; the body is a suffix */
    uint32_t      body_start;  /* index of the first body form within defn_form */
    const Form   *annot;       /* the `^reflect` symbol, for the diagnostic span */
} ReflectSite;

/* Record a `^reflect` function for the deferred totality pass. */
void rf_note_reflect_site(Elab *e, Binding *fn, Binding **params, uint32_t n_params,
                          const Form *defn_form, uint32_t body_start,
                          const Form *annot);
/* Classify every recorded site (purity + structural termination + coverage),
 * stamping Binding::reflect_total and emitting TUR-E0384 on the ones that
 * fail.  Runs after elaboration, before crossings are resolved. */
void rf_resolve_reflect_sites(Elab *e);
/* Classify ONE site without diagnostics, stamping the verdict only when it is
 * TOTAL.  Called at the end of elab_defn so an obligation discharged IN PLACE
 * (a return refinement decided while its defn is elaborated) can already
 * unfold the measure; the deferred pass is authoritative for rejections. */
void rf_stamp_reflect_site_eager(Elab *e, Binding *fn);
/* The purity walk's verdict, exported for the totality gate (elab_fns.c). */
bool rt_binding_is_pure(Binding *b);

/* Record a `#reads`-annotated function for the deferred verification pass. */
void rf_note_reads_site(Elab *e, Binding *fn, Binding **params,
                        uint32_t n_params, Span span, bool is_clone);

/* Verify every recorded `#reads` frame against its elaborated body, stamping
 * `reads_checked` on the ones where every read of mutable state attributes to
 * a frame-named parameter.  No diagnostic: EXCEEDED evidence is the slice-1
 * scan's job (TUR-W0383 and the congruence refusal it carries); this pass only
 * decides whether SILENCE was "saw everything, all clean" (VERIFIED) or "could
 * not see" (UNVERIFIED).  Only the DUMP is behind --dump-read-frames. */
void rf_resolve_read_frames(Elab *e);

/* G4a: may this kind be loaded/stored atomically in one machine operation? */
bool type_is_atomic_scalar(TypeKind k);

/* Record that macro-call form `call` elaborated to `expansion`, so the
 * crossing path walk can traverse INTO the expansion (macro-GENERATED
 * crossings/guards are otherwise unreachable from the source caller body).
 * Deduplicated on `call`; a re-elaboration overwrites with the latest
 * expansion (whose nodes the newest crossings point into -- an older
 * crossing still matches by span, since every copy of a template node
 * carries the same template span).  No-op unless `refined` is on. */
void refine_note_macro_expansion(Elab *e, const Form *call,
                                 const Form *expansion);

/* Attach the CLASS signature's parameter predicates to an already-recorded
 * crossing, so a statically-resolved dispatch whose instance demands less can
 * be linted (TUR-W0377) without changing what it is obliged to prove. */
void refine_note_call_site_class_preds(Elab *e, const Binding *callee,
                                       const Form *call_form,
                                       const Form **preds, const char **vars,
                                       uint32_t n_params);

/* Attach `env` / `caller` to every crossing recorded at or after `from` that
 * does not already have one.  Called by elab_defn once its body is elaborated
 * and its hypothesis environment is built.  Back-filling rather than keeping a
 * mutable "current env" on Elab is deliberate: elab_defn has many early-error
 * returns, and a stale environment left behind by one of them would hand a
 * later crossing hypotheses that do not hold there -- which is exactly the
 * direction the soundness invariant forbids. */
void refine_fill_call_site_env(Elab *e, uint32_t from, RefineEnv *env,
                               const char *caller, const Form *body);

/* Resolve and discharge every recorded crossing.  Runs once, after all
 * elaboration, from elaborate_program. */
void refine_resolve_call_sites(Elab *e);

/* GF1: per-gen elaboration state (stack-allocated, linked by parent pointer) */
typedef struct GenContext {
    uint32_t          n_yields;         /* number of (yield ...) forms seen so far */
    TypeKind          element_kind;     /* TypeKind of the first yield (TY_UNKNOWN until set) */
    bool              element_kind_set; /* true once first yield is elaborated */
    const char       *element_tyvar;    /* first yield typed with a signature tyvar */
    struct GenContext *parent;          /* enclosing GenContext (NULL for outermost) */
    /* CF5: true when the enclosing function calls itself inside this gen body */
    bool              is_recursive;
    /* Collect let-bindings inside gen body for struct field promotion */
    Binding         **let_bindings;     /* arena-allocated array of Binding* */
    uint32_t          n_let_bindings;
    uint32_t          cap_let_bindings;
} GenContext;

/* Phase 6: Macro definition */
/* Stage 2 (macro-system-direction-plan): how a macro's body runs. */
typedef enum MacroKind {
    MACRO_TEMPLATE = 0,   /* defmacro: substitute_params + the CT evaluator */
    MACRO_PROCEDURAL,     /* defmacro*: full-Turmeric body evaluated in the
                           * macro-time turi env (src/turi/macro_env.c) */
} MacroKind;

typedef struct MacroDef {
    const Symbol *name;
    Form **params;
    uint32_t n_params;       /* number of fixed params (excludes rest param) */
    bool is_variadic;        /* true if [params & rest] syntax used */
    const Symbol *rest_param; /* rest-arg symbol name, or NULL */
    /* docs/archive/history/macro-args-elaborated-before-expansion.md:
     * per-param ^syntax marker.  When is_syntax_param[i] is true, the
     * corresponding arg form is passed to the macro as data: substitute_params
     * leaves the param symbol in place and elab_eval_macro_form binds the
     * symbol to the raw arg Form in the CT env, so CT builtins like
     * (first decl) / (symbol-name (first decl)) walk the AST instead of
     * trying to evaluate it as code. NULL when no params are marked. */
    bool *is_syntax_param;
    bool rest_is_syntax;     /* ^syntax marker on the rest param */
    Form *body;
    Span span;
    /* Phase M4: module that defined this macro (NULL = stdlib/pre-module) */
    const Symbol *defining_module_name;
    /* Phase M4: true when injected via :refer — visible in any module context
     * but defining_module_name still holds the original module so private
     * helpers of that module remain accessible during expansion. */
    bool is_referred;
    /* Multi-form bodies: the body was synthesized as (do setup... template),
     * which only has meaning under the compile-time evaluator -- route the
     * substituted template through it unconditionally. */
    bool force_ct_eval;
    /* Stage 2: MACRO_TEMPLATE (default, memset-0) or MACRO_PROCEDURAL. */
    MacroKind kind;
    /* Stage 2: for MACRO_PROCEDURAL, the name the macro's compiled-to-defn
     * closure is bound under in the macro-time env ("<name>__mx"). */
    const char *proc_fn_name;
} MacroDef;

typedef enum CtValueTag {
    CT_VAL_FORM = 0,
    CT_VAL_FN,
} CtValueTag;

typedef struct CtEnv CtEnv;

typedef struct CtFn CtFn;

typedef struct CtValue {
    CtValueTag tag;
    union {
        Form *form;
        CtFn *fn;
    } as;
} CtValue;

typedef struct CtBinding {
    const Symbol *name;
    CtValue value;
    struct CtBinding *next;
} CtBinding;

struct CtEnv {
    CtEnv *parent;
    CtBinding *bindings;
    Elab *elab;
    bool *ok;
    /* True only when this env directly evaluates the inner of a `~@` splice.
     * In that position a list whose head names a user macro is expanded at
     * compile time (so a macro can recurse over a list to GENERATE the spliced
     * sequence, e.g. `~@(chain (rest xs))`). Elsewhere a macro-headed call is
     * left as data for ordinary elaboration -- expanding it during CT eval
     * would mis-fire on data forms threaded through builtins like `list`
     * (e.g. an accumulated `(map-assoc ...)`).  Not inherited by child envs.
     * docs/archive/history/ct-macro-evaluator-no-function-call-in-splice.md */
    bool expand_macro_head;
};

struct CtFn {
    Form **params;
    uint32_t n_params;
    bool is_variadic;
    const Symbol *rest_param;
    Form *body;
    CtEnv *env;
    Span span;
};

/* ---- elaborator function prototypes ---- */

/* elab_core.c */
Type type_from_kind(TypeKind k);
TypeKind typekind_from_symbol(const char *name);
uint32_t fwd_decl_scan_params(Arena *arena, const Form *params_f, TypeKind **out_arg_kinds);
bool fwd_decl_scan_variadic(const Form *params_f, TypeKind *rest_kind);
void fwd_decl_apply_variadic(Elab *e, Arena *arena, Type *fn_type, const Form *params_f);
bool typekind_is_numeric(TypeKind k);
int type_size_bytes(TypeKind kind);
bool typekind_is_concrete_for_disjoint(TypeKind k);
bool types_provably_disjoint(Type *a, Type *b);
void scope_init(Scope *s, Scope *parent);
void scope_free(Scope *s);
bool scope_borrow_conflicts(const Scope *s, Binding *binding, BorrowKind kind);
bool scope_add_borrow(Scope *s, Binding *binding, BorrowKind kind, Span span);
void scope_add(Scope *s, Binding *b);
Binding *scope_lookup(Scope *s, const Symbol *name);
Binding **collect_free_vars(const Expr *e, Binding **params, uint8_t n_params,
    Binding **self_exclude, uint32_t n_self_exclude, uint32_t *n_out);
void elab_register_file_def(Elab *e, Expr *def_expr);
/* used-attr-whole-program: force-load a module by name so its defns are
 * emitted on the single-file/whole-program path even with no `(import)`. */
void elab_force_load_module(Elab *e, const char *module_name);
int elab_expand_module_loads(Elab *e, Arena *arena, SymbolTable *st,
                             Form *const *forms, uint32_t nforms,
                             Form ***out_forms, uint32_t *out_n);
/* Pass-1 forward declaration of a single top-level (defn ...) form into
 * e->global.  Shared by elaborate_program (entry unit) and import_module
 * (imported/loaded modules) so bare top-level defns spliced by (load ...) can
 * self/mutually recurse.  A non-defn form is a no-op. */
void elab_pre_declare_toplevel_defn(Elab *e, Arena *arena, Form *f);
void elab_pre_declare_any_mut_def(Elab *e, const Form *f);
/* forward-call-to-generic-callee-typed-as-placeholder: Pass 2 elaborates a
 * defn that calls a not-yet-elaborated GENERIC defn of the same statement
 * list after that callee, since the generic's pass-1 forward decl has no
 * parameter types to instantiate its type variables from (likewise a callee
 * with a function-typed parameter, whose forward decl does not mark it as
 * the `:fn` carrier).  A defn deferred
 * this way holds back the defns that name it in turn, and any other form
 * that names it elaborates it first (fwd_gen_order_next_flush).  Shared by
 * the top-level driver and the defmodule body loop; see elab_toplevel.c. */
typedef struct FwdGenOrder {
    const Symbol **name;     /* per slot: the tracked defn's name, or NULL */
    uint8_t       *state;    /* per slot: FGO_* */
    bool          *primed;   /* per slot: tried speculatively in a cycle */
    const Symbol **keys;     /* open-addressed: every defn name in the list */
    uint32_t      *counts;   /* per key: slots under the name still waiting */
    uint32_t      *slot_of;  /* per key: the tracked slot, or UINT32_MAX */
    uint32_t       cap;      /* power of two; 0 = no generic defn, inert */
    uint32_t       n;
    bool           any_deferred;
} FwdGenOrder;
void fwd_gen_order_init(FwdGenOrder *o, const Elab *e, Form *const *forms,
                        uint32_t n);
bool fwd_gen_order_should_defer(const FwdGenOrder *o, Form *const *forms,
                                uint32_t i);
void fwd_gen_order_defer(FwdGenOrder *o, uint32_t i);
/* A waiting defn `f` names, which must be elaborated before `f` is (mark it
 * done first), or UINT32_MAX. */
uint32_t fwd_gen_order_next_flush(const FwdGenOrder *o, const Form *f);
void fwd_gen_order_done(FwdGenOrder *o, uint32_t i);
/* The second chance: calls `retry(ctx, i)` once for every slot deferred by
 * the order or flagged in `extra` (the symptom-A instance deferral), clearing
 * `extra[i]`.  With nothing deferred by the order this is the old single
 * source-order pass over `extra`.  `probe(ctx, i)` elaborates a slot
 * speculatively -- true when it succeeded and was kept, false when it was
 * rolled back -- and is how a cycle of lossy defns is primed (see the
 * definition). */
void fwd_gen_order_drain(FwdGenOrder *o, Form *const *forms, bool *extra,
                         void (*retry)(void *ctx, uint32_t i),
                         bool (*probe)(void *ctx, uint32_t i), void *ctx);
void fwd_gen_order_free(FwdGenOrder *o);
/* r7rs-lang-plan R3: full types for a forward decl's compound parameters in a
 * dynamic file (NULL when none); marks the matching arg_kinds slots TY_APP. */
Type **elab_fwd_param_full_types(Elab *e, Arena *arena, const Form *f,
                                 uint32_t name_idx, uint32_t params_idx,
                                 uint32_t param_arity, TypeKind *arg_kinds);
/* The full type of a named return annotation -- a bare registered ADT or a
 * closed application -- for the defmodule pre-pass (NULL when a leaf cannot
 * be named yet).  See elab_toplevel.c. */
Type *elab_fwd_compound_result_type(Elab *e, const Form *f, uint32_t name_idx,
                                    uint32_t params_idx, const Form *ret_f);
/* forward-call-to-aggregate-result-types-as-carrier: record a defmodule
 * forward decl whose named return names a type not registered yet, and retry
 * every recorded one (called at the start of each defn). */
void elab_fwd_note_pending_result(Elab *e, Binding *b, const Form *f,
                                  uint32_t name_idx, uint32_t params_idx,
                                  const Form *ret_f);
void elab_fwd_refresh_pending(Elab *e);
const Symbol *intern_cstr(SymbolTable *st, const char *s);
bool binding_mark_moved(Binding *b, Span use_span);
bool binding_mark_lent(Binding *b, Span use_span);
/* elab_call.c: shim a thin fn value to the fat representation (see there). */
Expr *elab_fn_value_to_fat(Elab *e, Expr *value);
/* byvalue-recursive-adt-boxes-are-never-freed: the by-value recursive (or
 * `any`-fielded) ADT def behind `t` whose spine a local owns and frees at
 * scope exit, or NULL.  elab_forms.c; shared with the parameter-mask
 * inference in elab_fns.c and the lend decision in elab_call.c. */
const AdtDef *elab_byval_localowned_adt(Type t);
bool binding_check_not_moved(Binding *b, Span use_span, const char *use_desc);
uint32_t move_state_snapshot_bindings(const Scope *scope,
    Binding ***out_bindings,
    bool **out_states);
bool *move_state_capture_current(Binding **bindings, uint32_t n);
void move_state_restore(Binding **bindings, const bool *states, uint32_t n);
uint32_t linear_state_snapshot_bindings(const Scope *scope,
    Binding ***out_bindings,
    bool **out_states);
bool *linear_state_capture_current(Binding **bindings, uint32_t n);
void linear_state_restore(Binding **bindings, const bool *states, uint32_t n);
bool is_binding_consumed(const Expr *body, Binding *binding);
/* set-bang-rc-release: stamp every `(set! binding v)` in `body` so codegen
 * releases the value being overwritten, normalizing borrow-shaped `v` to a
 * genuine +1 first.  Call ONLY for a binding that owns a continuous reference
 * (i.e. one that also qualifies for the scope-exit rc auto-drop). */
void elab_set_rc_release(Arena *arena, Expr *body, Binding *binding);
bool is_field_consumed(const Expr *body, Binding *binding, uint32_t field_idx);
bool is_field_consumed_in_handler(const Expr *body, Binding *binding, uint32_t field_idx);
Binding *expr_closure_fn_binding(const Expr *expr);
bool expr_closure_return_dispatches(const Expr *expr);
bool expr_closure_return_dispatches_untyped(const Expr *expr);
void elab_init_state(Elab *e, Arena *arena, SymbolTable *st);
MacroDef *elab_lookup_macro(Elab *e, const Symbol *name);

/* PS4: did this definition come from an earlier turn of the session this
 * elaborate call continues?  Always false outside a session (see the
 * turn_start_* watermarks on Elab). */
bool elab_prior_turn_global(const Elab *e, const Binding *b);
bool elab_prior_turn_macro(const Elab *e, const struct MacroDef *m);
bool elab_prior_turn_adt(const Elab *e, const AdtDef *ad);
bool elab_prior_turn_effect(const Elab *e, const Symbol *name);
bool elab_prior_turn_instance(const Elab *e, const struct TypeClassInstance *inst);
/* True when `file_id` names a stdlib source file (its path runs through a
 * `stdlib/` directory) -- the same test the duplicate-instance guard uses. */
bool elab_file_is_stdlib(uint16_t file_id);
/* PS4: drop an earlier turn's macro so a redefinition can register afresh. */
void elab_remove_macro(Elab *e, struct MacroDef *m);
Binding *binding_new(Elab *e, const Symbol *name, Type type,
    bool is_mut, bool is_global, Span span);
int elab_read_file(const char *path, char **out, size_t *out_len);
ElabModule *elab_find_loaded_module(Elab *e, const Symbol *name);
bool effect_row_contains_symbol(const EffectRow *row, const Symbol *name);
Expr *e_nil(Elab *e, Span span);
char *elab_mangle_binding_name(const Binding *b);

/* elab_macros.c */
void elab_register_macro(Elab *e, MacroDef *macro);
Form *quasiquote_expand_form(Elab *e, Form *f);
Form *elab_expand_macro(Elab *e, MacroDef *macro, Form **args, uint32_t n_args);
Expr *elab_defmacro(Elab *e, const Form *call);
Expr *elab_defmacro_star(Elab *e, const Form *call);
Expr *elab_gensym(Elab *e, const Form *call);

/* Stage 2 (macro-system-direction-plan): procedural-macro bridge, implemented
 * in src/turi/macro_env.c (include direction stays turi -> compiler; the
 * compiler sees only these declarations).
 *
 * elab_macro_env_define_proc evaluates the synthesized
 * `(defn <fn_name> [params : Syntax ...] : Syntax body...)` form in the
 * session's macro-time env; emits a diagnostic at err_span and returns false
 * on any definition-time failure.
 *
 * elab_macro_env_call_proc invokes a MACRO_PROCEDURAL macro's closure on the
 * raw call-site argument forms and returns the expansion imported into the
 * compiler's arena/symtab (forms built macro-side intern symbols into the
 * macro env's own table, so a deep re-interning copy is mandatory -- symbol
 * identity drives every special-form dispatch).  NULL after emitting a
 * diagnostic on arity mismatch, expansion-time error, or a non-syntax
 * result. */
bool  elab_macro_env_define_proc(Elab *e, const char *fn_name,
                                 const Form *defn_form, Span err_span);
Form *elab_macro_env_call_proc(Elab *e, MacroDef *macro,
                               Form **args, uint32_t n_args, Span call_span);

/* Stage 3: `(import m :for-macros)` -- evaluate module m into the macro-time
 * env (TURI_CAP_IMPORT granted transiently for the load) so defmacro* bodies
 * can call its functions at expansion time.  `path` is the resolved module
 * file.  Emits a diagnostic at `span` and returns false on failure.
 * Implemented in src/turi/macro_env.c. */
bool elab_macro_env_import(Elab *e, const Symbol *module_name,
                           const char *path, Span span);

/* Stage 3: resolve a module name to its file path using the same search
 * order elab_load_module uses (importing file's dir -> stdlib dir -> each
 * -I include dir), probing for existence only -- no read, no registry
 * side effects.  Returns false when no candidate exists.  elab_module.c. */
bool elab_module_resolve_path(Elab *e, const Symbol *name,
                              char *out, size_t cap);
/* r7rs-define-library-cannot-export-syntax: elab_module_resolve_path in the
 * shape scheme_lower_program takes (`ud` is the Elab), so an importer's
 * Scheme lowering can read a library's macros from its source. */
bool elab_scheme_library_path(void *ud, const char *module, char *out, size_t cap);
SchemeGlobalKind elab_scheme_global_kind(void *ud, const char *name);
bool elab_scheme_stdlib_file(void *ud, const char *name, char *out, size_t cap);

/* TY2.2: wrap a value in EX_UNION_INJECT to widen it to the `any` top type. */
Expr *elab_coerce_to_any(Elab *e, Expr *value);
/* The `any`-bridge adaptor for a function value whose signature differs from
 * the slot's only in where `any` appears (elab_call.c); NULL leaves it alone. */
Expr *elab_fn_any_bridge(Elab *e, Expr *arg, const Type *want_decl,
                         const AbiTypeBinding *binds, uint8_t n_binds);
bool elab_type_is_ground(const Type *t);
bool saffron_bare_tyvar_param_widens(const Type *fnt, uint32_t i);
Type elab_saffron_call_view(Elab *e, const Type *fnt);
/* saffron-effect-row-lost-through-unannotated-call: the RETURN-position widen.
 * Hoists an effectful call out of the widened body first; see its comment in
 * elab_call.c for why this is separate from elab_coerce_to_any (the argument
 * site's frame_box / stack_ok / any_drop_after stamps require the coercion to
 * return an EX_UNION_INJECT directly). */
Expr *elab_coerce_to_any_return(Elab *e, Expr *value);
/* byvalue-recursive-shared-copies-leak (elab_forms.c): a by-value product with
 * an `rc` field; is `x` a shared view (a copy whose owning fields someone else
 * holds); clone a shared view's rc fields where it becomes an owner. */
const AdtDef *elab_rc_product_adt(Type t);
bool elab_expr_is_shared_view(const Expr *x);
Expr *elab_clone_shared_rc_product(Elab *e, Expr *v, bool *had_shared);
Expr *elab_own_byval_copy(Elab *e, Expr *v, Binding *local);
/* cps-coloring-walk-has-no-arm-for-union-inject: hoist a control-bearing
 * operand into a `let` so the node above it sees a variable.  The CPS IR can
 * lower a control op in a let INIT but only delegates the nodes below, and a
 * delegated control op reaches the direct emitter, which aborts. */
Expr *elab_bind_control_temp(Elab *e, Expr *value, LetBinding *lb);
Expr *elab_hoist_control_operands(Elab *e, Expr *node);
/* union-tagged-union-c-emission: tag a member value flowing into a union slot.
 * The union twin of elab_coerce_to_any; see its comment in elab_call.c for why
 * it never sets frame_box. */
Expr *elab_coerce_to_union(Elab *e, Expr *value, const Type *union_t);

/* Gap 1 (instance-method-return-not-unified): report a genuine,
 * carrier-independent return-type conflict between a function's DECLARED return
 * and its body's actual type.  The int64 carrier ABI deliberately unifies the
 * representation of int / cstr / bool / opaque-handle / struct-handle, so a
 * width-compatible "mismatch" (return 42 where :cstr is declared) cannot be
 * soundly rejected without fighting the carrier -- those are the bridges the
 * ABI relies on.  What CAN be rejected is a clash of distinct NOMINAL
 * identities: the declared return is a concrete record ADT / opaque newtype
 * (carrying a def pointer) and the body yields a DIFFERENT concrete ADT.  Two
 * distinct nominal defs never share a carrier representation, so this is always
 * a real error.  ret_adt is the declared return's ADT def pointer; body is the
 * elaborated body's type.  Returns true on a conflict; a primitive / carrier /
 * tyvar / applied / unknown body is tolerated.  (structdef-retirement DS-D: the
 * former StructDef ret_struct parameter is gone -- every former struct is a
 * record ADT.) */
bool return_type_nominal_conflict(const AdtDef *ret_adt, Type body);

/* float-register-class-returns: sibling of return_type_nominal_conflict that
 * catches the one carrier-tolerated return mismatch that is NOT a benign
 * representation bridge -- a float-vs-non-float clash.  `int`, `cstr`, `bool`,
 * opaque handles, and struct/ADT handles all share the int64 GP register, so
 * swapping them in the result position is a no-op reinterpret; a float lives in
 * an xmm register, so returning a float where the declared return is a concrete
 * non-float (or vice versa) is a genuine register-class miscompile (xmm0 vs
 * rax).  Fires only when EXACTLY ONE side is a floating kind (TY_FLOAT*) and the
 * other is a concrete (register-pinned) non-float; tyvar / unknown / never / any
 * sides are tolerated (not yet a concrete cross-class result).  `declared` is
 * the function's declared return TypeKind; `body` is the elaborated body's type.
 * The int-literal -> float coercion is handled by the caller (it widens the
 * literal in place) before this check runs. */
bool return_type_register_class_conflict(TypeKind declared, Type body);

/* float-register-class-returns: if `declared` is a floating kind (TY_FLOAT*)
 * and `body` is a bare integer literal, widen the literal to that float in
 * place (mirroring numeric-literal coercion in argument / binding positions)
 * and return true.  The caller then treats the return as a coercion rather
 * than a register-class conflict.  Returns false (and leaves `body` untouched)
 * otherwise. */
bool rc_widen_int_literal_to_float_return(TypeKind declared, Expr *body);
/* float32-block-temp-widens-to-double: a float LITERAL closing a
 * multi-expression `: float32` / `: float64` body -- the tail of a `do`,
 * `let`, or both arms of an `if` -- keeps its default `double` type, so the
 * block's merge temp was declared `double` and narrowed on `return`
 * (-Wfloat-conversion under GCC; two roundings for a literal that is not
 * exactly representable).  A bare literal body is fine (it returns
 * directly, one conversion of a constant).  Retype the tail literal, and each
 * enclosing block node whose type was the default double, to the declared
 * width -- the same "the author wrote a constant AT that width" reasoning
 * the ascription arm applies to `(:: 7.1 float32)`.  Returns true when
 * anything was retyped. */
bool rc_narrow_float_literal_tail_to_declared_return(TypeKind declared, Expr *body);

/* pointer-vs-scalar-returns: the next carrier-tolerated slice past the nominal
 * and float guards.  `cstr` (a `const char*`) rides the same int64 GP register
 * as `int` / `bool`, so swapping them in the result position is a no-op
 * reinterpret the carrier ABI cannot see -- yet a bare integer is never a valid
 * string pointer in surface Turmeric.  Only the COMMIT direction is a sound
 * rejection: the declared return is concretely `cstr` and the body yields a
 * concrete integer-family scalar (int / bool / intN / uintN).  The reverse
 * (a declared integer carrier with a `cstr` body) is the deliberate
 * carrier-handle bridge -- generic / typeclass code legitimately returns a
 * pointer handle through an int64 result slot, exactly as it returns a struct
 * handle through `int` -- so it is left to a future carrier-aware unification,
 * like the same-register int-vs-bool case.  `declared` is the function's
 * declared return TypeKind; `body` is the elaborated body's type.  Returns true
 * only on the commit-direction conflict. */
bool return_type_pointer_scalar_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification Phase 2: the REVERSE pointer-scalar shape --
 * a concrete integer-family declared return with a concrete `cstr` body.  This
 * is the carrier-handle bridge the commit-direction helper tolerates, so it is
 * sound to reject ONLY for a committed position; the dispatcher applies that
 * gate.  Returns true on the shape match. */
bool return_type_pointer_scalar_reverse_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification Phase 2b: a `bool`-vs-non-bool-integer
 * mismatch -- exactly one of declared / body is `bool` and the other is a
 * concrete non-bool integer-family scalar.  `bool` and the integer family share
 * the int64 0/1 carrier, but the language treats them as distinct (binding
 * position already rejects the swap), so this is sound to reject ONLY for a
 * committed position; the dispatcher applies that gate.  Returns true on the
 * shape match. */
bool return_type_bool_integer_conflict(TypeKind declared, Type body);

/* carrier-aware-return-unification Phase 2c: exactly one side is an aggregate
 * that does NOT ride the int64 carrier -- a by-value record ADT (`tur_adt_S`),
 * or a :heap one (a typed pointer to it) -- and the other is a concrete,
 * register-pinned scalar.  Every tolerance above exists because both sides are
 * `int64_t` in the emitted C and the mismatch is a real bridge; here they are
 * different C types, so the program does not compile at all.  Membership is
 * decided by asking `type_c_name`, the function codegen itself uses, so a
 * transparent int newtype or a carrier-swallowed ADT-app is tolerated without
 * this predicate having to enumerate them.  Unlike the reverse-pointer-scalar
 * and bool-vs-integer checks this is NOT gated on the return class: those
 * tolerate a bridge between two things that are both `int64_t` in the emitted
 * C, and a by-value aggregate is not one of them, so no carrier class makes it
 * sound.  Only the bare, unparameterised record ADT counts -- a parametric
 * return has a crossing that grounds it. */
bool return_type_carrier_aggregate_conflict(Type declared, Type body);

/* committed-applied-return-vs-scalar: a ground by-value applied type (`(Option
 * float)`, `(Pair float int)`) against a concrete int-family / bool / cstr
 * return, either direction.  `declared_app` is the declared return when it is
 * an applied type (NULL otherwise; `ret_kind` then names the scalar).  Only a
 * committed (monomorphic, non-`#{Unsafe}`) defn has no crossing to ground the
 * applied side, so the caller gates it on RET_CLASS_COMMITTED. */
bool return_type_applied_scalar_conflict(const Type *declared_app,
                                         TypeKind ret_kind, Type body);
/* The same question for any position that states a type and receives a value
 * (a let annotation, a declared parameter): `want` a ground by-value applied
 * type and `got` a scalar or a DIFFERENT ground applied type, or `want` an
 * int-family / bool / cstr scalar and `got` a ground by-value applied type. */
bool applied_type_conflict(Type want, Type got);

/* carrier-aware-return-unification: classify a return position so the shared
 * dispatcher knows how much to reject against the int64 carrier ABI.
 *   RET_CLASS_COMMITTED -- a genuinely committed position: a monomorphic,
 *     non-`#{Unsafe}` `defn` that does not participate in the carrier.  Rejects
 *     every concrete ground mismatch: symmetric register-class, the reverse
 *     pointer-scalar direction (integer return, cstr body -> TUR-E0709), AND the
 *     bool-vs-integer mismatch (both directions -> TUR-E0709).
 *   RET_CLASS_CARRIER_FN -- a generic or `#{Unsafe}` `defn`.  Register-class
 *     stays symmetric (a float never rides the int64 carrier), but the reverse
 *     pointer-scalar direction is the deliberate carrier-handle bridge and is
 *     tolerated.
 *   RET_CLASS_CARRIER_METHOD -- a typeclass instance method.  Full carrier
 *     tolerance: register-class only in the float-COMMIT direction (a
 *     non-float-declared method with a float instance body is the per-instance
 *     bridge) and the reverse pointer-scalar direction tolerated.
 * Phase 3 will route grounded instance methods to RET_CLASS_COMMITTED. */
typedef enum {
    RET_CLASS_COMMITTED,
    RET_CLASS_CARRIER_FN,
    RET_CLASS_CARRIER_METHOD,
} ReturnClass;

/* Which return-position predicate fired (RET_CONFLICT_NONE = no conflict).  The
 * caller maps each to its site-specific diagnostic (function vs instance
 * method) and error code (NOMINAL -> TUR-E0001, REGISTER_CLASS -> TUR-E0707,
 * POINTER_SCALAR -> TUR-E0708, TYPE_REVERSE / BOOL_INTEGER -> TUR-E0709). */
typedef enum {
    RET_CONFLICT_NONE = 0,
    RET_CONFLICT_NOMINAL,
    RET_CONFLICT_REGISTER_CLASS,
    RET_CONFLICT_POINTER_SCALAR,
    RET_CONFLICT_TYPE_REVERSE,
    RET_CONFLICT_BOOL_INTEGER,
    RET_CONFLICT_CARRIER_AGGREGATE,
    RET_CONFLICT_NIL_BODY,
} ReturnConflict;

/* carrier-aware-return-unification: single dispatcher over the return-position
 * predicates above.  Runs nominal -> register-class -> pointer-scalar (commit)
 * -> pointer-scalar (reverse) and returns the first conflict; `cls` calibrates
 * the register-class and reverse-pointer-scalar axes (see ReturnClass).  Callers
 * widen an int-literal -> float body in place with
 * rc_widen_int_literal_to_float_return BEFORE calling, and should skip the
 * lazy-probe placeholder and inline-C (fiat TY_NIL) bodies as before. */
/* nil-tail-not-checked-against-declared-return: `checkable` is the caller's
 * decision and carries two facts -- the return type was written down, and the
 * tail is a nil LITERAL rather than merely nil-typed.  See elab_core.c. */
bool return_type_nil_body_conflict(TypeKind declared, Type body,
                                   bool checkable);
/* True when this body's TAIL is a nil literal, peeling the EX_DO / EX_LET /
 * EX_LETREC wrappers a multi-form or scope-opening body adds. */
bool body_tail_is_nil_literal(const Expr *e);
ReturnConflict return_position_conflict(const AdtDef *ret_adt,
                                        TypeKind ret_kind, Type body,
                                        ReturnClass cls, bool check_nil_body);

/* TY4: if `e` is a borrow (&x / &mut x) of a named binding, return that
 * binding (the referent); otherwise NULL.  Used by the borrow-escape check. */
const Binding *borrow_referent_binding(const Expr *e);

/* elab_unsafe.c */
Expr *elab_ptr_deref(Elab *e, const Form *call);
Expr *elab_ptr_write(Elab *e, const Form *call);
Expr *elab_ptr_add(Elab *e, const Form *call);
Expr *elab_ptr_sub(Elab *e, const Form *call);
Expr *elab_ptr_nullq(Elab *e, const Form *call);
Expr *elab_ptr_of(Elab *e, const Form *call);
Expr *elab_unsafe_cast(Elab *e, const Form *call);
Expr *elab_reinterpret(Elab *e, const Form *call);
Expr *elab_transmute(Elab *e, const Form *call);
Expr *elab_array_get_unchecked(Elab *e, const Form *call);
Expr *elab_array_set_unchecked(Elab *e, const Form *call);
Expr *elab_raw_malloc(Elab *e, const Form *call);
Expr *elab_raw_free(Elab *e, const Form *call);
Expr *elab_raw_realloc(Elab *e, const Form *call);
Expr *elab_raw_memcpy(Elab *e, const Form *call);
Expr *elab_raw_memset(Elab *e, const Form *call);
Expr *elab_c_call(Elab *e, const Form *call);
Expr *elab_call_ptr(Elab *e, const Form *call);
Expr *elab_callback_ptr(Elab *e, const Form *call);
Expr *elab_dlopen(Elab *e, const Form *call);
Expr *elab_dlsym(Elab *e, const Form *call);
Expr *elab_dlclose(Elab *e, const Form *call);
Expr *elab_unsafe(Elab *e, const Form *call);
/* proper-tail-calls T1 (T-D1): `(^tailcall <call>)` -- elaborates <call> and
 * flags it `wants_tailcall`, so the emitter reports TUR-E0716 if it does not
 * end up in tail position.  Returns the annotated call itself; the annotation
 * is a check, never a change of meaning. */
Expr *elab_tailcall(Elab *e, const Form *call);

/* elab_forms.c */
Form *splice_internal_defines(Elab *e, Form **items, uint32_t n, Span span);
Expr *elab_let(Elab *e, const Form *call);
/* compiled-closure-copies-a-captured-mut: the `(.v <cell>)` read a use of a
 * cell-aliased `^mut` name elaborates as (see Binding.cell_hidden_sym). */
Form *elab_mut_cell_read_form(Elab *e, const Binding *alias, Span span);
Expr *elab_letstar(Elab *e, const Form *call);
Expr *elab_letrec(Elab *e, const Form *call);
Expr *elab_named_let(Elab *e, const Form *call);
Expr *elab_do(Elab *e, const Form *call);
Expr *elab_if(Elab *e, const Form *call);
Expr *elab_thread(Elab *e, const Form *call);
Expr *elab_thread_last(Elab *e, const Form *call);
Expr *elab_set(Elab *e, const Form *call);
Expr *elab_while(Elab *e, const Form *call);
Expr *elab_case(Elab *e, const Form *call);
/* Elaborate forms [start..len) of `call` as one implicit body block: a single
 * form elaborates to itself, two or more are wrapped in an EX_DO.  This is what
 * lets `defer`, `reset`, `serial-reset` and `cloneable-reset` take a body while
 * their lowerings keep taking a single `Expr *`. */
Expr *elab_implicit_do(Elab *e, const Form *call, uint32_t start);
Expr *elab_defer(Elab *e, const Form *call);
Expr *elab_return(Elab *e, const Form *call);
Expr *elab_question(Elab *e, const Form *call);

/* GF1: Generator forms */
Expr *elab_gen(Elab *e, const Form *call);
Expr *elab_yield(Elab *e, const Form *call);
Expr *elab_gen_next(Elab *e, const Form *call);
Expr *elab_gen_done(Elab *e, const Form *call);
Expr *elab_gen_unwrap(Elab *e, const Form *call);

/* elab_memory.c */
Expr *elab_ref(Elab *e, const Form *call);
Expr *elab_lref_new(Elab *e, const Form *call);
Expr *elab_deref(Elab *e, const Form *call);
Expr *elab_drop(Elab *e, const Form *call);
Expr *elab_rc_of(Elab *e, const Form *call);
Expr *elab_rc_clone(Elab *e, const Form *call);
Expr *elab_rc_drop(Elab *e, const Form *call);
Expr *elab_rc_ptr(Elab *e, const Form *call);
Expr *elab_rc_strong_count(Elab *e, const Form *call);
Expr *elab_rc_from_ref(Elab *e, const Form *call);
Expr *elab_ref_from_rc(Elab *e, const Form *call);
Expr *elab_weak(Elab *e, const Form *call);
Expr *elab_weak_upgrade(Elab *e, const Form *call);
Expr *elab_weak_pred(Elab *e, const Form *call);
Expr *elab_ref_pred(Elab *e, const Form *call);
Expr *elab_gc_force(Elab *e, const Form *call);
Expr *elab_gc_enable(Elab *e, const Form *call);
Expr *elab_gc_disable(Elab *e, const Form *call);
Expr *elab_gc_auto(Elab *e, const Form *call);
Expr *elab_gc_collections(Elab *e, const Form *call);
Expr *elab_gc_objects_freed(Elab *e, const Form *call);
Expr *elab_gc_live_blocks(Elab *e, const Form *call);
Expr *elab_gc_candidate_high_water(Elab *e, const Form *call);

/* elab_effects.c */
Expr *elab_reset(Elab *e, const Form *call);
Expr *elab_shift(Elab *e, const Form *call);
Expr *elab_shift0(Elab *e, const Form *call);
Expr *elab_cloneable_reset(Elab *e, const Form *call);
Expr *elab_cloneable_shift(Elab *e, const Form *call);
Expr *elab_call_cc_star(Elab *e, const Form *call);
Expr *elab_serial_reset(Elab *e, const Form *call);
Expr *elab_serial_shift(Elab *e, const Form *call);
Expr *elab_try_with(Elab *e, const Form *call);
Expr *elab_defeffect(Elab *e, const Form *call);
Expr *elab_perform(Elab *e, const Form *call);
/* cps-backend-n6 cross-function resume: gated post-elaboration pass that wraps
 * every recorded reset body in a __Shift handler when the program contains a
 * cross-function resuming shift.  No-op otherwise.  Called by elaborate_program
 * after the top-level elaboration loop. */
void elab_wrap_resets_for_crossfn_resume(Elab *e);
Expr *elab_handle(Elab *e, const Form *call);
Expr *elab_handle_shallow(Elab *e, const Form *call);
/* FH2-FH5: first-class handler values */
Expr *elab_handler_lit(Elab *e, const Form *call);
Expr *elab_with_handler(Elab *e, const Form *call);
Expr *elab_compose_handlers(Elab *e, const Form *call);
bool cont_check_double_use(Elab *e, const Form *k_form);
Expr *elab_resume(Elab *e, const Form *call);
Expr *elab_make_resume(Elab *e, Expr *k, Expr *value, Span span);
Expr *elab_discontinue(Elab *e, const Form *call);
Expr *elab_cont_pred(Elab *e, const Form *call);
Expr *elab_call_cc(Elab *e, const Form *call);
Expr *elab_escape(Elab *e, const Form *call);

/* elab_fns.c */
Expr *elab_defn(Elab *e, const Form *call);
Expr *elab_fn(Elab *e, const Form *call);
/* bare-fat-param-non-int-result inference (Phase A); see
 * docs/archive/history/bare-fat-result-type-inference-plan.md. */
bool kind_is_non_int_register_class(TypeKind k);

/* The float register class, as the untyped-fn carrier rule spells it: a value
 * of this kind lives in xmm, not a GP register, so it cannot cross a
 * signature-less `fn` slot (a `:fn` parameter, or a struct field declared
 * bare `fn`) whose calling convention is the int64 word. */
static inline bool kind_is_float_class(TypeKind k) {
    return kind_is_non_int_register_class(k) || k == TY_FLOAT32 || k == TY_FLOAT64;
}

/* True when a function value of type `ft` has a float-class argument or
 * result, so it cannot be stored into -- or called through -- a
 * signature-less `fn` slot without a register-class miscompile. */
static inline bool fn_sig_has_float_class(const Type *ft) {
    if (!ft || ft->kind != TY_FN) return false;
    if (kind_is_float_class(ft->as.fn.result_kind)) return true;
    for (uint32_t k = 0; k < ft->as.fn.arity; k++)
        if (ft->as.fn.arg_kinds && kind_is_float_class(ft->as.fn.arg_kinds[k]))
            return true;
    return false;
}

/* bare-fat-result-monomorphization (Phase B); see
 * docs/archive/history/bare-fat-result-monomorphization-plan.md.
 *  - elab_specialize_bare_fat: re-elaborate `callee`'s retained body with its
 *    bare-^fat param result kind set to `k`, returning the clone's binding
 *    (memoized by (callee, k)); NULL if it cannot be specialized.
 *  - elab_track_bare_fat_lazy: register a deferred-canonical binding for the
 *    end-of-pass sweep.
 *  - elab_sweep_bare_fat_lazy: surface the deferred diagnostic for any lazy
 *    binding no call site specialized. */
Binding *elab_specialize_bare_fat(Elab *e, Binding *callee, TypeKind k);
void     elab_track_bare_fat_lazy(Elab *e, Binding *b);
void     elab_sweep_bare_fat_lazy(Elab *e);
bool retype_bare_fat_tail_calls(Expr *tail, TypeKind target);
Expr *elab_extern_c(Elab *e, const Form *call);
Expr *elab_def(Elab *e, const Form *call);

/* elab_call.c */
Expr *elab_call(Elab *e, Form *call);
/* SZ8 size-index recovery: the declared/ascribed type Form describing `x`'s
 * static type (NULL when none is recoverable). */
const Form *sz_recover_type_form(Elab *e, const Expr *x);
/* declared-size-index-never-checked-against-value: true (with the two folded
 * values) when a written `(Head idx ...)` size claim and the expression it
 * describes disagree at a position where both are statically known. */
bool sz_claim_disagrees(Elab *e, const Form *claim, const Expr *x,
                        int64_t *claimed, int64_t *actual);
/* gadt-length-index-not-enforced: is the claim that a GADT value has index
 * `claim` (an ascription, a declared return, an annotated let) provably false
 * of `x`?  `*got` is what x's index is known to be.  elab_call.c. */
bool gadt_claim_disagrees(Elab *e, const Form *claim, const Expr *x,
                          const struct Form **got);
/* gadt-length-index-not-enforced: `x` wrapped in an ascription to its known
 * GADT index, or `x` unchanged.  elab_call.c. */
Expr *gadt_refine_to_index(Elab *e, Expr *x);
Binding *make_poly_wrapper(Elab *e, Binding *inner_b, uint8_t inner_arity, Span span, bool typed_concrete);
/* MB1 (constrained-hkt-forall-mode-b): variant with `n_lead_ignore` leading
 * dict-carrier params the wrapper accepts but does not forward to the inner. */
Binding *make_poly_wrapper_ex(Elab *e, Binding *inner_b, uint8_t inner_arity,
                              uint8_t n_lead_ignore, Span span, bool typed_concrete);
Binding *poly_arg_fn_binding(Expr *arg);
/* MB1: build a dict-clone of a single-constraint polymorphic constrained fn. */
Binding *make_dict_clone(Elab *e, Binding *inner_b, Span span);

/* elab_module.c */
Expr *elab_load(Elab *e, const Form *call);
Expr *elab_defmodule(Elab *e, const Form *call);
Binding *elab_lookup_sym(Elab *e, const Symbol *sym, Span span, bool *had_error);

/* elab_structs.c */
void elab_add_forward_type(Elab *e, const Symbol *sym);
Expr *elab_defstruct(Elab *e, const Form *call);
Expr *elab_defopaque(Elab *e, const Form *call);
void elab_register_adt_def(Elab *e, AdtDef *def);
Expr *elab_defdata(Elab *e, const Form *call);
/* TP6: unpack a TY_APP chain on an ADT type to recover concrete type arguments
 * (into out_args, sized def->n_type_params); true on a fully-applied match.
 * adt_field_instantiate_type substitutes those args for the TY_TYVAR names in a
 * field type.  Shared with the dot-accessor field-access path. */
bool elab_adt_type_extract_args(const Type *t, const AdtDef *def, Type *out_args);
Type adt_field_instantiate_type(Elab *e, const AdtDef *def, const Type *t,
                                const Type *type_args);
/* Grounds a record-ADT ctor's type params (named in `tps`) by unifying each
 * declared field full_type against the supplied value's actual type, descending
 * into TY_APP/TY_FN fields.  Inference only -- a concrete field never fails. */
bool adt_field_collect_type_args(const char **tps, uint8_t n_tps,
                                 const Type *expected, Type actual,
                                 Type *type_args, bool *have_type_args);
TypeKind gadt_skolem_lookup(const SkolemEnv *env, const char *name);
Expr *elab_defgadt(Elab *e, const Form *call);
Expr *elab_coerce(Elab *e, const Form *call);
CtorDef *elab_lookup_ctor(Elab *e, const Symbol *name);
/* CONV-S6: classify + describe a product-shape construction surface for
 * diagnostics.  `conv_surface_is_struct` is true for a single-variant ADT
 * lowered from `defstruct`; `conv_surface_phrase` writes "struct 'Foo'" or
 * "variant 'Circle' of type 'Shape'" into `buf` and returns it. */
bool conv_surface_is_struct(const AdtDef *def);
const char *conv_surface_phrase(const AdtDef *def, const CtorDef *ctor,
                                char *buf, size_t buflen);
Expr *elab_match(Elab *e, const Form *call);
Expr *elab_make_struct(Elab *e, const Form *call);
Expr *elab_with(Elab *e, const Form *call); /* WITH-V0 */
Expr *elab_default_of(Elab *e, const Form *call);  /* M2b */
Expr *elab_borrow_immut(Elab *e, const Form *call);
/* typeclass-dispatch-on-any-receiver-emits-uncompilable-c: build the checked
 * `any` unbox (the EX_ANY_CAST `(cast x T)` lowers to) from an already-resolved
 * target Type.  Used by the `@TypeName` witness path, which pins an instance
 * and therefore already knows the type the receiver must be unboxed to. */
Expr *elab_any_unbox_to(Elab *e, Expr *val, Type target, Span span);
/* r7rs-type-errors-are-uncatchable-panics: record on a raising Scheme cast
 * (elab_any_unbox_to's result) the procedure its value is passed to, so the
 * error names it ("car: not a pair").  A no-op for any other node. */
void elab_any_cast_note_callee(Elab *e, Expr *cast, const Binding *callee);
/* saffron-lang-plan S2/D3: the declared TypeKind of an UNANNOTATED positional
 * parameter -- `any` in a Saffron file, `int` in a Turmeric one.  Defined in
 * elab_fns.c, where the full reasoning and the two deliberate exclusions
 * (`& rest`, `extern-c`) are documented; also read by defeffect. */
TypeKind saffron_default_param_kind(Span sp);
/* Convert a runtime Type back to its source-form spelling (primitives,
 * named ADTs, TY_APP chains).  NULL for a kind that has no source form --
 * callers use that as a clean decline.  Defined in elab_typeclasses.c. */
Form *type_to_form(Elab *e, const Type *t, Span span);
/* saffron-lang-plan D4: wrap an `any` in the truthiness operator so it can feed
 * a C-level `bool` slot (an `if` condition, a contract predicate).  Returns the
 * expression unchanged when it is not an `any` in a Saffron file, so callers
 * apply it unconditionally.  Defined in elab_forms.c. */
Expr *elab_saffron_truthy(Elab *e, Expr *cond, Span site);
/* saffron-lang-plan D7: reject a static-only feature in a Saffron file, naming
 * it and the guarantee it rests on.  Returns true when it rejected (false
 * outside Saffron, so callers can guard on it).  Defined in elab_forms.c. */
bool saffron_reject_static_only(Elab *e, Span span, const char *feature,
                                const char *why);
Expr *elab_borrow_mut(Elab *e, const Form *call);

/* elab_types.c */
Expr *elab_defkind(Elab *e, const Form *call);
Expr *elab_defrec(Elab *e, const Form *call);
Expr *elab_defalias(Elab *e, const Form *call);
Type *type_expr_from_form(Elab *e, const Form *form, const Symbol *rec_name,
    const Symbol **type_params, Kind *type_param_kinds,
    uint8_t n_type_params);
/* ptr-generic-parameterised-type: resolve a "ptr<T>" type-name string to a
 * typed raw pointer (TY_PTR_VOID with non-NULL pointee).  Returns NULL when
 * the name is not a typed "ptr<...>" form (e.g. "ptr<void>" or a non-ptr
 * name), so callers can fall through to their existing handling. */
Type *ptr_type_from_keyword_name(Elab *e, const char *name, uint32_t len,
    Span span, const Symbol *rec_name, const Symbol **type_params,
    Kind *type_param_kinds, uint8_t n_type_params);
/* rc-angle-bracket-annotation-becomes-tyvar: resolve a typed reference-family
 * keyword annotation (`rc<T>`, `weak<T>`, `ref<T>`, `lref<T>`) to its real
 * TY_RC/TY_WEAK/TY_REF/TY_LREF Type.  Returns NULL when the name is not one of
 * these angle-bracket forms, so callers fall through to their existing
 * handling (bare `rc`, tyvar, alias, ...). */
Type *rc_family_type_from_keyword_name(Elab *e, const char *name, uint32_t len,
    Span span, const Symbol *rec_name, const Symbol **type_params,
    Kind *type_param_kinds, uint8_t n_type_params);
/* Resolve a type-annotation form (F_KEYWORD `:int`, spaced F_TYPE_ANN
 * wrapping any form, or a list constructor like `(-> a b)`) into a Type*.
 * Unwraps F_TYPE_ANN and delegates to type_expr_from_form for the inner
 * form, with arrow / borrow / handler / session / role heads routed
 * through their constructor paths.  Used by defn, fn, defstruct, and
 * let-bindings to share one type-form parser. */
Type *fn_type_from_form(Elab *e, const Form *form,
                        const Symbol **type_params,
                        Kind *type_param_kinds,
                        uint8_t n_type_params);
/* MF4: separate struct / GADT namespaces. Resolves a type name by walking
 * the ADT registry first, then the struct registry. Returns an
 * arena-allocated Type* (TY_ADT or TY_STRUCT) when found, else NULL.
 * Prefers GADTs over structs when both share a name, per the MF4 design. */
Type *elab_lookup_type_by_name(Elab *e, const Symbol *name);
/* CTOR-V0: scope-aware lookup that returns the nearest TY_STRUCT/TY_ADT binding
 * for `name` (skipping shadowing value bindings of other kinds), or NULL. */
Binding *scope_lookup_type_def(struct Scope *s, const Symbol *name);
/* Variadic HKT rows: validate a type-application argument's kind against the
 * constructor's positional parameter kind (row concern only). arg_index is
 * 0-based. Returns false and emits TUR-E0012 on a row/non-row mismatch. */
bool check_row_type_arg_kind(Type ctor_type, uint8_t arg_index, Type arg_type,
                             Span arg_span);
Expr *elab_deftype(Elab *e, const Form *call);
Expr *elab_type_app(Elab *e, const Form *call);
Expr *elab_ascribe(Elab *e, const Form *call);
Expr *elab_pack(Elab *e, const Form *call);
Expr *elab_open(Elab *e, const Form *call);

/* elab_typeclasses.c */
Expr *elab_defclass(Elab *e, const Form *call);
Expr *elab_definstance(Elab *e, const Form *call);
/* class-superclasses SC2/SC4: the post-unit pass over the superclass graph --
 * resolve each defclass preamble, reject cycles, and enforce the instance
 * obligation (Half B).  Runs after every form in the unit is registered, from
 * elaborate_program_session.  Returns false when it reported an error. */
bool elab_typeclass_superclasses_finish(Elab *e);
/* class-and-generic-in-an-instance-less-module: retry the parked defns (see
 * Elab.noinst_pending).  `final` elaborates whatever is left for real and
 * returns false when any still fails; otherwise a retry happens only when an
 * instance registered since the last one, and always returns true. */
bool elab_noinst_retry(Elab *e, bool final);
Expr *elab_method_call(Elab *e, const Form *call);
/* Phase RT: if `name` is a typeclass method whose dispatch type variable
 * appears only in the return type (a return-only-dispatch method, e.g.
 * `default-of [] : a`), resolve the instance from the expected-type channel
 * (e->expected_type) and emit a direct call to that instance's impl.  Sets
 * *handled to true when `name` matched such a method (even if resolution
 * failed and a diagnostic was emitted), so the caller does not fall through to
 * the ordinary call path.  Returns NULL when not handled or on error. */
Expr *elab_try_return_dispatch(Elab *e, const Form *call, const Symbol *name,
                               bool *handled);

/* True when `name` resolves to a return-only-dispatch typeclass method (its
 * dispatch tyvar appears only in the result) with no shadowing binding, so its
 * instance can only be picked from an expected result type.  elab_if uses this
 * to let a concrete sibling arm supply the expected type for such a method
 * (return-directed-methods-pure-empty-inference, fix direction #2). */
bool elab_symbol_is_return_dispatch_method(Elab *e, const Symbol *name);

/* elab_concurrent.c */
Expr *elab_thread_spawn(Elab *e, const Form *call);
Expr *elab_async(Elab *e, const Form *call);
Expr *elab_await(Elab *e, const Form *call);
Expr *elab_select(Elab *e, const Form *call);
Expr *elab_panic(Elab *e, const Form *call);
Expr *elab_panic_with(Elab *e, const Form *call);
Expr *elab_catch_unwind(Elab *e, const Form *call);
Expr *elab_catch_panic_of(Elab *e, const Form *call);
Expr *elab_panic_payload_type(Elab *e, const Form *call);
Expr *elab_panic_payload_value(Elab *e, const Form *call);
Expr *elab_panic_payload_file(Elab *e, const Form *call);
Expr *elab_panic_payload_line(Elab *e, const Form *call);
Expr *elab_panic_payload_downcast(Elab *e, const Form *call);
Expr *elab_stm(Elab *e, const Form *call);
Expr *elab_atomically(Elab *e, const Form *call);
Expr *elab_retry(Elab *e, const Form *call);
Expr *elab_check(Elab *e, const Form *call);
Expr *elab_or_else(Elab *e, const Form *call);
Expr *elab_tvar_new(Elab *e, const Form *call);
Expr *elab_tvar_read(Elab *e, const Form *call);
Expr *elab_tvar_write(Elab *e, const Form *call);
Expr *elab_tvar_modify(Elab *e, const Form *call);
Expr *elab_tvar_swap(Elab *e, const Form *call);
Expr *elab_tvar_cas(Elab *e, const Form *call);

/* elab_toplevel.c */
Expr *elab_as_cast(Elab *e, const Form *call);
Expr *elab_any_type_of(Elab *e, const Form *call);
Expr *elab_any_cast(Elab *e, const Form *call);
Expr *elab_is_q(Elab *e, const Form *call);
Expr *elab_form(Elab *e, Form *f);

/* elab_global.c -- SS5: multi-party global protocol types */
Expr *elab_defprotocol(Elab *e, const Form *call);
Expr *elab_make_protocol(Elab *e, const Form *call);
Expr *elab_send_to(Elab *e, const Form *call);
Expr *elab_recv_from(Elab *e, const Form *call);
Expr *elab_recv_timeout_from(Elab *e, const Form *call);
/* Advance a role cursor past every step the role is not party to: a message
 * between two other roles, and a timed receive between two other roles (whose
 * two continuations project identically for this role -- defprotocol checks
 * that -- so the cursor follows the `ok` one). */
GlobalInteraction *role_skip_bystander_steps(GlobalInteraction *step,
                                             const char *role_name);

/* project.c -- SS6: projection algorithm */
Type *session_project(Elab *e, GlobalInteraction *step, const char *role, Span span);

/* elab_dynvars.c */
Expr       *elab_defdynamic(Elab *e, const Form *call);
Expr       *elab_binding(Elab *e, const Form *call);
DynVarEntry *dynvar_lookup(const Elab *e, const Symbol *name);

/* elab_effects.c -- fx-row-syntax-rename-plan Phase 2 deprecation warner */
/* The index of a `(defn ...)` form's name, past its pre-name attributes
 * (elab_toplevel.c).  Returns the form's length when there is no name. */
uint32_t elab_defn_name_index(const Elab *ep, const Form *f);
void warn_legacy_fx_row(Form *f);

/* elab_sessions.c */
Expr *elab_session_make(Elab *e, const Form *call);
Expr *elab_session_send(Elab *e, const Form *call);

/* Session payload lowering onto the runtime's int64 rendezvous word
 * (session-payloads-are-int64-only).  The compiled channel carries one
 * machine word; a payload crosses it by value-cast (ints/bools), by bit
 * reinterpretation (floats -- a value cast would truncate 7.25 to 7), or by
 * an explicit pointer<->intptr cast (cstr, Session/Role endpoints, rc, ...).
 * A by-value aggregate has no word-sized carrier and is rejected at
 * elaboration rather than handed to cc. */
typedef enum SessPayloadClass {
    SESS_PAY_INT = 0,      /* (int64_t)(x) -- ints, bools, handles carried as int64 */
    SESS_PAY_F64,          /* tur_session_f64_bits / tur_session_bits_f64 */
    SESS_PAY_F32,          /* tur_session_f32_bits / tur_session_bits_f32 */
    SESS_PAY_PTR,          /* (int64_t)(intptr_t)(x) / (void *)(intptr_t)x */
    SESS_PAY_UNSUPPORTED,  /* by-value struct/ADT: TUR-E0212 */
} SessPayloadClass;
SessPayloadClass session_payload_class(Type t);
/* Emit TUR-E0212 naming `op` and the payload type when the class is
 * SESS_PAY_UNSUPPORTED; returns false in that case. */
bool session_payload_supported(Elab *e, Type t, Span span, const char *op);
/* Arena-allocated C expression converting `inner` (a C expression of the
 * payload's C type) to the int64 word, and back from a word to the payload
 * type.  `inner` need not be NUL-terminated: pass its length. */
const char *session_payload_to_word(Elab *e, Type t, const char *inner, size_t inner_len);
const char *session_payload_from_word(Elab *e, Type t, const char *inner, size_t inner_len);
Expr *elab_session_recv(Elab *e, const Form *call);
Expr *elab_session_close(Elab *e, const Form *call);
Expr *elab_session_offer(Elab *e, const Form *call);
Expr *elab_session_choose_left(Elab *e, const Form *call);
Expr *elab_session_choose_right(Elab *e, const Form *call);
Expr *elab_session_recv_timeout(Elab *e, const Form *call);


/* any-struct-box-leak-per-widen: does this expression evaluate to an `any` whose
 * payload box the evaluating expression owns?  See elab_call.c. */
bool any_expr_is_owned_temp(const Expr *x, int depth);

/* RM1 (reclamation-plan): the per-callee freshness analysis -- see the walker
 * in elab_fns.c and the flag's comment in expr.h. */
bool elab_body_returns_fresh_sum_box(const Expr *e);
/* ... and its stamping form: sets both returns_fresh_sum_box and
 * fresh_sum_via_param_mask on `b` from the elaborated body. */
void elab_stamp_sum_freshness(Binding *b, Binding **params, uint32_t n_params,
                              const Expr *body);
/* The non-retaining parameter masks (fn / ptr-scalar / sum), inferred from the
 * elaborated body.  Shared by defn and instance-method elaboration. */
void elab_infer_nonretain_masks(Binding *b, Binding **params, uint32_t n_params,
                                Expr *body);


/* r7rs-programs-compile-slowly: the first id of the stdlib's own counter --
 * far past anything a program draws, so the two ranges never meet. */
#define ELAB_STDLIB_ID_BASE 1000000u

/* A fresh binding / gensym id (see `next_stdlib_id`). */
static inline uint32_t elab_fresh_id(Elab *e) {
    return (e->in_stdlib_load || e->ids_stdlib) ? e->next_stdlib_id++ : e->next_id++;
}

#endif
