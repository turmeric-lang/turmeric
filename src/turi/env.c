#include "env.h"
#include "eval.h"   /* TURI_DEFAULT_SANDBOX_FUEL */
#include "fiber.h"
#include "reader_macros.h"  /* RM Q#5: session-scoped reader-macro registry */
#include "elab.h"           /* TR2.2b: persistent ElabSession lifecycle */
#include "spice_loader.h"   /* RP3: env owns the loaded TurSpiceImage */
#include "collections_native.h"  /* Vec/Set/Map/HAMT native overrides */
#include "string_native.h"        /* owned String type native overrides */
#include "interpreter_natives.h"  /* option/result/str/math/seq/json/... natives */
#include "../runtime/globals.h"  /* g_interpret_mode (libturi-embed-interpret-mode-flag) */
#include "../compiler/lang_dialects.h"  /* lang_traits: the session language's prelude */

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Recursion-depth guard: RETIRED (C4, turi-c-scoped-forms-heap-bounding)
 *
 * The old `eval_depth >= max_eval_depth` guard turned a would-be C-stack
 * overflow into a graceful "recursion limit exceeded".  After the trampoline
 * (turi-eval-trampoline-plan) and turi-c-scoped-forms-heap-bounding (C1-C3),
 * every interpreter recursion -- tail / non-tail, reset/shift, call/cc,
 * serial/cloneable resume, catch-unwind, atomically, and effect handlers --
 * folds onto the heap work-stack and runs 1,000,000 deep with no C-stack growth
 * per level, so the guard was dead for every real program and its stack-size-
 * derived sizing (TURI_EVAL_FRAME_BYTES et al.) is gone.  Sandbox recursion
 * limiting is now step-fuel's job (turi_env_set_fuel): it bounds total work
 * regardless of shape, and unbounded folded recursion grows the heap
 * work-stack (reclaimed at teardown), not the C stack.
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * Global-binding hash table (open addressing, linear probing)
 * ---------------------------------------------------------------------- */

#define HT_INIT_CAP 64u

static uint32_t ht_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void ht_init(EnvHashTable *ht) {
    ht->cap   = HT_INIT_CAP;
    ht->count = 0;
    ht->slots = (EnvBinding **)calloc(HT_INIT_CAP, sizeof(EnvBinding *));
}

static void ht_insert_raw(EnvHashTable *ht, EnvBinding *b) {
    uint32_t mask = ht->cap - 1u;
    uint32_t idx  = ht_hash(b->name) & mask;
    while (ht->slots[idx]) {
        if (strcmp(ht->slots[idx]->name, b->name) == 0) {
            ht->slots[idx] = b;  /* replace existing slot pointer */
            return;
        }
        idx = (idx + 1u) & mask;
    }
    ht->slots[idx] = b;
    ht->count++;
}

static void ht_grow(EnvHashTable *ht) {
    uint32_t old_cap   = ht->cap;
    EnvBinding **old   = ht->slots;
    ht->cap   = old_cap * 2u;
    ht->count = 0;
    ht->slots = (EnvBinding **)calloc(ht->cap, sizeof(EnvBinding *));
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i]) ht_insert_raw(ht, old[i]);
    }
    free(old);
}

/* Insert or update the slot for b->name to point to b. */
static void ht_insert(EnvHashTable *ht, EnvBinding *b) {
    if (ht->count * 3u >= ht->cap * 2u) ht_grow(ht);
    ht_insert_raw(ht, b);
}

/* Find an EnvBinding* for name, or NULL if not present. */
static EnvBinding *ht_find(const EnvHashTable *ht, const char *name) {
    if (!ht->slots) return NULL;
    uint32_t mask = ht->cap - 1u;
    uint32_t idx  = ht_hash(name) & mask;
    for (;;) {
        EnvBinding *b = ht->slots[idx];
        if (!b) return NULL;
        if (b->name == name || (b->name[0] == name[0] && strcmp(b->name, name) == 0))
            return b;
        idx = (idx + 1u) & mask;
    }
}

