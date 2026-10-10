#ifndef TURI_EVAL_H
#define TURI_EVAL_H

#include <stdio.h>

#include "turi/env.h"
#include "turi/fiber.h"
#include "turi/value.h"
#include "runtime/globals.h"  /* TurNativeRetType (typed native registration) */

/* ---------------------------------------------------------------------------
 * Public eval API (Phase S0)
 * --------------------------------------------------------------------------- */

/* Evaluate a Turmeric source string in the given environment.
 * Prior definitions in `env` remain visible.  New top-level definitions
 * (defn, def) are stored back into `env` for subsequent calls.
 *
 * On parse/elaboration error the diagnostic is emitted to stderr (or to this
 * env's sink, if one was installed via turi_env_set_diag_sink); the returned
 * value has tag TURI_ERROR with a short description.
 *
 * If `src` uses `(import ...)`, set the import search root FIRST with
 * turi_env_set_module_base_dir(env, dir) -- otherwise imports resolve relative
 * to the process cwd, which for an embedder is rarely the intended directory.
 *
 * The returned value is valid until `turi_env_free(env)`.  Closures hold
 * internal pointers into env-owned arenas and must not outlive env. */
TuriValue turi_eval(TuriEnv *env, const char *src);

/* Evaluate the contents of a file. */
TuriValue turi_eval_file(TuriEnv *env, const char *path);

/* Call a closure value directly with the given arguments.
 * Bypasses re-elaboration, so macros from previous elaboration calls
 * remain usable inside the closure body.  The closure must have been
 * obtained from turi_env_get() after a turi_eval_file() call.
 * Returns TURI_ERROR on arity mismatch or runtime error. */
TuriValue turi_call(TuriEnv *env, TuriValue fn, TuriValue *args, uint32_t n_args);
/* turi_call as a dynamic call site makes it: a variadic closure's surplus
 * arguments are packed into its rest chain. */
TuriValue turi_call_dynamic(TuriEnv *env, TuriValue fn, TuriValue *args, uint32_t n_args);

/* Initialise the diagnostics subsystem for standalone libturi use.
 * Call once before the first turi_eval.  `use_color` enables ANSI colour
 * in error messages; pass false when output is not a terminal. */
void turi_init(bool use_color);

/* Write a human-readable REPL representation of v into buf (at most cap
 * bytes, NUL-terminated). */
void turi_value_repr(char *buf, size_t cap, TuriValue v);

/* Like turi_eval, but also fills out_type_tag with the elaborated type name
 * of the last top-level expression (e.g. "int", "bool", "Point", "ptr<void>").
 * Passing NULL/0 for out_type_tag / tag_cap is equivalent to turi_eval. */
TuriValue turi_eval_typed(TuriEnv *env, const char *src,
                           char *out_type_tag, size_t tag_cap);

/* Evaluate source code, registering it under custom path for debugger/diagnostics. */
TuriValue turi_eval_with_path(TuriEnv *env, const char *src, const char *path);
TuriValue turi_eval_with_path_typed(TuriEnv *env, const char *src, const char *path,
                                    char *out_type_tag, size_t tag_cap);

/* Attempt to call the Show typeclass instance for val.
 * Returns a heap-allocated C string (caller must free() it), or NULL when
 * no Show instance is registered for this value's concrete type.
 * Currently handles TURI_STRUCT values; primitives fall back to NULL. */
const char *turi_try_show(TuriEnv *env, TuriValue val);

/* SI4-C: Show a heap-pointer stdlib value using type_tag from turi_eval_typed.
 * Handles TURI_INT values whose elaborated type is "Pair" or "Cons".
 * Returns a heap-allocated C string (caller must free() it), or NULL when
 * the type_tag is not a known heap-pointer type. */
const char *turi_show_result(TuriEnv *env, TuriValue val, const char *type_tag);

/* Attempt to call the Show typeclass instance for a heap-pointer (TURI_INT)
 * value whose elaborated type is a named ADT/struct/record (e.g. Vec, Set,
 * Map, or a user defstruct/defgadt).  `type_tag` comes from turi_eval_typed.
 * Lets the REPL display `(vec-of 1 2 3)` as "[1 2 3]" via the stdlib
 * Show[Vec] instance without an explicit (show ...).  Primitive and
 * ptr<void> tags are skipped (the ptr<void> Show reads any pointer as a
 * result<T,E>, which would be wrong for an arbitrary heap value).
 * Returns a heap-allocated C string (caller must free() it), or NULL. */
const char *turi_try_show_by_tag(TuriEnv *env, TuriValue val, const char *type_tag);

/* ---------------------------------------------------------------------------
 * Debugger Phase 2: interactive interpreter debugger
 * --------------------------------------------------------------------------- */

/* Attach an interactive debugger to env.  After this call the eval loop checks
 * each AST node against the breakpoint table / step predicate and, on a hit,
 * yields to a command REPL on `in` (commands) / `out` (prompts + listings).
 * Pass NULL for in/out to use stdin/stdout.  Also registers the `(break)`
 * builtin (an explicit source-driven breakpoint).  Idempotent: a second call
 * is a no-op.  The debugger starts UNARMED -- no node will stop until
 * turi_debug_arm() is called, so prelude/top-level loading runs uninterrupted. */
void turi_debug_enable(TuriEnv *env, FILE *in, FILE *out);

/* Arm the attached debugger so it stops at the first eval node it sees (the
 * program-entry stop).  Call right before running the program (e.g. main). */
void turi_debug_arm(TuriEnv *env);

/* Arm the attached debugger for breakpoints and step requests only, bypassing the
 * program-entry stop. */
void turi_debug_arm_breakpoints(TuriEnv *env);

/* Detach and free the debugger (no-op if none attached). */
void turi_debug_disable(TuriEnv *env);

