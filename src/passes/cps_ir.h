#ifndef TUR_CPS_IR_H
#define TUR_CPS_IR_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "expr.h"
#include "arena.h"
#include "types.h"

/* =========================================================================
 * CPS2 (cps-transform-plan): A-normal-form / CPS intermediate representation.
 *
 * This is the representation the selective-CPS lowering (CPS3) targets. It is
 * produced ONLY for colored (may-capture) functions; uncolored functions keep
 * their direct-style Expr tree untouched. The IR is intentionally small:
 *
 *   - Atoms are trivial values (variables and literals): no atom ever needs to
 *     be reduced, which is exactly the ANF invariant.
 *   - Every non-trivial subexpression is named by a `let`-style binder
 *     (CT_LETPRIM / CT_LETCALL / CT_LETVAL), so evaluation order is explicit.
 *   - The current continuation is reified as a CKont and threaded through:
 *     a tail position becomes either an application of the continuation
 *     (CT_APPCONT, "(k v)") or a tail call that passes the continuation on
 *     (CT_TAILCALL, "f(args, k)").
 *   - The function's return continuation `k` has type cont<T> (CPS2.3), where
 *     T is the function's result type kind (KK_RET carries it).
 *
 * The IR is dump-only at CPS2 (exposed via --dump-cps); it is not yet wired
 * into codegen. CPS3 consumes it.
 * ========================================================================= */

/* ---- Atoms: trivial (already-evaluated) values ------------------------- */
typedef enum CAtomKind {
    CA_VAR,       /* reference to a source binding or a CPS-introduced var */
    CA_CVAR,      /* reference to a CPS-introduced result var (by id+name) */
    CA_INT,       /* integer literal */
    CA_BOOL,      /* boolean literal */
    CA_UNIT,      /* nil / unit literal */
    CA_STR,       /* string literal (cstr) */
    CA_FLOAT,     /* float / double literal (Tier B) */
    CA_OTHER,     /* a trivial value we don't model precisely */
} CAtomKind;

typedef struct CAtom {
    CAtomKind     kind;
    TypeKind      ty;          /* type kind of the value */
    const Type   *type;        /* full type when known (for carrier-ABI ADT detection); may be NULL */
    const Binding *var;        /* CA_VAR */
    uint32_t      cvar_id;     /* CA_CVAR */
    const char   *cvar_name;   /* CA_CVAR */
    int64_t       i;           /* CA_INT */
    bool          b;           /* CA_BOOL */
    StrSlice      str;         /* CA_STR */
    double        f;           /* CA_FLOAT */
} CAtom;

/* ---- Continuations ---------------------------------------------------- */
typedef enum CKontKind {
    KK_RET,       /* the function's return continuation parameter `k : cont<T>` */
    KK_VAR,       /* a local continuation variable introduced by CT_LETCONT */
    KK_PROMPT,    /* the value delivered to the nearest delimited prompt (reset) */
    KK_LOOP,      /* cps-while-native: transform-internal marker -- a body tail in
                   * this position is the loop back-edge (lowered to CT_CONTINUE);
                   * never reaches emission. */
} CKontKind;

typedef struct CKont {
    CKontKind kind;
    uint32_t  id;     /* KK_VAR id, or the result-type kind for KK_RET */
    TypeKind  ty;     /* result type kind the continuation accepts */
} CKont;