/* -------------------------------------------------------------------------
 * Default-natives registry (libturi-per-embed-env-and-peripherals Gap 1)
 *
 * An embedder seeds a fixed set of natives once; every subsequently-created
 * env installs them automatically, so a host with N per-script envs does not
 * re-register the same table N times (and cannot forget one).
 * ---------------------------------------------------------------------- */

typedef struct DefaultNative {
    char        *name;   /* owned (strdup) */
    TuriNativeFn fn;
    void        *ud;
} DefaultNative;

static DefaultNative *g_default_natives;
static size_t         g_default_natives_count;
static size_t         g_default_natives_cap;

void turi_register_default_native(const char *name, TuriNativeFn fn, void *ud) {
    if (!name || !fn) return;
    for (size_t i = 0; i < g_default_natives_count; i++) {
        if (strcmp(g_default_natives[i].name, name) == 0) {
            g_default_natives[i].fn = fn;
            g_default_natives[i].ud = ud;
            return;
        }
    }
    if (g_default_natives_count == g_default_natives_cap) {
        size_t nc = g_default_natives_cap ? g_default_natives_cap * 2u : 8u;
        DefaultNative *grown =
            (DefaultNative *)realloc(g_default_natives, nc * sizeof(DefaultNative));
        if (!grown) return;
        g_default_natives     = grown;
        g_default_natives_cap = nc;
    }
    DefaultNative *e = &g_default_natives[g_default_natives_count++];
    e->name = strdup(name);
    e->fn   = fn;
    e->ud   = ud;
}

/* Typed variant: register the default native as usual, then record its runtime
 * return type in the process-global signature registry so the elaborator types
 * calls to it (and curated typed wrappers over it) correctly.  See
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md. */
void turi_register_default_native_typed(const char *name, TuriNativeFn fn,
                                        void *ud, TurNativeRetType ret) {
    turi_register_default_native(name, fn, ud);
    tur_native_sig_register(name, ret);
}

void turi_clear_default_natives(void) {
    for (size_t i = 0; i < g_default_natives_count; i++) free(g_default_natives[i].name);
    free(g_default_natives);
    g_default_natives       = NULL;
    g_default_natives_count = 0;
    g_default_natives_cap   = 0;
}

static void install_default_natives(TuriEnv *env) {
    for (size_t i = 0; i < g_default_natives_count; i++)
        turi_env_register_native(env, g_default_natives[i].name,
                                 g_default_natives[i].fn, g_default_natives[i].ud);
}

/* -------------------------------------------------------------------------
 * Native ud finalizers (libturi-per-embed-env-and-peripherals Gap 5)
 *
 * turi_env_register_native_ex records a (free_fn, ud) pair here; turi_env_free
 * fires them in LIFO order so a native whose ud is owned by the env (e.g. a
 * per-script object) is torn down with the env.  turi_env_reset deliberately
 * leaves them intact -- natives survive a reset, so their ud must too.
 * ---------------------------------------------------------------------- */

typedef struct NativeFinalizer {
    TuriNativeFreeFn        free_fn;
    void                   *ud;
    struct NativeFinalizer *next;
} NativeFinalizer;

/* -------------------------------------------------------------------------
 * TuriEnv lifecycle
 * ---------------------------------------------------------------------- */