/* Register the `(break)` builtin without attaching a debugger.  With no
 * debugger present it is a no-op (returns nil), so a program containing
 * `(break)` runs unchanged under plain `tur --interpret`; under `tur debug`
 * the same call forces a pause.  turi_debug_enable also registers it. */
void turi_debug_register_break_builtin(TuriEnv *env);

/* ---------------------------------------------------------------------------
 * Debugger Phase 3: control surface for the DAP server
 *
 * The Debug Adapter Protocol server (src/turi/dap.c) drives the very same
 * TuriDebugger as the Phase 2 text REPL, but through a typed control API rather
 * than a line-oriented command loop.  When the debugger pauses it invokes a
 * registered pause handler (the DAP server's stop dispatcher) instead of the
 * built-in REPL; the handler inspects frames / locals and issues one resume
 * (continue / step) via the calls below.
 * --------------------------------------------------------------------------- */

/* Why the program paused, reported to the pause handler. */
typedef enum {
    TURI_DBG_STOP_ENTRY,       /* first node after arming (stopOnEntry) */
    TURI_DBG_STOP_BREAKPOINT,  /* a line breakpoint was hit */
    TURI_DBG_STOP_STEP,        /* a step (in/over/out) predicate fired */
    TURI_DBG_STOP_PAUSE,       /* the (break) builtin forced a pause */
} TuriDbgStop;

/* One activation frame, innermost = index 0. */
typedef struct {
    const char *fn_name;     /* function owning the frame (interned; never freed) */
    const char *file_path;   /* full source path, or "" if unknown */
    uint32_t    line;        /* 1-based source line of the executing node */
    uint32_t    col;         /* 1-based source column (col_start) */
    uint32_t    end_line;    /* 1-based; == line for single-line spans */
    uint32_t    end_col;     /* 1-based, exclusive */
} TuriDbgFrame;

/* Pause handler: invoked while the eval loop is suspended at a node.  It must
 * issue exactly one resume (turi_debug_resume_*) before returning; returning
 * without one is treated as a continue. */
typedef void (*TuriDbgPauseFn)(TuriEnv *env, TuriDbgStop reason, void *ud);

/* Install (or, with NULL, clear) the pause handler.  With one set, a stop calls
 * cb(env, reason, ud) instead of the Phase 2 text REPL. */
void turi_debug_set_pause_handler(TuriEnv *env, TuriDbgPauseFn cb, void *ud);

/* Conditional-breakpoint hook.  When a line breakpoint carrying a non-empty
 * condition string matches, the debugger calls cb(env, condition, ud) and stops
 * only if it returns true.  With no hook installed, conditions are ignored
 * (the breakpoint always fires). */
typedef bool (*TuriDbgCondFn)(TuriEnv *env, const char *condition, void *ud);
void turi_debug_set_cond_handler(TuriEnv *env, TuriDbgCondFn cb, void *ud);

/* Custom breakpoint matcher callback.  If set, the debugger delegates the
 * breakpoint-hit check to this callback instead of matching against its local
 * table.  Returns true if the line is a breakpoint. */
typedef bool (*TuriDbgBpMatchFn)(TuriEnv *env, const char *file_path, uint32_t line, void *ud);
void turi_debug_set_bp_match_handler(TuriEnv *env, TuriDbgBpMatchFn cb, void *ud);

/* Breakpoint-table management for the DAP setBreakpoints request. */
void turi_debug_clear_breakpoints(TuriEnv *env);
void turi_debug_clear_breakpoints_for_file(TuriEnv *env, const char *basename);
/* Add a line breakpoint (basename match; pass "" to match any file).  `cond`
 * may be NULL / "" for an unconditional breakpoint.  Returns the 1-based
 * breakpoint id, or -1 if the table is full. */
int  turi_debug_add_breakpoint(TuriEnv *env, const char *basename, uint32_t line,
                               const char *cond);

/* Resume controls -- call exactly one from inside the pause handler. */
void turi_debug_resume_continue(TuriEnv *env);
void turi_debug_resume_step_in(TuriEnv *env);
void turi_debug_resume_step_over(TuriEnv *env);
void turi_debug_resume_step_out(TuriEnv *env);

/* Resume and stop at the next *expression*, not the next line.
 *
 * step_in and friends are line-granular, which is the granularity DAP speaks
 * and a human wants to drive.  It is the wrong one for a recorder: a line is
 * not a unit of evaluation in a Lisp, so line-granular stops collapse nested
 * calls on one line into a single stop and collapse a one-line loop body into
 * a single stop for the whole loop.  This resume stops at every located node
 * instead.  It is what turi/trace.c drives the recorder with; there is no
 * interactive command bound to it. */
void turi_debug_resume_step_node(TuriEnv *env);

/* Number of live activation frames (capped at the backtrace storage limit). */
int  turi_debug_frame_count(TuriEnv *env);
/* Fill *out for frame `idx` (0 = innermost).  Returns false if idx is out of
 * range. */
bool turi_debug_frame_at(TuriEnv *env, int idx, TuriDbgFrame *out);

/* Enumerate the locals visible in frame `idx`, innermost binding first, each
 * passed to cb as (name, value-repr, ud).  Shadowed outer bindings are
 * suppressed. */
void turi_debug_frame_locals(TuriEnv *env, int idx,
                             void (*cb)(const char *name, const char *repr,
                                        void *ud),
                             void *ud);

/* Resolve a single name in frame `idx` (locals then globals) and render its
 * value into out_repr.  Returns false if no such binding is in scope. */
bool turi_debug_eval_name(TuriEnv *env, int idx, const char *name,
                          char *out_repr, size_t cap);

/* Evaluate `src` as an arbitrary Turmeric expression in the lexical scope of
 * paused frame `idx` (0 = innermost). Renders the result into out_repr.
 * Returns false (with a short message in out_repr) on parse, elaboration, or
 * runtime error. */
bool turi_debug_eval_expr(TuriEnv *env, int idx, const char *src,
                          char *out_repr, size_t cap);