/* ---- Terms ------------------------------------------------------------ */
typedef enum CTermKind {
    CT_APPCONT,      /* (kont atom)                : deliver atom to a continuation */
    CT_LETVAL,       /* let x = atom in body       : trivial rebind */
    CT_LETPRIM,      /* let x = op(atoms...) in body */
    CT_LETCALL,      /* let x = f(atoms...) in body : direct (uncolored) call */
    CT_TAILCALL,     /* f(atoms..., kont)          : colored tail call, threads kont */
    CT_LETCONT,      /* letcont j(x) = jbody in body : a join point */
    CT_IF,           /* if atom then t else e */
    CT_RESET,        /* reset: bind x = <delimited body's value> in body */
    CT_SHIFT,        /* shift: capture current cont as k', run body to the prompt */
    CT_HANDLE,       /* handle: run delim under an effect handler; body is the continuation */
    CT_PERFORM,      /* perform: bind x = perform(effect, args), continue body */
    CT_AWAIT,        /* F3 (cps-async): bind x = await(fut), continue body -- lowered
                      * to a dk_shift against the entry prompt, capturing `body` +
                      * the outer continuation as a heap continuation the reactor
                      * resumes.  Structurally mirrors CT_PERFORM. */
    CT_RESUME,       /* resume: bind x = resume(k, v) [= dk_invoke], continue body */
    CT_LETRAW,       /* bind x = <direct-emitted owning-value op>, continue body.
                      * The RHS is a source Expr (rc/of, rc/drop, ...) emitted by
                      * the direct emitter (emit_value); the owning value stays a
                      * local and never crosses a DK slot. See emit_cps_ir.c. */
    CT_CLONEABLE,    /* U3 Shape 1: (cloneable-reset (cloneable-shift receiver val))
                      * with a TRIVIAL (identity) continuation -- alloc an identity
                      * cloneable_cont, call receiver, bind reset value, continue. */
    CT_CALLCC,       /* (call/cc f) / (escape f): a LOCAL setjmp escape landing --
                      * bind x = the call/cc value (f's normal return, or the value
                      * an upward tur_escape_resume delivers), continue body.  Does
                      * NOT thread the DK continuation.  Native for a capture-free
                      * receiver f; a capturing receiver still delegates via LETRAW. */
    CT_LOOP,         /* cps-while-native: a `while` with an interior control op,
                      * lowered to a synthesized tail-recursive colored `__cps`
                      * helper.  The ^mut loop-carried vars are the helper params;
                      * the body is a CT_IF(cond, iter, exit) whose iter arm ends in
                      * a CT_CONTINUE back-edge and whose exit arm delivers the
                      * live-after var to KK_RET.  The interior handle lowers as a
                      * normal CT_HANDLE inside the body; its continuation carries the
                      * back-edge, which is why a same-function join is impossible and
                      * the loop must be a real recursive fn (see
                      * docs/archive/cps-while-loop-with-interior-handle-no-native-lowering.md). */
    CT_CONTINUE,     /* the CT_LOOP back-edge: re-enter the loop helper with the
                      * next-iteration argument atoms (the pre-created `$next` CVars
                      * a `set!` binds), threading the helper's own continuation. */
    CT_MATCH,        /* B4: a `match` on a heap-ADT scrutinee -- an N-way tag
                      * dispatch (each arm binds its ctor fields and delivers its
                      * CPS body to the match continuation, like CT_IF's arms). */
    CT_UNSUPPORTED,  /* a source form outside the CPS2 subset (carries a reason) */
} CTermKind;

typedef struct CTerm CTerm;

/* U3 Shape 2: one context frame around a cloneable-shift, reified as a DK frame.
 * Two frame kinds, distinguished by which of `op` / `call_fn` is set:
 *   - Arithmetic frame (`op` != NULL, `call_fn` == NULL): `(<op> <operand> [])`
 *     for `op` in "+"/"-"/"*"/"/"; `operand` is the other (int-atom) operand
 *     captured into the frame env; `hole_left` is true iff the shift is the left
 *     operand.
 *   - Call frame (`call_fn` != NULL, `op` == NULL): a call to a top-level
 *     uncolored int-returning fn.  `ignore_value` distinguishes two forms:
 *       * false -- a 1-arg hole call `(f [])`: the hole is the sole argument
 *         (no captured env, env passed as 0), `hole_left` true.
 *       * true  -- a 0-arg ignore-value tail `(f)` from a do-sequence: the frame
 *         runs `f()` on resume regardless of the resumed value (no env).
 * Only an arithmetic frame carries a captured env operand; call frames pass env
 * 0.  Frames are stored outermost-first (matching the dk_frame push order). */
