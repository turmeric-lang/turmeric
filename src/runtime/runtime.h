#ifndef TUR_RUNTIME_H
#define TUR_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for TypeKind from types.h */
/* We can't forward-declare enums in C, so we use int for type_tag */
typedef int TypeKindInt;

/* Forward declaration for effect row (future-proofing for v3 effects).
 * This matches the forward declaration in types.h. */
struct EffectRow;

/* A defer thunk - a function pointer that takes an env pointer and returns void.
 * 
 * Per effects-plan.md §6.10, this is the unified defer model that makes
 * the S1/S2/S3 strategy choice a runtime policy decision rather than an
 * architectural rewrite. The thunk is registered with tur_frame_push_defer
 * and invoked by tur_frame_fire_lifo at scope exit.
 * 
 * For v1, thunks take a void* env parameter to support captures.
 * For defers without captures, NULL is passed (though the thunk ignores it). */
typedef void (*defer_fn_t)(void *env);

/* Phase B2: DeferMode controls when a registered defer thunk fires.
 *   DEFER_NORMAL    — fires on normal scope exit (original behaviour).
 *   DEFER_SUSPENDED — fires when the owning continuation is suspended
 *                     (e.g. when cloneable-shift captures the frame).
 *   DEFER_REPLAY    — fires on every resume of a cloneable continuation.
 * The default mode for tur_frame_push_defer() is DEFER_NORMAL.
 * Use tur_frame_push_defer_mode() to register a defer with another mode. */
typedef enum {
    DEFER_NORMAL    = 0,
    DEFER_SUSPENDED = 1,
    DEFER_REPLAY    = 2,
} DeferMode;

/* Maximum number of defers per frame. This is a compile-time limit.
 * In practice, scopes rarely have more than a handful of defers. */
#define TUR_FRAME_MAX_DEFERS 32

/* A stack-allocated frame for tracking defers in a scope.
 * Each scope (let/defn/while body) gets one of these.
 * 
 * Per effects-plan.md §6.10.1, this is the unified defer model that makes
 * the S1/S2/S3 strategy choice (§6.10.2) a runtime policy decision rather
 * than an architectural rewrite.
 * 
 * Fields:
 *   defers:     Array of defer thunk function pointers (max TUR_FRAME_MAX_DEFERS).
 *               Thunks are registered via tur_frame_push_defer and fired in LIFO
 *               order by tur_frame_fire_lifo.
 *   envs:       Array of env pointers for defers with captures (parallel to defers).
 *               For defers without captures, the corresponding env is NULL.
 *   n:          Number of active defers in the list.
 *   parent:     Pointer to the enclosing frame (for scope unwind chain).
 *               Used by tur_frame_fire_chain to unwind through nested scopes.
 *   may_capture: Future-proofing for v3 effects (§6.10) - whether this frame may
 *                be captured by a continuation. Always false in v0/v1.
 *   effect_row: Future-proofing for v3 effects (§6.10) - effect row for this frame.
 *                NULL in v0/v1, treated as empty effect set.
 */
typedef struct tur_frame {
    defer_fn_t defers[TUR_FRAME_MAX_DEFERS];
    void *envs[TUR_FRAME_MAX_DEFERS];      /* Env pointers for captured defers */
    /* Phase B2: per-defer firing mode (DEFER_NORMAL / DEFER_SUSPENDED / DEFER_REPLAY).
     * Initialised to DEFER_NORMAL by tur_frame_push_defer().
     * tur_frame_fire_lifo() fires only DEFER_NORMAL defers; use the mode-specific
     * variants for the other modes. */
    DeferMode modes[TUR_FRAME_MAX_DEFERS];
    int n;
    struct tur_frame *parent;
    /* Future-proofing fields - unused in v0/v1 but reserved for v3 effects */
    bool may_capture;
    struct EffectRow *effect_row;
} tur_frame;

/* Initialize a frame. Should be called on stack-allocated frames.
 * parent: The enclosing frame, or NULL if this is the outermost frame. */
static inline void tur_frame_init(tur_frame *f, tur_frame *parent) {
    f->n = 0;
    f->parent = parent;
    f->may_capture = false;
    f->effect_row = NULL;
}