/* ---------------------------------------------------------------------------
 * SB3 / SB4: Sandbox resource-limit and capability API
 * --------------------------------------------------------------------------- */

/* Default step-fuel limit applied by turi_env_new_sandboxed (overridable at
 * compile time).  The former TURI_DEFAULT_SANDBOX_DEPTH is gone: C4
 * (turi-c-scoped-forms-heap-bounding) retired the recursion-depth guard, so a
 * sandbox bounds work via step-fuel alone. */
#ifndef TURI_DEFAULT_SANDBOX_FUEL
#  define TURI_DEFAULT_SANDBOX_FUEL  10000000u   /* 10M eval steps */
#endif

/* Set the step-fuel limit for env.  Each call to the evaluator consumes one
 * unit.  When fuel reaches 0, turi_eval returns TURI_ERROR.
 * Pass 0 to disable fuel checking (default for unrestricted environments).
 * turi_env_new_sandboxed sets a default of TURI_DEFAULT_SANDBOX_FUEL. */
void turi_env_set_fuel(TuriEnv *env, uint64_t steps);

/* C4: retained as a no-op for API/ABI compatibility.  The recursion-depth
 * guard was retired (interpreter recursion is heap-bounded); bound total work
 * with turi_env_set_fuel instead. */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth);

/* Grant a capability to an environment (no-op if already granted). */
void turi_env_allow(TuriEnv *env, TuriCaps cap);

/* Revoke a capability from an environment (no-op if already absent). */
void turi_env_deny(TuriEnv *env, TuriCaps cap);

/* Return true if the environment currently holds the given capability. */
bool turi_env_has_cap(TuriEnv *env, TuriCaps cap);

/* ---------------------------------------------------------------------------
 * Phase S7: Async C API
 * --------------------------------------------------------------------------- */

/* Register a native C function as a named global in env.
 * After this call, Turmeric code can call the function by name.
 * The global binding keeps the `name` POINTER, not a copy, so `name` must
 * outlive env: a literal, or a copy in env's sym_arena -- never a stack
 * buffer (docs/archive/r7rs-eval-host-procedure-name-on-the-stack.md). */
void turi_env_register_native(TuriEnv *env, const char *name,
                               TuriNativeFn fn, void *ud);

/* security-audit-plan WP3 (S-1): capability-checked native dispatch.
 *
 * turi_env_register_native looks `name` up in the builtin classification table
 * (src/turi/native_caps.c) and stamps the row's required capabilities on the
 * native; a call from an env that lacks any of them returns TURI_ERROR without
 * running the native.  A name with no row carries no requirement -- an
 * embedder's own native is exposed because the embedder chose to expose it.
 *
 * turi_env_register_native_caps states the requirement explicitly instead of
 * consulting the table.  Pass TURI_CAP_NONE to expose a native unconditionally
 * even when it shadows a classified builtin name. */
void turi_env_register_native_caps(TuriEnv *env, const char *name,
                                   TuriNativeFn fn, void *ud, TuriCaps required);

/* One row of the classification table: a builtin native's name and the
 * capabilities a caller must hold.  0 = pure. */
typedef struct TuriNativeCapRow {
    const char *name;
    TuriCaps    caps;
} TuriNativeCapRow;

/* The whole table, sorted by strcmp on name; *n_out receives its length. */
const TuriNativeCapRow *turi_native_cap_table(size_t *n_out);

/* The row for `name`, or NULL when the name is not a classified builtin. */
const TuriNativeCapRow *turi_native_cap_find(const char *name);

/* Render `caps` as a comma-separated list ("fs,proc") into buf; returns buf. */
const char *turi_caps_describe(TuriCaps caps, char *buf, size_t n);

/* -------------------------------------------------------------------------
 * security-audit-plan S-5: handle provenance.
 *
 * The tree-walking interpreter carries every collection / string / iterator /
 * symbol / cons / continuation handle as a bare TURI_INT holding a raw pointer,
 * and a native casts it straight back with no check that the integer came from
 * the matching constructor.  In a capability-restricted env (a sandbox, the
 * compile-time macro env) that is a forgeable wild read/write -- `(vec-get 4096
 * 0)` reads address 4096 with no capability at all.
 *
 * The fix is a per-restricted-env provenance set (docs/reported direction 1):
 * a native that MINTS a handle records (kind, pointer); a native that CONSUMES
 * one is refused unless the pointer is a live entry of the matching kind; a
 * native that FREES one removes it.  The KIND key is what keeps a count or a
 * different handle from being replayed as a vec -- `(vec-get (vec-len v) 0)` is
 * refused because a vec-len result is not a VEC handle.
 *
 * Every distinct interpreter handle representation is a kind.  Handles that
 * share a heap layout (Set and Map are both the {void* hamt} box) share a kind;
 * confusion within one layout is harmless, confusion across layouts is refused.
 * TURI_HK_GENERIC is the catch-all for a pointer handle with no more specific
 * kind (and the kind the caps-drop global seed uses, since it cannot recover a
 * global int's true kind): a forged arbitrary integer is still refused by every
 * kind, and GENERIC never satisfies a native that wants a specific kind.
 *
 * The interpreter's own value model re-tags words too -- `(:: w cstr)`, a
 * by-value struct ascription, a field read through a bare-int receiver, a
 * call through a fn-typed carrier, gen-unwrap, a TVar, a panic payload -- and
 * each of those sites checks the same registry.  The words they accept are
 * recorded where a value LOSES its tag: every closure, string, struct and
 * generator argument entering a native, and the interpreter's own stores
 * (rest-list cells, TVar payloads, set literals).  So a re-tag succeeds
 * exactly for a word that was once a value of that kind. */