typedef struct CloneFrame {
    const char    *op;
    const Binding *call_fn;
    bool           ignore_value;
    CAtom          operand;
    /* A captured call-frame env that is not a simple atom (e.g. a `(mk-rec ...)`
     * constructor) -- the emitter emit_value's it at the reset site instead of
     * reading `operand`.  NULL for atomic / no-env frames.  `operand.type` still
     * carries the env's type for the serial marshal-kind decision. */
    const Expr    *env_expr;
    bool           hole_left;
} CloneFrame;

/* U3 Shape 2 (let-bearing context): one pure `let` binding sitting in the
 * cloneable context spine.  `init` is a shift-free scalar expression evaluated
 * once at the reset site (direct-emitted); `binding` names the C local so the
 * captured frame operands that reference it resolve.  Recorded outermost-first
 * (source order). */
typedef struct CloneLet {
    const Binding *binding;
    const Expr    *init;
} CloneLet;

/* One clause of a (possibly multi-case) handle: an effect and its handler body,
 * binding the effect's params + the resumable continuation `k`. */
typedef struct CHandleCase {
    const Symbol   *effect;
    const Binding **params;
    uint32_t        n_params;
    const Binding  *k;
    CTerm          *case_body;
    /* cps-dk-multishot-user-effects (Phase A): this case handles a RESUMABLE-PAYLOAD
     * effect (its constructor has a `(fn [effect-cont] R)` param) and resumes
     * through the payload.  Selects the DK-backed cloneable-cont wrap for `k` PLUS
     * the boxed-payload `arg` reap at emit (generalizes the `is_shift_effect`
     * gate), so the payload's `(k v)` / `resume` resumes the DK chain through
     * `tur_cloneable_cont_resume`.  Scoped to resumable-payload effects -- NOT set
     * for a hand-written `^multishot` handler on a non-payload effect (whose `arg`
     * is not a boxed pointer and must not be reaped as one). */
    bool            resumable_payload;
} CHandleCase;

/* B4: one arm of a restricted `match` on a heap-ADT scrutinee.  `ctor` selects
 * the arm (its ->tag is tested against the scrutinee's tag word; ->adt gives the
 * C aggregate name); `fields` are the pattern's field bindings extracted from
 * the scrutinee (via adt_field_member_path); `body` is the CPS-translated arm
 * body, delivered to the match continuation.  A catch-all (wildcard / bare var)
 * arm has ctor == NULL and is emitted as the trailing `else`. */
typedef struct CMatchArm {
    const struct CtorDef *ctor;
    const struct Binding **fields;
    uint32_t              n_fields;
    CTerm                *body;
} CMatchArm;

typedef struct CVar {     /* a CPS-introduced binder */
    uint32_t    id;
    const char *name;
    TypeKind    ty;
    const Type *type;      /* full type when known (carrier-ABI ADT detection); may be NULL */
    /* When this binder stands for a source Binding (a `let`-bound name), the
     * emitter must name it via name_for_binding so it matches every reference
     * site (which resolve through name_for_binding).  NULL for a fresh binder. */
    const Binding *bind;
} CVar;