TuriEnv *turi_env_new(void) {
    /* libturi-embed-interpret-mode-flag: anybody calling turi_env_new() is by
     * definition an interpreter embedder; the compiler path never builds an env
     * this way. Flip the process-global elaborator flag so registered natives
     * resolve at runtime instead of failing as "unknown function". */
    g_interpret_mode = true;
    TuriEnv *env = (TuriEnv *)calloc(1, sizeof(TuriEnv));
    if (!env) return NULL;
    arena_init(&env->sym_arena, 0);
    /* turi-env-owned-value-arena-pool-plan: value-payload pools. Init before any
     * builtin registration, which allocates native closures from scratch.
     * turi-value-pool-scratch-promotion-plan: scratch is the default allocation
     * target; perm receives promoted escapees (empty until promotion runs). */
    arena_init(&env->value_scratch, 0);
    arena_init(&env->value_perm, 0);
    symtab_init(&env->st, &env->sym_arena);
    buf_init(&env->src_acc);
    buf_init(&env->src_combined);   /* TR2.3: reused per-eval source blob */
    /* TR2: incremental parse + elaboration is ON by default. A long-lived env
     * (REPL, notebook kernel, Trowel / Try Turmeric / Godot embeddings) is
     * otherwise O(N^2) in retained memory and time over a session -- measured at
     * ~1 GB and quadratic parse/elaborate over 1500 turns, versus ~2 MB and
     * linear with this on. A single-eval embedder is unaffected: the first eval
     * has no accumulated prefix, so it takes the whole-program path either way.
     *
     * Results are identical to the old path except for one intentional fix:
     * redefining a top-level `defn` across turns now works instead of failing
     * with "already defined by an auto-loaded stdlib module" (an artifact of the
     * old path re-elaborating prior turns under stdlib_prefix). Guarded by the
     * tur_incremental_elab_diff A/B harness.
     *
     * TUR_NO_INCREMENTAL_ELAB=1 restores the whole-program path, for bisecting a
     * suspected incremental-path bug; turi_env_set_incremental_elab overrides. */
    {
        const char *off = getenv("TUR_NO_INCREMENTAL_ELAB");
        env->incremental_elab = !(off && *off && strcmp(off, "0") != 0);
    }
    env->caps = TURI_CAP_ALL;
    /* Gap 7: default to interpret mode (every turi_env_new caller is an
     * interpreter embedder; mirrors the g_interpret_mode = true above).  An
     * embedder driving compile-mode elaboration flips this with
     * turi_env_set_interpret_mode. */
    env->interpret_mode = true;
    ht_init(&env->globals_ht);
    /* Phase S7: initialise async scheduler state */
    turi_sched_init(env);
    /* Register async native builtins (sleep-async, with-timeout, etc.) */
    turi_async_register_builtins(env);
    /* Register eval-layer native builtins (panic?, etc.) */
    turi_eval_register_builtins(env);
    /* Register Vec/Set/Map/HAMT collection native overrides so collections
     * resolve for every interpreter env created through libturi (embedders,
     * the WASM REPL, the test harnesses), not just the `tur` CLI.  Registered
     * before install_default_natives so an embedder-seeded default of the same
     * name still wins.  See docs/archive/history/turi-interp-collections-libturi-plan.md. */
    turi_register_collection_natives(env);
    /* Owned String type (owned-string-type-plan): register the tur_string_*
     * primitives so stdlib/string.tur's pure-Turmeric ops + typeclass instances
     * resolve under --interpret exactly as in compiled code. */
    turi_register_string_natives(env);
    /* Register the remaining stdlib inline-C native overrides (option/result/
     * str/math/safe/contract/comonad/typeclass, seq, json/schema, the
     * concurrency + OS-handle modules, and sym) so every interpreter env
     * created through libturi -- embedders driving turi_eval, the WASM REPL,
     * `tur repl`, and the fixture-runner worker -- resolves the same overrides
     * as the `tur --interpret` path, rather than hitting "inline-C not
     * supported in interpreter mode" the moment an op bottoms out in one.
     * Registered here (before any preload) rather than after: the EX_FN_DEF
     * "keep native override" branch in eval.c preserves a pre-registered native
     * when a module's inline-C body of the same name is later loaded, so the
     * shim still wins over the loaded body -- the same ordering the collection
     * natives above rely on.  The CLI/REPL/WASM entry points also call this
     * after their preload; that re-registration is idempotent (same name -> same
     * fn pointer).  See docs/archive/turi-interp-stdlib-natives-libturi-plan.md. */
    turi_env_register_interpreter_natives(env);
    /* RM Q#5: persistent reader-macro registry for session semantics. */
    env->reader_macros = (ReaderMacroRegistry *)arena_alloc(
        &env->sym_arena, sizeof(ReaderMacroRegistry));
    reader_macros_init(env->reader_macros, &env->sym_arena);
    /* Gap 1: install any embedder-seeded default natives last, so they can
     * override a builtin of the same name if the embedder intends to. */
    install_default_natives(env);
    return env;
}