/* Push a defer thunk onto the frame's defer list with DEFER_NORMAL mode.
 * The thunk will be called in LIFO order when the frame is fired at scope exit.
 * env: The environment pointer for captured defers, or NULL if no captures.
 * Returns 0 on success, -1 if the frame is full (shouldn't happen in practice). */
static inline int tur_frame_push_defer(tur_frame *f, defer_fn_t thunk, void *env) {
    if (f->n >= TUR_FRAME_MAX_DEFERS) {
        return -1; /* Frame full */
    }
    f->defers[f->n] = thunk;
    f->envs[f->n] = env;
    f->modes[f->n] = DEFER_NORMAL;
    f->n++;
    return 0;
}

/* Push a defer thunk with an explicit DeferMode.
 * Use this when registering DEFER_SUSPENDED or DEFER_REPLAY defers for
 * cloneable continuation interaction. */
int tur_frame_push_defer_mode(tur_frame *f, defer_fn_t thunk, void *env, DeferMode mode);

/* Fire all DEFER_NORMAL defers in LIFO order and reset the frame.
 * This is called at scope exit (normal, return, or error).
 * DEFER_SUSPENDED and DEFER_REPLAY defers are NOT fired here. */
void tur_frame_fire_lifo(tur_frame *f);

/* Fire all DEFER_SUSPENDED defers in LIFO order (does NOT reset n).
 * Called when a cloneable continuation suspends the current scope. */
void tur_frame_fire_lifo_for_suspend(tur_frame *f);

/* Fire all DEFER_REPLAY defers in LIFO order (does NOT reset n).
 * Called on each resume of a cloneable continuation. */
void tur_frame_fire_lifo_for_replay(tur_frame *f);

/* Fire all defers (all modes) and walk up the parent chain firing each frame's defers.
 * This is used for unwinding through multiple scope levels (e.g., on return or panic). */
void tur_frame_fire_chain(tur_frame *f);

/* Phase 18: Delimited continuations.
 *
 * Two continuation representations coexist (see cps-transform-plan.md, CPS0.4 /
 * CPS6):
 *
 *   1. tur_cont / tur_cloneable_cont (BELOW) -- the original *fiber-frame
 *      snapshot* path. It copies up to TUR_CONT_MAX_CAPTURED_FRAMES native
 *      frames into a fixed array, so capture is BOUNDED: tur_cont_alloc returns
 *      NULL past the ceiling. This remains the shipping fast path for shallow
 *      delimited capture (small `reset`s), retained per CPS0.4 / OQ-CPS2.
 *
 *   2. TurKont (src/runtime/cps_rt.h) + the DK prompt machine
 *      (src/runtime/cps_prompt.h) -- the *heap-reified* CPS substrate. A
 *      continuation is a heap closure chain; capture is an O(1) pointer take
 *      and is UNBOUNDED in depth (validated at 500k frames). The
 *      TUR_CONT_MAX_CAPTURED_FRAMES ceiling does NOT apply there. Undelimited
 *      capture to the implicit root prompt uses this path.
 *
 * CPS6: the ceiling is retired *on the CPS path* (which never had it); the
 * constant below is kept solely for the retained fiber fast path. */

/* Maximum number of captured frames per continuation -- FIBER FAST PATH ONLY.
 * Does not constrain the heap-reified CPS substrate (cps_rt.h / cps_prompt.h),
 * whose capture is unbounded. */
#define TUR_CONT_MAX_CAPTURED_FRAMES 16

/* A continuation frame for delimited continuations (shift/reset).
 *
 * BOUNDED fiber-frame snapshot (see the two-model note above): captures the
 * execution state at the point of shift into a fixed `captured[]` array,
 * allowing it to be resumed later. Continuations are one-shot (move-only).
 * For unbounded capture use the heap-reified CPS substrate (cps_rt.h).
 * 
 * Fields:
 *   cont_fn:      Function pointer to the continuation trampoline.
 *   env:          Environment captured by the continuation.
 *   parent:       Parent continuation (for nested reset boundaries).
 *   n_captured:   Number of tur_frame structures captured by this continuation.
 *   captured:     Array of captured tur_frame pointers.
 *   consumed:     Whether this continuation has been resumed (one-shot enforcement).
 */