struct CTerm {
    CTermKind kind;
    union {
        struct { CKont kont; CAtom v; }                                   appcont;
        struct { CVar x; CAtom v; CTerm *body; }                          letval;
        struct { CVar x; const char *op; const BuiltinSpec *spec; CAtom *args; uint32_t n; CTerm *body; } letprim;
        struct { CVar x; const Binding *fn; CAtom *args; uint32_t n; CTerm *body;
                 /* RC2 (generic-show-wrapper-cps-monomorphization-plan): the source
                  * EX_CALL, retained so the CPS emitter can run the same per-ABI-spec
                  * typeclass re-resolution (emit_reresolve_method_call) the direct
                  * emitter uses -- the CTerm otherwise drops the dispatch dict_arg,
                  * leaving a carrier-erased `(show x)` baked to the int rep. NULL for
                  * synthetic calls with no source Expr. */
                 const Expr *call_expr;
                 /* sum-closure-payload-never-dropped (the CPS half): when this
                  * call is the init of a `let` binding -- `(some <closure>)` --
                  * the EX_LET and the binding's index, so the emitter can ask
                  * the direct emitter's question (let_binding_sum_closure_
                  * freeable) and reap the live arm's closure at the entry
                  * boundary.  NULL otherwise. */
                 const Expr *sum_let; uint32_t sum_let_idx; } letcall;
        /* fn_atom is the callee key when fn == NULL (E2c: a via_registry call
         * whose callee is a struct-field fn-value load `(.f obj)`, not a named
         * binding).  The emitter uses fn_atom's atom_str as the `__tur_cps_lookup`
         * key in that case. */
        struct { const Binding *fn; CAtom fn_atom; CAtom *args; uint32_t n; CKont kont; bool via_registry;
                 /* E2 (fat-closure fn-value threading): the callee `fn` is a
                  * poly-fn PARAM whose runtime value is a `tur_poly_fn_t` fat
                  * closure.  Dispatch through its `fn_cps` DK-threading slot when
                  * populated (an effectful fn-value), else the direct `fn.fn`
                  * call delivered to the continuation.  Single int arg only (the
                  * tur_poly_fn_t.fn_cps ABI is `(void*, int64_t, DK*)`). */
                 bool via_fncps;
                 /* RC2: source EX_CALL retained for per-ABI-spec typeclass
                  * re-resolution in the CPS emitter (see letcall.call_expr). */
                 const Expr *call_expr; } tailcall;
        struct { CVar j; CVar param; CTerm *jbody; CTerm *body; }         letcont;
        struct { CAtom cond; CTerm *then_; CTerm *else_; }                if_;
        struct { CVar x; CTerm *delim; CTerm *body; }                     reset;
        /* shift0 == true lowers to dk_shift0 (does NOT reinstall the prompt);
         * false is a plain shift (dk_shift, prompt stays installed). */
        struct { CVar k; CTerm *body; bool shift0; }                      shift;
        /* handle: delim = body threading the handler prompt; body = the handle's
         * continuation; cases = the N handler clauses (each delivered by return),
         * one per handled effect.  Each case's k / params are bound in its body.
         * shallow == true (from `handle-shallow`, F2) lowers each case to
         * dk_handler_shallow (NOT re-installed on resume, the effect-side analogue
         * of shift0); false is a plain deep dk_handler. */
        /* B3 part 2: `dyn` marks a DYNAMIC first-class with-handler -- the handler
         * cases are not statically known; `dyn_table` is the runtime
         * tur_handler_table_t* value (a variable / field read).  The emitter
         * installs the handler group via dk_hgroup_from_table(dyn_table, ...)
         * instead of the static per-case chain, and emits no case fns
         * (they were emitted at the handler literal's creation site).  n_cases is
         * 0 for a dyn handle. */
        struct { CVar x; CTerm *delim; CTerm *body;
                 CHandleCase *cases; uint32_t n_cases; bool shallow;
                 bool dyn; CAtom dyn_table; }                             handle;
        /* B4: `scrut` is the heap-ADT carrier atom; `adt` the ADT def (for the C
         * aggregate name / tag word); `arms` the ctor arms (last may be a
         * ctor==NULL catch-all).  Each arm delivers its body to the enclosing
         * continuation (tail) or to a join point (bind), like CT_IF.
         *
         * cps-match-on-builtin-sum-evicts: when `direct` is set the match is
         * any other shape (an Option / Result scrutinee, literal arms, `any`
         * narrowing) and the DIRECT emitter dispatches it -- `direct` is the
         * source match over the scrutinee atom, whose pattern tests and
         * binders it emits -- handing each arm body back through its arm hook
         * (EmitCtx.match_tail), where arms[i].body is emitted.  `adt` is NULL
         * then, and each arm's `fields` are the binders its pattern declares. */
        struct { CAtom scrut; const struct AdtDef *adt;
                 CMatchArm *arms; uint32_t n_arms;
                 const struct Expr *direct;
                 /* The direct dispatch's guards read these enclosing
                  * variables (each arm's own binders excluded): the capture
                  * and mutation walks visit them like the scrutinee. */
                 CAtom *guard_vars; uint32_t n_guard_vars; }              match;
        struct { const Symbol *effect; CAtom *args; uint32_t n;
                 CVar x; CTerm *body; bool resumable_payload; }           perform;
        /* F3 await: fut = the awaited future atom; x = the awaited value binding;
         * body = the continuation.  Emitted as a dk_shift whose body is the fixed
         * __tur_await_body runtime helper (resume-if-ready / park-if-pending). */
        /* fresh_fut: the awaited expression is a spawn written at the await,
         * `(await (async ...))`, so nothing else names the future and the
         * await owns it (emit_await marks it with __tur_await_own; the reader
         * that takes its value frees it). */
        struct { CAtom fut; CVar x; CTerm *body; bool fresh_fut; }       await;
        struct { CAtom k; CAtom v; CVar x; CTerm *body; }                 resume;
        /* reap_env: e is a freeable, provably-non-escaping capturing closure
         * whose heap fat-env the direct emitter does NOT free at this leaf
         * position (only emit_value(EX_LET) applies the scoped-env free).  When
         * set, emit_letraw registers the bound env pointer for a single-node free
         * at the outermost DK entry boundary (safe: the closure is dead after its
         * lifted body, and boundary reap never double-frees). */
        /* reap_any_env: the same registration for an `any` binder that holds a
         * fresh capturing closure (cps_any_closure_env_freeable): the env is
         * the tagged value's payload, so it is untagged first. */
        /* reap_at_backedge (closure-let-in-self-tail-loop-leaks): with
         * reap_env, the binder is only ever INVOKED -- never an argument, not
         * even to a parameter that keeps nothing -- so a self tail call in the
         * main body may free it before jumping back rather than leave it to
         * the boundary.  An argument of that very call would be the next
         * turn's live parameter. */
        struct { CVar x; const Expr *e; CTerm *body; bool reap_env;
                 bool reap_any_env; bool reap_at_backedge; }               letraw;
        /* U3 cloneable (multi-shot).  `receiver` is a named, uncolored top-level
         * fn called with the fresh cloneable_cont handle; its result is the reset
         * value bound to x; then run body.
         *   n_frames == 0: Shape 1 -- identity continuation, no dk_copy_range.
         *   n_frames >= 1: Shape 2 -- an arithmetic context `(<op> <operand> ...
         *     [])` around the shift (outermost-first), each frame reified as a DK
         *     frame so the captured continuation deep-clones (dk_copy_range).
         * Optional context enrichers (both fall through to delegation otherwise):
         *   lets / n_lets: pure `let` prelude bindings emitted as C locals at the
         *     reset site before the frame operands (which may reference them).
         *   if_cond != NULL: a single `if` branch point in the context.  The
         *     shift-bearing arm rides the frame chain; the pure arm (if_pure) is
         *     direct-emitted on the other branch.  if_when true => the shift arm is
         *     the `then` arm (C test `if (cond)`), false => `if (!(cond))`. */
        /* `serial` selects the marshalable (serial-reset) lowering instead of the
         * multi-shot (cloneable-reset) one: same frame chain, but the frames use
         * the shared tagged marshaler (`__sk_frame_for_tag`) and the shift body
         * hands the receiver the copied DK chain directly (no cloneable_cont wrap),
         * so the captured continuation round-trips through save-cont!/resume-cont!.
         * The native serial path currently covers only arithmetic-frame contexts
         * (n_frames >= 1, no lets/if/call frames); everything else delegates. */
        /* receiver: a named uncolored top-level fn (the common case).
         * receiver_expr (U7): a CLOSURE receiver emitted as a value -- its thunk
         * is called with (closure-env, cont) instead of a bare fn ptr.  Exactly
         * one of receiver / receiver_expr is set.  Currently a closure receiver is
         * admitted for Shape 1 (n_frames == 0) only; Shape 2 keeps delegating. */
        /* recv_outward (serial-receiver-effect-cannot-reach-enclosing-handler):
         * the named receiver is colored and an effect ESCAPES it, so it cannot
         * run under the shift body's fresh root.  The reset is lowered as an
         * ordinary colored call instead -- `x = receiver(frames)` with the rest
         * lifted as its continuation -- so the effect walks out through the
         * rest of the enclosing function into the handlers around the reset.
         * The continuation handed over is the one the shift would capture: the
         * context frames over a fresh prompt. */
        struct { CVar x; const Binding *receiver; const Expr *receiver_expr;
                 bool serial; bool recv_outward;
                 CloneLet *lets; uint32_t n_lets;
                 CloneFrame *frames; uint32_t n_frames;
                 /* Count of leading `frames` that sit OUTSIDE the `if` branch point
                  * (collected before it during the outside-in context walk).  The
                  * shift arm rides the whole `frames` chain; the pure arm (if_pure)
                  * must re-apply exactly these outer frames -- frames[n_outer_frames..)
                  * are inside the shift-bearing arm and do NOT apply to the pure arm.
                  * 0 when there is no `if` or no outer frames. */
                 uint32_t n_outer_frames;
                 const Expr *if_cond; const Expr *if_pure; bool if_when;
                 CTerm *body; }                                           cloneable;
        /* (call/cc f) / (escape f): `e` is the original EX_CALLCC expr (the
         * receiver f = e->as.callcc_.fn is emitted as a value; the call/cc result
         * type = e->type).  The receiver is capture-free (a named fn or a
         * zero-capture fn value), so no env rides the escape landing. */
        struct { CVar x; const Expr *e; CTerm *body; }                    callcc;
        /* cps-while-native.  params/inits: the loop-carried ^mut vars (each param
         * CVar carries its source Binding so every in-body read of the var names
         * the param via name_for_binding -- reads resolve to the loop-ENTRY version
         * by naming, no rebinding map).  body: CT_IF(cond, iter, exit).  result_kont
         * is the caller's continuation kind (KK_RET / KK_PROMPT) -- the emitter uses
         * it only to pick the threaded continuation for the entry call; the exit arm
         * itself delivers to the helper's own KK_RET. */
        struct { CVar *params; uint32_t n_params; CAtom *inits;
                 CTerm *body; CKont result_kont; }                        loop;
        struct { CAtom *args; uint32_t n; }                               cont_;
        struct { const char *why; }                                       unsupported;
    } as;
};