typedef enum TuriHandleKind {
    TURI_HK_NONE = 0,   /* not a handle: a scalar/count/key/opaque word */
    TURI_HK_VEC,        /* native Vec box: int64_t[4] {data,len,cap,track} */
    TURI_HK_SETMAP,     /* Set/Map box: void*[2] {hamt, track} */
    TURI_HK_HAMT,       /* raw Hamt* (map-hamt result; tur_hamt_* args) */
    TURI_HK_HAMT_ITER,  /* HamtIter* */
    TURI_HK_HAMT_TRANS, /* HamtTransient* */
    TURI_HK_STRING,     /* owned String / StringBuilder (tur_string.c) */
    TURI_HK_SBUF,       /* tur_sb string builder */
    TURI_HK_SLICE,      /* tur_slice (owned slice, tur_string.c) */
    TURI_HK_SLICEBOX,   /* slice-* box: int64_t[2] {data, len} */
    TURI_HK_CONS,       /* tur/list malloc'd { head, tail } cons cell */
    TURI_HK_SEQCELL,    /* seq cons/pair/option cell (same shape, distinct API) */
    TURI_HK_CONT,       /* TuriCont* / escape boundary (continuation) */
    TURI_HK_SYM,        /* interned Symbol* carried as :Sym */
    TURI_HK_JSON,       /* json node handle */
    TURI_HK_GENARR,     /* gen-arr generic-array handle */
    TURI_HK_GRID,       /* TuriGridRep* */
    TURI_HK_MUTMAP,     /* TurMmWrap* mutable map */
    TURI_HK_BTCELL,     /* backtracking / logic-var cell (bt-*, g-*) */
    TURI_HK_FUTURE,     /* WkFutureCell* */
    TURI_HK_CHAN,       /* WkChan* (chan-*, schan-*, async-chan-*) */
    TURI_HK_MUTEX,      /* pthread_mutex_t* */
    TURI_HK_BYTES,      /* bytes-* int64-prefixed buffer */
    TURI_HK_REACTOR,    /* epoll/kqueue reactor handle */
    TURI_HK_CMP,        /* carrier comparator C fn-ptr (mk-cmp / keyeq) */
    TURI_HK_GENERIC,    /* a pointer handle with no more specific kind */
    TURI_HK_CLOSURE,    /* a TuriClosure* that crossed into a native, where an
                         * int64 carrier can drop its tag (turi_prov_note_args) */
    TURI_HK_CSTR,       /* a NUL-terminated string riding the int64 carrier: a
                         * TURI_CSTR that lost its tag (a native argument, a
                         * rest-list cell) or a string a native handed back as a
                         * bare word.  Also the kind of a native position that
                         * READS a C string, where a tagged TURI_CSTR is fine */
    TURI_HK_STRUCT,     /* a TuriStruct* riding the carrier: a by-value struct /
                         * record ADT that lost its tag the same ways */
    TURI_HK_GENBOX,     /* &TuriGen::box -- the ptr<void> gen-next hands out and
                         * gen-unwrap reads the yielded word through */
    TURI_HK_TVAR,       /* TuriTVar* -- an STM cell (tvar/new) */
    TURI_HK_PANIC,      /* TuriPanicPayload* -- a caught panic's err payload */
    TURI_HK_RESULTBOX,  /* raw int64_t[3] {is_ok, ok, err} Result box (catch-
                         * unwind, result-collect, ...) */
    TURI_HK_MMSTORAGE,  /* TurMmStorage* -- a MutableMap's slot table, the word
                         * `(.storage m)` reads out of its { storage } wrapper */
    TURI_HK_OPTIONBOX,  /* raw int64_t[2] {is_some, value} Option box (json/get) */
    TURI_HK_BTSTREAM,   /* WkBtCell {value, next} -- a backtracking stream cell
                         * (bt-cons / mreturn / mplus / mbind); NOT a trail
                         * TurBtCell, which is TURI_HK_BTCELL */
    TURI_HK_RESPAIR,    /* int64_t[2] {ok_vec, err_vec} (result-partition) */
    TURI_HK_SCHEMA,     /* tur_sch_t int64_t[4] {kind, a, b, c} schema node */
    TURI_HK_SCHERRS,    /* schema-decode error vector {data, len, cap} */
    TURI_HK_SCHERR,     /* one schema error record int64_t[3] {path, msg, val} */
    TURI_HK_R7IO,       /* r7rs_io* -- an R7RS port buffer */
    TURI_HK_R7IDTAB,    /* r7rs_idtab* -- an R7RS identity table */
    TURI_HK_R7KCONT,    /* R7kCont* -- an R7RS call/cc stack image */
    TURI_HK_ARGCELL,    /* *args* cell {char *value, next} -- a host-built
                         * cons whose head is a trusted C string */
    TURI_HK_IDENTITY,   /* int64_t[1] {value} -- the Identity comonad cell */
    TURI_HK_PAIR,       /* WkTuple2 {e1, e2} -- the env-pair comonad cell */
    TURI_HK_SIZEDBUF,   /* TuriSizedBufRep {len, data} -- sized-buf */
    TURI_HK_MOCKTIME,   /* int64_t[1] {now_ms} -- Mock-Time */
    TURI_HK_TASKGROUP,  /* WkTaskGroup* -- structured-concurrency task group */
    TURI_HK_GEN,        /* TuriGen* -- the word behind a TURI_GEN generator */
    TURI_HK_OWNED_CSTR, /* a malloc'd char* the caller may free(3) */
    TURI_HK_SEQVEC,     /* seq-out-vec int64_t[3] {data, len, cap} -- NOT a
                         * gen-arr, which is {len, cap, data} (GENARR) */
    TURI_HK__COUNT
} TuriHandleKind;