TuriEnv *turi_env_new_with_natives(const TuriNativeSpec *specs, size_t n) {
    TuriEnv *env = turi_env_new();
    if (!env) return NULL;
    for (size_t i = 0; specs && i < n; i++) {
        if (specs[i].name && specs[i].fn)
            /* Gap 5: honour each spec's optional ud finalizer. */
            turi_env_register_native_ex(env, specs[i].name, specs[i].fn,
                                        specs[i].ud, specs[i].free_ud);
    }
    return env;
}

TuriEnv *turi_env_new_sandboxed(void) {
    TuriEnv *env = turi_env_new();
    if (!env) return NULL;
    env->sandboxed       = true;
    env->caps            = TURI_CAP_NONE;
    /* C4: recursion is heap-bounded, so a sandboxed env limits work via
     * step-fuel alone (the eval_depth guard was retired). */
    env->step_fuel_limit = TURI_DEFAULT_SANDBOX_FUEL;
    env->step_fuel       = TURI_DEFAULT_SANDBOX_FUEL;
    /* security-audit-plan S-5: a sandbox is restricted from birth (no preload
     * runs before this), so turn on handle-provenance tracking now -- every
     * handle it uses is minted after this point and recorded as it is made. */
    turi_prov_enable_and_seed(env);
    return env;
}

void turi_env_free(TuriEnv *env) {
    if (!env) return;

    /* RP3: drop the spice image first. dlclose runs C-side destructors
     * (atexit handlers, etc.) so it should happen before the async
     * scheduler tears down its state -- otherwise spice destructors
     * that touch the runtime would see a half-freed env. */
    if (env->spice_image) {
        /* Gap 8: a borrowed (shared) image is owned by the prototype env -- do
         * not free it here, just drop the reference. */
        if (!env->spice_image_borrowed)
            tur_spice_image_free(env->spice_image);
        env->spice_image = NULL;
    }
    /* RP5: drop any retired images that (reload) accumulated. They
     * outlived the current image so previously-installed FFI binding
     * shims could still see their name strings. Free in LIFO order
     * (most-recent first) for symmetry with how they were pushed. */
    {
        struct TurSpiceImageNode *n = env->retired_spice_images;
        while (n) {
            struct TurSpiceImageNode *next = n->next;
            if (n->image) tur_spice_image_free(n->image);
            free(n);
            n = next;
        }
        env->retired_spice_images = NULL;
    }

    /* Debugger Phase 2: drop any attached debugger state. */
    if (env->debugger) {
        turi_debug_disable(env);
    }

    /* Phase S7: free async scheduler state */
    turi_sched_free(env);

    /* Free all per-call arenas */
    ArenaNode *node = env->eval_arenas;
    while (node) {
        ArenaNode *next = node->next;
        arena_free(&node->arena);
        free(node);
        node = next;
    }

    /* interp-collections-never-freed: reclaim interpreter-created collection
     * buffers (Vec and Set/Map) whose bare-int carriers no rc-drop path
     * reclaims.  Each `destroy` frees a raw malloc'd wrapper (a Vec's data
     * buffer, or a Set/Map box's persistent HAMT via tur_hamt_free); a
     * tombstoned node (box == NULL, from an explicit vec-free/set-free/map-free)
     * is skipped.  This must run BEFORE value_perm is freed below -- the nodes
     * live in that pool. */
    {
        TuriCollBuf *cb = env->coll_bufs;
        while (cb) {
            if (cb->box) { cb->destroy(cb->box); cb->box = NULL; }
            cb = cb->next;
        }
        env->coll_bufs = NULL;
    }

    /* turi-env-owned-value-arena-pool-plan: reclaim all escaping value payloads
     * (closures, structs, captured frames/bindings, cons cells, ...) in one
     * shot, plus the process-global fallback pool the error/rejection
     * constructors use (the env adopts it on free; single-env pattern).
     * turi-value-pool-scratch-promotion-plan: free both value regions. */
    arena_free(&env->value_scratch);
    arena_free(&env->value_perm);
    turi_val_global_pool_free();

    /* TR2.2b: the persistent elaboration session owns malloc'd scope/registry
     * storage; free it before the vector below. */
    if (env->elab_session) {
        elab_session_free((ElabSession *)env->elab_session);
        env->elab_session       = NULL;
        env->elab_session_forms = 0;
    }
    buf_free(&env->src_combined);   /* TR2.3 */
    /* TR2: the accumulated-Form vector is malloc'd (the Forms themselves live
     * in eval_arenas, already freed above). */
    free(env->acc_forms);
    env->acc_forms     = NULL;
    env->n_acc_forms   = 0;
    env->cap_acc_forms = 0;
    free(env->prelude_globals);   /* PS5 */
    env->prelude_globals   = NULL;
    env->n_prelude_globals = 0;
    free(env->acc_turns);   /* PS1 */
    env->acc_turns     = NULL;
    env->n_acc_turns   = 0;
    env->cap_acc_turns = 0;

    /* Free global bindings */
    EnvBinding *b = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        /* b->name points into sym_arena — do not free separately */
        free(b);
        b = next;
    }

    free(env->globals_ht.slots);

    /* Gap 4: free a module base dir we own (set via the setter). A directly
     * assigned (borrowed) path leaves module_base_dir_owned false. */
    if (env->module_base_dir_owned) {
        free((void *)env->module_base_dir);
        env->module_base_dir = NULL;
    }

    /* Gap 5: fire native ud finalizers in LIFO order, then free the nodes. */
    {
        NativeFinalizer *fin = (NativeFinalizer *)env->native_finalizers;
        while (fin) {
            NativeFinalizer *next = fin->next;
            if (fin->free_fn) fin->free_fn(fin->ud);
            free(fin);
            fin = next;
        }
        env->native_finalizers = NULL;
    }

    buf_free(&env->src_acc);
    symtab_free(&env->st);
    /* security-audit-plan S-5: release the handle-provenance registry. */
    turi_prov_free(env);
    arena_free(&env->sym_arena);
    free(env);
}