/* Translate one colored function body into CPS. Returns NULL if `fd` has no
 * body. The result is a CTerm delivering the body's value to KK_RET. */
CTerm *cps_ir_translate_fn(Arena *a, Expr *program, FnDef *fd);

/* How many fresh binders the last cps_ir_translate_fn call minted: they are
 * named `__t0` .. `__t<count-1>`, which are also the direct emitter's
 * fresh_tmp names, so an emitter rendering that term must mint its own
 * temporaries from `count` up. */
uint32_t cps_ir_last_fresh_count(void);

/* Pretty-print a CPS term. */
void cps_ir_print(const CTerm *t, FILE *out, int indent);

/* --dump-cps: color `program`, then translate and print every colored
 * user-level top-level function in CPS form. */
void cps_ir_dump_program(Arena *a, Expr *program, FILE *out);

/* E2a: fn-value PARAM bindings whose effectful tail calls thread the DK. */
void cps_ir_thread_param_reset(void);
/* Forget the callee_fndef binding -> FnDef table (a new classification). */
void cps_ir_callee_cache_reset(void);
void cps_ir_thread_param_add(const Binding *param);
/* cps-local-fn-alias-or-lambda-called-in-place-refused (a value use as well
 * as calls): a `let`-bound local holding a fresh capturing lambda that is
 * threadable -- it flows to a thread param, so its env-taking `__cps` twin
 * is registered -- and is ALSO called through the local.  Such a call
 * threads the caller's continuation through the registry, keyed on the env
 * box's slot 0 (the lifted entry), like an empty-row call through a fat
 * thread param; a direct call would run the lambda's perform from a fresh
 * root (`unhandled effect`).  Registered per function by the emitter. */