typedef struct tur_cont {
    void (*cont_fn)(void *env, int64_t value);  /* Function to call to resume */
    void *env;                                     /* Captured environment */
    struct tur_cont *parent;                      /* Parent continuation */
    tur_frame *captured[TUR_CONT_MAX_CAPTURED_FRAMES];
    int n_captured;
    bool consumed;     /* One-shot: true after resume */
    /* Phase B2: Marks this continuation as a cloneable one.  If true,
     * tur_cont_resume() will abort with a diagnostic — callers must use
     * tur_cloneable_cont_resume() instead. */
    bool is_cloneable;
} tur_cont;

/* Allocate a new continuation with captured frame chain.
 * frame_chain: Array of tur_frame pointers to capture.
 * n_frames:   Number of frames to capture.
 * Returns:     New tur_cont, or NULL on error. */
tur_cont *tur_cont_alloc(tur_frame **frame_chain, int n_frames);

/* Resume a continuation with a value. Consumes the continuation (one-shot).
 * cont:    The continuation to resume.
 * value:   The value to pass to the continuation.
 * Note:    After calling this, the continuation is marked as consumed and cannot
 *          be resumed again. */
void tur_cont_resume(tur_cont *cont, int64_t value);

/* Drop a continuation without resuming it. Fires defers on captured frames.
 * cont:    The continuation to drop.
 * Note:    This is called when a continuation is garbage collected without being resumed. */
void tur_cont_drop(tur_cont *cont);

/* Check if a continuation has been consumed.
 * cont:    The continuation to check.
 * Returns: true if the continuation has been resumed, false otherwise. */
bool tur_cont_consumed(tur_cont *cont);

/* Phase B2: Cloneable continuations */

/* A cloneable continuation can be resumed multiple times.
 * Unlike tur_cont (one-shot), tur_cloneable_cont can be cloned before use.
 * Each clone is a fresh continuation that can be resumed independently.
 * All captured environment values must implement the Clone trait.
 *
 * v1 note: Clone performs a shallow copy of env (not a deep clone).  Full
 * deep-clone support requires typeclass dispatch via Clone instances and is
 * deferred to a later phase. */
typedef struct tur_cloneable_cont {
    void (*cont_fn)(void *env, int64_t value);  /* Function to call to resume */
    void *env;                                     /* Captured environment (must be cloneable) */
    void *(*clone_env)(const void *env);         /* Deep-clone the captured env (CPS-CL6) */
    void  (*drop_env)(void *env);                /* Release the captured env (CPS-CL6) */
    struct tur_cloneable_cont *parent;           /* Parent cloneable continuation */
    tur_frame *captured[TUR_CONT_MAX_CAPTURED_FRAMES];
    int n_captured;
    bool consumed;  /* One-shot per clone: true after resume */
} tur_cloneable_cont;

/* Allocate a new cloneable continuation.
 * frame_chain: Array of tur_frame pointers to capture.
 * n_frames:   Number of frames to capture.
 * Returns:     New tur_cloneable_cont, or NULL on error. */
tur_cloneable_cont *tur_cloneable_cont_alloc(tur_frame **frame_chain, int n_frames);

/* Clone a cloneable continuation. Creates a fresh copy that can be resumed independently.
 * cont:    The continuation to clone.
 * Returns:  A new tur_cloneable_cont that is a copy of cont, or NULL on error. */
tur_cloneable_cont *tur_cloneable_cont_clone(const tur_cloneable_cont *cont);

/* Resume a cloneable continuation with a value. Consumes this clone (one-shot).
 * cont:    The cloneable continuation to resume.
 * value:   The value to pass to the continuation. */
void tur_cloneable_cont_resume(tur_cloneable_cont *cont, int64_t value);

/* Drop a cloneable continuation without resuming it. Fires defers on captured frames.
 * cont:    The cloneable continuation to drop. */
void tur_cloneable_cont_drop(tur_cloneable_cont *cont);