/* Flags on a native's handle signature. */
enum {
    TURI_HSIG_MINT = 1u << 0,  /* result is a freshly-minted handle -> register */
    TURI_HSIG_FREE = 1u << 1,  /* the handle args are freed -> unregister */
    /* A dual-representation native: a tagged TURI_STRUCT at one of its handle
     * positions is read through the struct API (turi_struct_field), never cast
     * to the handle's layout, so the guard admits it.  Without the flag a
     * struct there is refused -- its header would be read as the handle. */
    TURI_HSIG_STRUCT_OK = 1u << 2,
    /* The same for a tagged TURI_CSTR: the native reads a string there (a name
     * that may arrive as a string or as a :Sym word, say), so it is admitted
     * at a position whose kind is not TURI_HK_CSTR. */
    TURI_HSIG_CSTR_OK = 1u << 3,
};

/* How many leading argument positions a handle signature describes.  The
 * guard treats a position past this as a non-handle, so it must cover the
 * widest handle-taking native: map-assoc-eq's comparator is its fifth
 * argument, and with 4 positions it was called through unchecked. */
#define TURI_HSIG_MAX_ARGS 6

/* One row of the handle-signature table: which leading args are pointer handles
 * (and of what kind), what kind the result is, and mint/free behaviour. */
typedef struct TuriNativeHandleRow {
    const char    *name;
    uint8_t        arg[TURI_HSIG_MAX_ARGS];  /* TuriHandleKind per arg, 0 = none */
    uint8_t        result;                   /* TuriHandleKind of result, 0 = none */
    uint8_t        flags;                    /* TURI_HSIG_MINT | TURI_HSIG_FREE */
} TuriNativeHandleRow;

/* The whole handle-signature table (sorted by name), and a binary-search find.
 * A name with no row consumes/produces no handles. */
const TuriNativeHandleRow *turi_native_handle_table(size_t *n_out);
const TuriNativeHandleRow *turi_native_handle_find(const char *name);

/* Guard: before a provenance-tracked native runs, verify each of its handle
 * arguments is a live handle of the declared kind.  Returns true and sets *out
 * to a refusal error when a forged handle is found; false to let the call
 * proceed. */
bool turi_prov_guard_native(TuriEnv *env, const TuriNativeHandleRow *sig,
                            const TuriValue *args, uint32_t n, TuriValue *out);

/* Track: after the native ran, register a freshly-minted handle result and
 * forget freed handle arguments, per the signature. */
void turi_prov_track_native(TuriEnv *env, const TuriNativeHandleRow *sig,
                            const TuriValue *args, uint32_t n, TuriValue result);

/* Low-level registry ops (also used by the cont-builtin guard and the caps-drop
 * global seed).  register/forget are no-ops when provenance is off. */
void turi_prov_register(TuriEnv *env, TuriHandleKind kind, const void *ptr);
void turi_prov_forget(TuriEnv *env, const void *ptr);   /* all kinds for ptr */
bool turi_prov_check(TuriEnv *env, TuriHandleKind kind, const void *ptr);

/* A closure, string or struct argument entering a native can come back out as
 * a bare int64 -- stored in a Vec, a map, a cell -- so register each one under
 * its kind there (TURI_HK_CLOSURE / _CSTR / _STRUCT).  No-op when provenance is
 * off. */
void turi_prov_note_args(TuriEnv *env, const TuriValue *args, uint32_t n);

/* The same for one value that drops its tag outside a native call (a rest-list
 * cell built by the interpreter itself).  No-op when provenance is off. */
void turi_prov_note_value(TuriEnv *env, TuriValue v);

/* Re-tag an int64 carrier as the C string it holds.  In a provenance-tracked env
 * the word must be a string that lost its tag (TURI_HK_CSTR, or a GENERIC
 * pre-restriction global), else the result is a TURI_ERROR: an erasing
 * ascription `(:: w cstr)` would otherwise hand any integer to strlen.  0 stays
 * the NULL cstr it always was. */
TuriValue turi_cstr_from_carrier(TuriEnv *env, int64_t w);

/* The most one native allocation may ask for in a provenance-tracked env.
 * Step fuel meters evaluation, not a native's own work, so a single
 * (vec-new-filled n x) could otherwise take the host's memory. */
#define TURI_PROV_MAX_ALLOC_BYTES ((uint64_t)256 << 20)

/* May a native allocate `count` elements of `elem` bytes?  Always true
 * outside a provenance-tracked env. */
bool turi_prov_alloc_ok(TuriEnv *env, int64_t count, size_t elem);

/* For a C callback a native hands words to -- the HAMT key comparators, which
 * compare STORED keys as well as the caller's: is `w` a live handle of kind k
 * in the provenance-tracked env whose native is running on this thread?  True
 * when no such native is running (provenance off).  A CSTR word also passes as
 * a GENERIC pre-restriction global, like everywhere else. */
bool turi_prov_word_live(TuriHandleKind k, int64_t w);

/* Re-tag an int64 carrier as the closure it holds.  In a provenance-tracked env
 * the word must be a closure some native was handed (TURI_HK_CLOSURE), else
 * the result is a TURI_ERROR: an erasing ascription or a carrier-taking native
 * would otherwise turn a caller integer into a call target.  0 stays 0. */
TuriValue turi_closure_from_carrier(TuriEnv *env, int64_t w);

/* Turn provenance tracking on for a restricted env, seeding it from the current
 * globals (their pointer-carrying values become GENERIC handles) so a handle
 * minted before the caps dropped is not mistaken for a forgery.  Idempotent. */
void turi_prov_enable_and_seed(TuriEnv *env);

/* Record (live) or forget the three R7RS standard-port buffers as R7IO
 * handles in a provenance-tracked env.  They are made while the R7RS prelude
 * loads, before a restricted env's capabilities drop, so turi_env_allow
 * records them when I/O is granted and turi_env_deny forgets them when it is
 * taken away: a cap-free port write reaches stdout only with TURI_CAP_IO. */
void turi_r7rs_std_ports_prov(TuriEnv *env, bool live);

/* Release the provenance registry (called from turi_env_free). */
void turi_prov_free(TuriEnv *env);