void cps_ir_thread_local_reset(void);
/* `env_boxed`: the local's value is the closure's env box (slot 0 the lifted
 * entry) -- a capturing lambda -- rather than the direct entry itself. */
void cps_ir_thread_local_add(const Binding *local, bool env_boxed);
bool cps_ir_thread_local_has(const Binding *local);
bool cps_ir_thread_local_env_boxed(const Binding *local);
bool cps_ir_thread_param_has(const Binding *param);
bool cps_ir_param_call_threads(const Binding *p, const Expr *call);

/* E2 (fat-closure fn-value threading): the `tur_poly_fn_t.fn_cps` slot's ABI is
 * `int64_t (*)(void *env, int64_t a0, ..., struct DK *)` -- one int64 word per
 * argument, up to this many.  cps_ir_fncps_sig_ok says whether a fn of type
 * `fn_ty` fits it: every argument a scalar -- an integer of any width, a
 * `bool`, a float, a `cstr` or a `ptr<void>` -- carried as its word
 * (cps_ir_fncps_word_of / cps_ir_fncps_value_of), and a result of the same
 * kinds or unit.  The poly-wrap that FILLS the slot (emit_expr.c) and the
 * analysis that relies on it being filled (arg_fat_has_fn_cps) ask this one
 * question, so the two cannot drift apart. */