/* Phase R2: Panic — print message to stderr, then abort.
 * Re-entry (double panic) calls abort() immediately. */
extern int tur_panic_in_progress;
void tur_panic(const char *msg);

/* Phase R5: Panic strategy - ABORT variant for ^no-unwind */
void tur_panic_abort(const char *msg);

/* CT4: Contract handler API -- override the handler called on contract failure. */
void tur_set_contract_handler(void (*h)(const char *));
void *tur_get_contract_handler(void);

/* Phase R2: Panic with typed payload */
/* TypeKind is an enum from types.h; we use int for type_tag in runtime.h */

/* Payload carried by a panic. This is the runtime representation of a
 * typed panic value, allowing catch-panic-of to filter by type. */
typedef struct tur_panic_payload {
    TypeKindInt type_tag;   /* TypeKind of the panicked value (as int) */
    void *value;            /* Boxed payload value (heap-allocated) */
    const char *file;       /* Source file where panic occurred */
    int line;               /* Source line where panic occurred */
} tur_panic_payload;

/* Panic with a typed payload. The payload is boxed on the heap.
 * type_tag: The TypeKind of the payload (as int).
 * payload:  The boxed value (ownership transferred to tur_panic_with).
 * file:     Source file location.
 * line:     Source line location. */
void tur_panic_with(TypeKindInt type_tag, void *payload, const char *file, int line);

/* Result type for catch-unwind/catch-panic-of: holds either Ok(value) or Err(payload) */
typedef enum tur_result_tag {
    TUR_RESULT_OK,
    TUR_RESULT_ERR,
} tur_result_tag;

/* Generic result type used by catch-unwind */
typedef struct tur_result {
    tur_result_tag tag;
    union {
        int64_t ok_val;           /* For simple integer returns (Phase R2: simplified) */
        void *ok_ptr;            /* For pointer returns */
        tur_panic_payload *err;  /* For panic payloads */
    } u;
} tur_result;

/* Typed callback for thunk in catch-unwind/catch-panic-of */
typedef void (*tur_thunk_fn)(void *env, tur_result *out);

/* Catch any panic at a boundary.
 * thunk:   The function to call. Takes env pointer and result output pointer.
 * env:     Environment to pass to thunk.
 * out:     Output result struct.
 * Returns: true if a panic was caught, false if thunk completed normally.
 * Note:    This uses setjmp/longjmp internally. */
bool tur_catch_unwind(tur_thunk_fn thunk, void *env, tur_result *out);

/* Catch panics of a specific type at a boundary.
 * expected_type: Only catch panics with this TypeKind (as int).
 * thunk:        The function to call.
 * env:          Environment to pass to thunk.
 * out:          Output result struct.
 * Returns:       true if a panic of matching type was caught, false otherwise
 *               (including if a panic of different type occurred, which re-panics). */
bool tur_catch_panic_of(TypeKindInt expected_type, tur_thunk_fn thunk, void *env, tur_result *out);

/* Accessors for panic payload */
TypeKindInt tur_panic_payload_type(tur_panic_payload *p);
void *tur_panic_payload_value(tur_panic_payload *p);
const char *tur_panic_payload_file(tur_panic_payload *p);
int tur_panic_payload_line(tur_panic_payload *p);

/* Downcast a panic payload to a specific type.
 * p:         The panic payload to downcast.
 * target_type: The TypeKind to cast to (as int).
 * Returns:   The boxed value if type matches, NULL otherwise. */
void *tur_panic_payload_downcast(tur_panic_payload *p, TypeKindInt target_type);

/* CPS3: Selective CPS -- v1 identity-CPS continuation handle.
 * Wraps a function pointer so that colored functions can be called in CPS style.
 * CPS4 will extend this struct to support heap-reified continuations. */
typedef struct tur_cps_cont {
    void (*fn)(struct tur_cps_cont *k, int64_t value);
} tur_cps_cont_t;

/* Invoke the continuation with value.  No-op if k is NULL. */
void tur_cps_apply(tur_cps_cont_t *k, int64_t value);

#endif /* TUR_RUNTIME_H */