/* Like turi_env_register_native, but also records the Turmeric type the
 * native's TuriValue result carries at runtime (`ret`).  Without this, the
 * elaborator types every interpreter-mode native call -- and any defn wrapping
 * it -- as :int, so a curated facade declaring an honest :float / :cstr / :bool
 * / opaque return type failed elaboration (TUR-E0707/E0708).  Registering the
 * return type lets both a direct call and a typed wrapper see the right type.
 * The signature registry is process-global (it must be readable by the
 * elaborator, which has no env at the eval-mode call site), so a later
 * registration of the same name -- on any env -- replaces the recorded type.
 * Pass TUR_NRT_INT for the historical untyped behavior.  See
 * docs/archive/history/untyped-native-registration-blocks-curated-facades.md. */
void turi_env_register_native_typed(TuriEnv *env, const char *name,
                                    TuriNativeFn fn, void *ud,
                                    TurNativeRetType ret);

/* Gap 5 (libturi-per-embed-env-and-peripherals): finalizer for a native's user
 * data, fired from turi_env_free.  See turi_env_register_native_ex. */
typedef void (*TuriNativeFreeFn)(void *ud);

/* Gap 5: like turi_env_register_native, but `free_ud` (when non-NULL) is invoked
 * with `ud` from turi_env_free, so a native registered with `ud = a per-script
 * object` can let that object's lifetime ride along with the env.  Finalizers
 * fire in LIFO order at teardown and are NOT fired by turi_env_reset (natives
 * survive a reset).  Registering the same name more than once enqueues a
 * finalizer per call -- register a given (ud, free_ud) pair once.  When no
 * finalizer is needed, plain turi_env_register_native is the right call: a
 * native whose `ud` outlives the env (a process-global, or a default native)
 * must NOT take a finalizer.  Default natives (turi_register_default_native)
 * are shared across every env, so they intentionally have no finalizer hook. */
void turi_env_register_native_ex(TuriEnv *env, const char *name,
                                  TuriNativeFn fn, void *ud,
                                  TuriNativeFreeFn free_ud);

/* Gap 7: set this env's interpret-mode bit (see TuriEnv.interpret_mode).  An
 * embedder running interpret-mode eval wants `true` (the default); one driving
 * compile-mode elaboration through the same process wants `false`.  The bit is
 * installed into the process-global elaborator flag for the duration of each
 * turi_eval on this env, then restored, so two co-resident embedders no longer
 * clobber each other's mode.  (The elaborator still reads a process-global
 * within a single eval; full per-env threading remains future work.) */
void turi_env_set_interpret_mode(TuriEnv *env, bool interpret);

/* Register eval-layer native builtins (struct-aware predicates, etc.). */
void turi_eval_register_builtins(TuriEnv *env);

/* ---------------------------------------------------------------------------
 * Per-embed-env peripherals (libturi-per-embed-env-and-peripherals)
 *
 * Helpers for embedders that create one TuriEnv per attached script and need
 * the embed surface to scale beyond a single shared env.
 * --------------------------------------------------------------------------- */

/* Gap 1: a single native (name + fn + user data) to install on a new env.
 * Gap 5: `free_ud` is an optional finalizer for `ud`, fired from turi_env_free
 * when this spec is installed via turi_env_new_with_natives (NULL = none).
 * Older positional initializers `{ name, fn, ud }` leave it NULL, which keeps
 * the prior "ud must outlive the env" contract -- opt in only when the env
 * should own `ud`. */
typedef struct TuriNativeSpec {
    const char  *name;
    TuriNativeFn fn;
    void        *ud;
    TuriNativeFreeFn free_ud;
} TuriNativeSpec;

/* Gap 1: register a native that every SUBSEQUENT turi_env_new / turi_env_new_*
 * call installs automatically, so an embedder shipping a fixed set of natives
 * seeds them once instead of re-registering on every per-script env.  `name` is
 * copied; re-registering the same name replaces the prior spec.  Calling this
 * does not retroactively affect envs that already exist. */
void turi_register_default_native(const char *name, TuriNativeFn fn, void *ud);

/* Like turi_register_default_native, but also records the native's runtime
 * return type in the process-global signature registry (see
 * turi_env_register_native_typed) so the elaborator types calls to it -- and
 * curated typed wrappers over it -- correctly instead of defaulting to :int. */
void turi_register_default_native_typed(const char *name, TuriNativeFn fn,
                                        void *ud, TurNativeRetType ret);

/* Gap 1: drop every default-native registration. */
void turi_clear_default_natives(void);

/* Gap 1: like turi_env_new, but also installs the given natives table on the
 * fresh env (after the default-natives registry).  A NULL/0 table is the same
 * as turi_env_new. */
TuriEnv *turi_env_new_with_natives(const TuriNativeSpec *specs, size_t n);

/* Gap 2: reset an env to a from-scratch interpreter state -- clear all globals
 * that turi_eval installed (user defns/defs), the accumulated source, the
 * deferred/handler/return/throw/abort control state, and any in-flight catch
 * boundary -- while KEEPING every registered native (the builtins installed by
 * turi_env_new and the embedder's own natives) alive.  This is what a script
 * `_reload` wants: re-run the new source over a clean slate without tearing the
 * env down and re-registering natives (which turi_env_free would force).
 *
 * Note: any non-native definitions a prelude installed (e.g. interpreted stdlib
 * defns loaded via turi_eval_file) are dropped too -- re-eval the prelude after
 * a reset.  Arena memory from prior evals is reclaimed at turi_env_free, not
 * here; reset is a logical reset of bindings + control state, not a heap purge.
 * Call between top-level eval cycles, not from inside an async/handler frame. */
void turi_env_reset(TuriEnv *env);

/* Gap 3: per-env diagnostic sink.  When set, this env's parse/elaboration
 * diagnostics are delivered to `cb` (as structured single-line records) instead
 * of stderr for the duration of each turi_eval / turi_eval_typed / turi_eval_file
 * call on this env.  `level` matches DiagLevel (0=error,1=warning,2=note,3=help);
 * `code` is the "TUR-E0001"-style string or "" ; `file` is the source path or "";
 * `line`/`col` are 1-based (0 when unavailable).  Pass cb == NULL to clear.
 * TuriDiagSinkFn is declared in turi/env.h (where the env field lives). */