/* -------------------------------------------------------------------------
 * Per-embed-env peripherals (libturi-per-embed-env-and-peripherals)
 * ---------------------------------------------------------------------- */

void turi_env_reset(TuriEnv *env) {
    if (!env) return;

    /* Drop accumulated REPL/eval source so the next turi_eval starts fresh.
     * The prelude pin goes with it -- this reset drops the preloaded stdlib
     * along with everything else, so there is nothing left to pin; a caller
     * that re-preloads calls turi_env_pin_prelude again. */
    buf_truncate(&env->src_acc, 0);
    env->src_pin_len      = 0;
    env->pin_toplevel     = 0;
    env->pin_prog_items   = 0;
    env->pin_acc_forms    = 0;
    env->pin_next_line    = 0;
    env->pin_acc_turns    = 0;
    env->prior_toplevel   = 0;
    env->prior_prog_items = 0;
    env->n_acc_turns      = 0;

    /* Rebuild the globals list, keeping only native-closure bindings (the
     * builtins from turi_env_new plus the embedder's natives); free every
     * binding turi_eval installed (user defns/defs). The kept EnvBinding nodes
     * are reused as-is; their value/name pointers stay valid. */
    EnvBinding *keep = NULL;
    EnvBinding *b    = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        if (turi_value_is_native(b->value)) {
            b->next = keep;
            keep    = b;
        } else {
            free(b);
        }
        b = next;
    }
    env->globals = keep;

    /* Rebuild the hash table over the surviving bindings. */
    free(env->globals_ht.slots);
    ht_init(&env->globals_ht);
    for (EnvBinding *k = keep; k; k = k->next) ht_insert(&env->globals_ht, k);

    /* Clear transient interpreter control / unwind state. */
    env->returning    = false; env->return_value = turi_nil();
    env->throwing     = false; env->throw_value  = turi_nil();
    env->aborting     = false; env->abort_value  = turi_nil();
    env->abort_target = NULL;  env->abort_prompt_kind = 0;
    env->panicking    = false;
    env->in_no_unwind = false;
    env->handler_stack = NULL;
    env->defer_stack   = NULL;
    env->catch_jmp     = NULL;
    env->last_tc_env   = NULL;
    env->defining_mod    = NULL;
    env->current_module  = NULL;
    env->reader_type   = READER_TURMERIC;

    /* Restore step fuel to the configured limit (0 == unlimited). */
    env->step_fuel = env->step_fuel_limit;
}