#define CPS_FNCPS_MAX_ARGS 8
bool cps_ir_fncps_sig_ok(const Type *fn_ty);
/* The C spelling of an argument (or result) kind cps_ir_fncps_sig_ok admits. */
const char *cps_ir_fncps_arg_ctype(TypeKind k);
/* A value of kind `k` spelled as the int64 word that crosses the slot, and back
 * (malloc'd C expressions).  A float crosses as its bits -- the DK slot's
 * Tier-B convention, so a result delivered by the callee's `__cps` entry reads
 * back the same way -- a pointer through intptr_t, anything else by a cast. */
char *cps_ir_fncps_word_of(TypeKind k, const char *value);
char *cps_ir_fncps_value_of(TypeKind k, const char *word);
/* The same for a capturing lambda, given its LIFTED type (the env parameter
 * first): a closure's slot is a dispatcher on the env box's slot 0, which holds
 * the lifted entry itself only for an `int`/`int64` or unit result (a narrow
 * result such as `bool` is widened there by a wrapper the registry does not
 * know), so those are the results it admits. */
bool cps_ir_fncps_closure_sig_ok(const Type *lifted_ty);

/* cps-let-alias-of-effectful-fn-param-refused: the immutable fn-valued
 * PARAMETER that binding `i` of `let` only renames -- `(let [f g] ...)` with
 * every use of `f` a saturated call -- or NULL.  The translator inlines each
 * such call as a call through the parameter and drops the binding, and the
 * threading classifier (ptc_walk) counts it as one: one answer for both. */
const Binding *cps_ir_let_fnparam_alias(const Expr *let, uint32_t i);
/* The same for an immutable local holding a GLOBAL fn -- named, or a lifted
 * captureless lambda -- every use of which is a saturated call: the global,
 * or NULL. */
const Binding *cps_ir_let_global_fn_alias(const Expr *let, uint32_t i);
/* A captureless `letrec` member (global, its lifted lambda in source_binding):
 * the lambda a call through it calls, or NULL. */
const Binding *cps_ir_letrec_member_target(const Binding *f);
/* An immutable local bound to a fresh CAPTURING lambda, every use of which is
 * a saturated call: the lifted lambda (a call through the local is a call to
 * it with the env box first), or NULL. */
const Binding *cps_ir_let_local_closure(const Expr *let, uint32_t i);

#endif