void turi_env_set_diag_sink(TuriEnv *env, TuriDiagSinkFn cb, void *ud);

/* Gap 4: set the base directory used to resolve `(import ...)` paths.  Must be
 * called before turi_eval on source that imports modules, otherwise imports
 * resolve relative to the process cwd (rarely what an embedder wants).  `path`
 * is copied; pass NULL to restore the default (".").  This is the supported way
 * to configure env->module_base_dir. */
void turi_env_set_module_base_dir(TuriEnv *env, const char *path);

/* Resolve `(import ...)` the way `tur run <path>` would for a program whose
 * source lives at `path` (a file; a notebook or script the embedder is about
 * to evaluate).  The module base dir becomes path's directory, and the extra
 * search dirs become the enclosing spice's src/, every `:spices` dep's src/
 * that is on disk, and the src/ of each workspace sibling -- the walk-up the
 * per-file commands do.  Outside any `build.tur` only the base dir is set.
 * The env owns what this builds (turi_env_free frees it); a later call
 * replaces it, and NULL clears both.  Returns the number of extra dirs found,
 * or -1 on allocation failure.  Survives turi_env_reset.
 * TURI_HAS_SEARCH_PATH_FOR lets an embedder that must also build against an
 * older libturi fall back to turi_env_set_module_base_dir.
 * See docs/archive/notebook-eval-no-module-base-dir.md */
#define TURI_HAS_SEARCH_PATH_FOR 1
int turi_env_set_search_path_for(TuriEnv *env, const char *path);

/* Let source evaluated on `env` use `(import ...)` at the top level, outside
 * any defmodule -- the interactive-session model a REPL or a notebook wants,
 * where one turn imports and the next calls what it referred.  `:refer`,
 * `:as` and `:for-macros` all work, and a referred name stays bound for the
 * rest of the session.  Off by default: a program's imports belong to its
 * defmodule, as they do when the same file is compiled.  `tur repl` turns it
 * on.  Defined alongside TURI_HAS_SEARCH_PATH_FOR. */
void turi_env_set_toplevel_imports(TuriEnv *env, bool on);

/* Load the standard library into a fresh `env` the way `tur --interpret`
 * does for a program: the core macros (`when`/`cond`/`for`/...), the typed
 * collections (Vec, Map, Set, Option, Result, ...) and the interpreter's
 * natives for stdlib functions whose bodies are inline C.  A bare
 * turi_env_new env has only the elaborator builtins, so a module that names
 * `(Vec float)` does not elaborate in it.  (Not the REPL's Show slice, and
 * not json/schema, which `--interpret` adds on top.)  The stdlib directory is
 * $TUR_STDLIB_DIR when set, else `stdlib_root`, else a cwd-relative `stdlib`;
 * when TUR_STDLIB_DIR is unset and `stdlib_root` is given, it is exported as
 * TUR_STDLIB_DIR, which a module import's stdlib fallback reads.  A program
 * `tur build`s can bake its root in with the autolink hint
 * `-DNAME=@TUR_STDLIB_ROOT@`.  Call once, before the first turi_eval.
 * Defined alongside TURI_HAS_SEARCH_PATH_FOR. */
void turi_env_preload_stdlib(TuriEnv *env, const char *stdlib_root);

/* Run a spice's compiled code for the calls the interpreter cannot make.  The
 * interpreter does not execute inline-C bodies beyond a few simple shapes, so
 * a session that imports a spice whose functions are inline C (most native
 * bindings, numeric kernels) can name them but not call them.  This builds
 * the spice at or above `spice_root` as a shared library (`tur build --shared`,
 * cached under `<root>/.tur-repl-cache/` and rebuilt when a source is newer),
 * loads it, and binds each export the FFI layer can marshal -- scalars,
 * :cstr, pointers, `defopaque` handles and by-value records -- as a native
 * under its module-qualified name, and under its bare name too unless
 * something (a stdlib function, a definition of the session's) already holds
 * that.  Importing the module afterwards keeps the compiled export, whatever
 * its body: a Turmeric wrapper over private inline-C helpers runs compiled
 * too.  A definition the session makes itself still wins.  An export with a
 * record slot is callable once its module is imported (the import supplies
 * the record's layout).
 *
 * The HOST must export its symbols to the image (link with `-rdynamic`, CMake
 * ENABLE_EXPORTS): the image resolves the runtime it shares with libturi
 * (tur_string_release, ...) against the executable.
 * `tur_bin` NULL means $TUR_BIN, else `tur` on PATH.  The env owns the image.
 * Returns the number of exports bound, 0 when that spice is already attached,
 * or -1 after printing why the build or load failed -- once: the env records
 * the failure and answers -1 without rebuilding for the rest of its life.
 * See docs/reported/notebook-cells-cannot-call-inline-c-spices.md */
#define TURI_HAS_ATTACH_SPICE 1
int turi_env_attach_spice(TuriEnv *env, const char *spice_root, const char *tur_bin);

/* turi_env_attach_spice for the spice that provides `module_name` (e.g.
 * "stats/dist"): the module is looked up where an import would find it,
 * the stdlib aside -- the base dir, then the extra search dirs.  Returns 0
 * when it is not found, not inside a spice, or already provided by an image
 * the env holds.  An embedder calls it for each module a turn imports, before
 * evaluating the turn. */
int turi_env_attach_spice_for_module(TuriEnv *env, const char *module_name,
                                     const char *tur_bin);

/* Internal: true when v is a native-closure binding (used by turi_env_reset to
 * tell embedder/builtin natives from turi_eval-created defns). */
bool turi_value_is_native(TuriValue v);