void turi_env_set_diag_sink(TuriEnv *env, TuriDiagSinkFn cb, void *ud) {
    if (!env) return;
    env->diag_sink    = cb;
    env->diag_sink_ud = ud;
}

void turi_env_set_module_base_dir(TuriEnv *env, const char *path) {
    if (!env) return;
    if (env->module_base_dir_owned) {
        free((void *)env->module_base_dir);
        env->module_base_dir       = NULL;
        env->module_base_dir_owned = false;
    }
    if (path) {
        char *copy = strdup(path);
        if (!copy) return;  /* leave base dir unset on OOM */
        env->module_base_dir       = copy;
        env->module_base_dir_owned = true;
    } else {
        env->module_base_dir = NULL;  /* default (".") */
    }
}

void turi_env_register_native_ex(TuriEnv *env, const char *name,
                                  TuriNativeFn fn, void *ud,
                                  TuriNativeFreeFn free_ud) {
    if (!env) return;
    /* Install the binding via the eval-layer registrar (TuriClosure is internal
     * to eval.c). */
    turi_env_register_native(env, name, fn, ud);
    /* Gap 5: record the finalizer (if any) so turi_env_free can run it. */
    if (free_ud) {
        NativeFinalizer *fin = (NativeFinalizer *)malloc(sizeof(NativeFinalizer));
        if (!fin) return;  /* the native is still registered; only the finalizer is lost on OOM */
        fin->free_fn = free_ud;
        fin->ud      = ud;
        fin->next    = (NativeFinalizer *)env->native_finalizers;
        env->native_finalizers = fin;
    }
}

/* interp-collections-never-freed: see the header for the contract.  The node is
 * allocated from value_perm (never the rewindable value_scratch pool): the
 * coll_bufs list is walked at turi_env_free, so with scratch promotion enabled a
 * scratch-allocated node would be poisoned by arena_reset while still linked.
 * value_perm is present and freed at turi_env_free on every path. */
TuriCollBuf *turi_env_track_collection(TuriEnv *env, void *box,
                                       TuriCollBufFreeFn destroy,
                                       TuriCollBufScanFn scan) {
    if (!env || !box || !destroy) return NULL;
    /* TR3: reuse a node the sweep recycled before growing the perm pool. */
    TuriCollBuf *node = env->coll_bufs_free;
    if (node) {
        env->coll_bufs_free = node->next;
    } else {
        node = (TuriCollBuf *)turi_val_perm_alloc(env, sizeof(TuriCollBuf));
    }
    node->box     = box;
    node->destroy = destroy;
    node->scan    = scan;
    node->marked  = false;
    node->next    = env->coll_bufs;
    env->coll_bufs = node;
    return node;
}

void turi_env_untrack_collection(TuriCollBuf *node) {
    if (node) node->box = NULL;
}

void turi_env_set_interpret_mode(TuriEnv *env, bool interpret) {
    if (!env) return;
    env->interpret_mode = interpret;
}

void turi_env_set_scratch_promotion(TuriEnv *env, bool enable) {
    if (!env) return;
    env->scratch_promotion = enable;
}

void turi_env_set_incremental_elab(TuriEnv *env, bool enable) {
    if (!env) return;
    env->incremental_elab = enable;
}