/* Phase R2: raise a catchable interpreter panic (recoverable by catch-unwind,
 * with the standard message + double-panic guard).  Used by native functions
 * such as result-must / option-must instead of _exit(1).  Does not return. */
void turi_runtime_panic(TuriEnv *env, const char *msg);

/* security-audit-plan S-5: call immediately before any exit()/_exit()/abort()
 * a native makes on the program's behalf (an out-of-bounds index, a failed
 * contract).  In an env that may not end the host (no TURI_CAP_PROC) it does
 * not return: turi_eval stops the program and returns TURI_ERROR
 * "panic: <msg>".  Otherwise it returns and the caller exits as before, so
 * unrestricted output is unchanged. */
void turi_host_exit_guard(TuriEnv *env, const char *msg);
/* r7rs-lang-plan R8: the value an identity question should compare.  A widen
 * to `any` of a payload that cannot answer for its own type (a Vec, a Map, an
 * opaque) wraps it in a FRESH one-field box each time, so two widens of one
 * vector are two different structs; this looks through the box. */
TuriValue turi_any_identity_payload(TuriValue v);

/* r7rs-lang-plan T5: the interpreter's side of a re-entrant continuation (the
 * R7RS prelude's call/cc copies the C stack; see eval.c).  `turi_cont_pin`
 * records a capture: frames and per-call temporaries made before it are
 * never freed (an image may complete their activations again), while those
 * made after the last capture are handed back as usual (the capture epoch);
 * a capture saves the env's dynamic-extent fields, the evaluator's boundary
 * stacks and the drivers' heap work stacks, and a re-entry puts them back
 * (after the stack image is restored). */
typedef struct TuriContState TuriContState;
void           turi_cont_pin(void);
/* Free the heap work stacks of the drives a re-entry is about to abandon:
 * those registered below `top`, the top of the image being restored. */
void           turi_cont_release_drives(const void *top);
TuriContState *turi_cont_state_capture(TuriEnv *env);
void           turi_cont_state_restore(TuriEnv *env, const TuriContState *s);
/* The per-form prompt (r7rs-toplevel-reentry-reruns-forms): the drive the
 * current top-level form runs under, and the boundary a capture stops its
 * drive snapshot at.  `set` returns the previous boundary. */
void          *turi_cont_drive_mark(void);
void          *turi_cont_set_drive_boundary(void *b);

/* Run the cooperative event loop until all async fibers and timers complete. */
void turi_run_event_loop(TuriEnv *env);

/* Spawn a new async task from Turmeric source; returns TURI_FUTURE value.
 * src must evaluate to a zero-argument closure or a direct expression. */
TuriValue turi_task_spawn(TuriEnv *env, const char *src);

/* Cancel a task future (marks owner fiber as cancelled, rejects future). */
void turi_task_cancel(TuriEnv *env, TuriFuture *f);

/* Non-blocking poll of a future; returns result/error or TURI_NIL if pending. */
TuriValue turi_future_poll_val(TuriFuture *f);

/* Start an async sleep for ms milliseconds; returns TURI_FUTURE → nil. */
TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms);

/* DEPR-D0: turi_native_throw deleted with the (throw)/(try)/(catch) front
 * end; no callers remain.  See docs/archive/history/throw-deprecation-plan.md. */

/* Fire all remaining top-level/module-level deferred actions.
 * Call after turi_call(main) to honour module-level (defer ...) forms. */
void turi_run_pending_defers(TuriEnv *env);

/* W1b: read field `idx` of a struct VALUE as a TuriValue.  Lets the
 * Result/Option native shims (in main.c, where TuriStruct is incomplete) accept
 * a make-struct TuriStruct in addition to their native int64 box.  Sets *found
 * to true and returns the field when v is a TURI_STRUCT with idx in range; sets
 * *found to false and returns nil otherwise. */
TuriValue turi_struct_field(TuriValue v, uint32_t idx, bool *found);

/* collection-multiword-element-boxing: generic content comparator for two
 * struct/ADT KEY carriers (TuriStruct pointers as int64).  A multi-word struct
 * Map key / Set element's MapKey `mk-cmp` :turi branch returns this function's
 * address (via the `struct-key-cmp` native), making it a stampable
 * bool(int64,int64) C fn ptr so structural Eq[Map]/Eq[Set] recover it -- the
 * interpreter analogue of the compiled runtime's tur_hamt_box_key_eq. */
bool turi_struct_key_eq_c(int64_t a, int64_t b);

/* Companion generic content hash for a struct/ADT key value: a `Hash` :turi
 * branch returns `(struct-hash p)` (the `struct-hash` native) so the interpreter
 * hash is uniform per struct.  Deterministic within a run; equal-content keys
 * hash equally. */
int64_t turi_struct_hash_c(TuriValue v);

/* Returns the constructor/struct name of a TURI_STRUCT value (e.g. "PureFree"),
 * or NULL when v is not a struct.  Lets natives in main.c dispatch on an ADT
 * constructor without the opaque TuriStruct layout. */
const char *turi_struct_name(TuriValue v);

/* Builds a TURI_STRUCT carrying constructor `name` and `n` fields (copied).
 * `name` must be a stable string (a literal or interned symbol); the value
 * matches a `(name ...)` pattern by name, so natives can return an ADT value
 * (e.g. a Left/Right) without the opaque TuriStruct layout. */
TuriValue turi_make_struct(TuriEnv *env, const char *name, TuriValue *fields, uint32_t n);

/* SEQ: advance a generator VALUE (carrier holds the TuriGen*) one step; returns
 * the yielded value and sets *done to 1 when the generator just exhausted.
 * Lets the seq inline-C natives (main.c) drive a TURI_GEN. */
TuriValue turi_gen_advance_val(TuriEnv *env, TuriValue gen, int *done);
/* True if the generator value has run off its end (or is null). */
bool turi_gen_done_val(TuriValue gen);

#endif /* TURI_EVAL_H */