void turi_env_pin_prelude(TuriEnv *env) {
    if (!env) return;
    env->src_pin_len     = env->src_acc.len;
    env->pin_toplevel    = env->prior_toplevel;
    env->pin_prog_items  = env->prior_prog_items;
    env->pin_acc_forms   = env->n_acc_forms;
    env->pin_next_line   = env->acc_next_line;
    env->pin_acc_turns   = env->n_acc_turns;
}

void turi_env_reset_to_prelude(TuriEnv *env) {
    if (!env) return;

    /* Clamp: a caller that shortened src_acc by other means (turi_env_reset)
     * must not leave the pin pointing past the end of the buffer. */
    size_t pin = (env->src_pin_len < env->src_acc.len) ? env->src_pin_len
                                                       : env->src_acc.len;
    buf_truncate(&env->src_acc, pin);

    if (pin == 0) {
        /* Nothing pinned -- the historical full discard. */
        env->prior_toplevel   = 0;
        env->prior_prog_items = 0;
        env->n_acc_forms      = 0;
        env->acc_next_line    = 0;
        env->n_acc_turns      = 0;
    } else {
        /* Rewind to the pin.  The counters come back too, so the prelude is
         * marked already-run: its definitions are still bound in env->globals
         * from the original load, and replaying the `(load ...)` forms would
         * hit the elaborator's loaded_modules dedup and register nothing. */
        env->prior_toplevel   = env->pin_toplevel;
        env->prior_prog_items = env->pin_prog_items;
        env->n_acc_forms      = env->pin_acc_forms;
        env->acc_next_line    = env->pin_next_line;
        env->n_acc_turns      = env->pin_acc_turns;
    }

    /* TR2: the elaboration session was built from forms read under the OLD
     * reader; drop it either way.  It is rebuilt by replaying the retained
     * turns (PS1: acc_turns), which for the pinned region is a re-elaboration,
     * not a re-evaluation -- prior_prog_items above is what keeps the program
     * items from running a second time. */
    if (env->elab_session) {
        elab_session_free(env->elab_session);
        env->elab_session       = NULL;
        env->elab_session_forms = 0;
    }
}

static int prelude_global_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const TuriPreludeGlobal *)a)->binding;
    uintptr_t y = (uintptr_t)((const TuriPreludeGlobal *)b)->binding;
    return (x > y) - (x < y);
}

void turi_env_snapshot_prelude(TuriEnv *env) {
    if (!env) return;
    free(env->prelude_globals);
    env->prelude_globals   = NULL;
    env->n_prelude_globals = 0;

    uint32_t n = 0;
    for (EnvBinding *b = env->globals; b; b = b->next) n++;
    if (n == 0) return;
    TuriPreludeGlobal *snap = (TuriPreludeGlobal *)malloc(n * sizeof *snap);
    if (!snap) return;   /* no snapshot: a rewind degrades to reset_to_prelude */
    uint32_t i = 0;
    for (EnvBinding *b = env->globals; b; b = b->next, i++) {
        snap[i].binding = b;
        snap[i].value   = b->value;
    }
    qsort(snap, n, sizeof *snap, prelude_global_cmp);
    env->prelude_globals   = snap;
    env->n_prelude_globals = n;
}

void turi_env_rewind_to_prelude(TuriEnv *env) {
    if (!env) return;
    turi_env_reset_to_prelude(env);
    if (!env->prelude_globals) return;

    /* Rebuild the list in its existing order: keep prelude bindings (with their
     * prelude values) and natives, free everything a turn added -- the same
     * disposal turi_env_reset uses for user bindings. */
    EnvBinding  *keep = NULL;
    EnvBinding **tail = &keep;
    EnvBinding  *b    = env->globals;
    while (b) {
        EnvBinding *next = b->next;
        TuriPreludeGlobal probe = { b, {0} };
        TuriPreludeGlobal *hit = (TuriPreludeGlobal *)bsearch(
            &probe, env->prelude_globals, env->n_prelude_globals,
            sizeof probe, prelude_global_cmp);
        if (hit || turi_value_is_native(b->value)) {
            if (hit) b->value = hit->value;
            b->next = NULL;
            *tail   = b;
            tail    = &b->next;
        } else {
            free(b);
        }
        b = next;
    }
    env->globals = keep;

    free(env->globals_ht.slots);
    ht_init(&env->globals_ht);
    for (EnvBinding *k = keep; k; k = k->next) ht_insert(&env->globals_ht, k);
}

void turi_env_apply_lang(TuriEnv *env, ReaderType reader_type) {
    turi_env_apply_lang_dialect(env, reader_type,
                                env ? env->lang : LANG_TURMERIC);
}

/* saffron-lang-plan S8: the same switch, carrying the LANGUAGE axis too.
 *
 * `turi_env_apply_lang` above only ever moved the reader, so `#lang saffron`
 * typed at the REPL prompt was accepted and its language half silently
 * dropped: the reader was already `turmeric`, the early-out fired, and
 * `(defn add [a b] ...)` kept Turmeric's `int` parameter default. A user
 * following the guide types exactly that line first.
 *
 * The dialect joins the early-out condition, so switching ONLY the language
 * still resets the session -- which it must, since accumulated source was
 * elaborated under the old defaults. */
void turi_env_apply_lang_dialect(TuriEnv *env, ReaderType reader_type,
                                 LangDialect dialect) {
    if (!env) return;
    if (reader_type == env->reader_type && dialect == env->lang) return;
    turi_env_reset_to_prelude(env);
    /* Wipe the persistent session reader-macro registry (RM Q#5).  The reason
     * is the reset, not the vanished layer axis: turi_env_reset_to_prelude
     * discards the session source, so the `#use-reader-macros` forms that
     * registered those user macros are no longer part of the session either
     * -- leaving their dispatches live would let a macro outlive the text
     * that defined it.  The built-in dispatches (`#s"..."`) are reinstalled
     * on the next read by reader_macros_install_builtins, so they survive as
     * they should.  The old entries' arena storage is env-lifetime and
     * reclaimed at teardown, matching the interpreter's process-lifetime
     * policy. */
    if (env->reader_macros)
        reader_macros_init(env->reader_macros, &env->sym_arena);
    env->reader_type = reader_type;
    env->lang        = dialect;
    /* The language's prelude joins the stdlib autoload list when the session
     * language has one -- the REPL's analogue of the entry-file switch in
     * main.c. */
    g_lang_prelude = lang_traits(dialect)->prelude;
}

void turi_env_set_shared_spice_image(TuriEnv *env, struct TurSpiceImage *image) {
    if (!env) return;
    /* Replace any image we currently OWN (a borrowed one is the prototype's to
     * free; just drop our reference to it). */
    if (env->spice_image && !env->spice_image_borrowed)
        tur_spice_image_free(env->spice_image);
    env->spice_image          = image;
    env->spice_image_borrowed = (image != NULL);
}

/* -------------------------------------------------------------------------
 * Global binding access
 * ---------------------------------------------------------------------- */

TuriValue turi_env_get(TuriEnv *env, const char *name) {
    EnvBinding *b = ht_find(&env->globals_ht, name);
    if (b) return b->value;
    return turi_errorf("unbound variable: %s", name);
}

void turi_env_set(TuriEnv *env, const char *name, TuriValue value) {
    /* Update existing binding in linked list if present. */
    EnvBinding *b = ht_find(&env->globals_ht, name);
    if (b) {
        b->value = value;
        return;
    }
    /* Create new binding, prepend to linked list, insert into hash table. */
    b         = (EnvBinding *)malloc(sizeof(EnvBinding));
    b->name   = name;
    b->value  = value;
    b->next   = env->globals;
    env->globals = b;
    ht_insert(&env->globals_ht, b);
}

EnvBinding *turi_env_find_binding(TuriEnv *env, const char *name) {
    return ht_find(&env->globals_ht, name);
}

void turi_env_rebuild_hash_table(TuriEnv *env) {
    free(env->globals_ht.slots);
    ht_init(&env->globals_ht);
    for (EnvBinding *k = env->globals; k; k = k->next) {
        ht_insert(&env->globals_ht, k);
    }
}
