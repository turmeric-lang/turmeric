#ifndef TUR_CPS_PROMPT_H
#define TUR_CPS_PROMPT_H

#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * CPS5 (cps-transform-plan): multi-prompt delimited-control machine.
 *
 * The Dybvig--Peyton-Jones--Sabry model, expressed over heap continuation
 * chains (a CPS program is a chain; this is the same shape CPS4's TurKont uses,
 * specialized here with prompt/shift markers so the machine can capture
 * sub-continuations by *slicing the chain*):
 *
 *   - reset = a prompt marker in the chain (DK_PROMPT, tag t).
 *   - shift(t, body) / shift0(t, body) capture the sub-continuation from the
 *     shift point up to the nearest enclosing prompt tagged t, reify it as a
 *     callable chain, and run body with it. shift RE-INSTALLS the prompt on the
 *     captured sub (so resuming re-delimits); shift0 does NOT (CPS0.2 table).
 *   - the captured sub is multi-shot: dk_invoke runs a fresh copy each time, so
 *     call/cc* / cloneable resume is just "invoke more than once".
 *   - an implicit ROOT prompt (DK_ROOT_TAG) lets an undelimited capture reach
 *     program entry (CPS5.3): dk_run_root treats program end as the prompt.
 *
 * Standalone machine (not yet wired into codegen). Exercised by
 * tests/cps_prompt_unit.c.
 * ========================================================================= */

#define DK_ROOT_TAG 0   /* the implicit root prompt's tag */

typedef struct DK DK;

/* A plain frame: given the value flowing in, produce the value flowing out. */
typedef intptr_t (*DKFrame)(intptr_t env, intptr_t value);

/* A shift/shift0 body: receives the captured sub-continuation (invoke it with
 * dk_invoke, possibly multiple times) and returns the value delivered to the
 * prompt's outer continuation. */
typedef intptr_t (*DKBody)(intptr_t env, DK *subk);

/* An algebraic-effect handler case, carried by a handler-prompt marker: receives
 * the effect argument and the captured sub-continuation `subk` (from the perform
 * site up to this handler), and returns the value delivered to the handler's
 * outer continuation.  A `resume` inside the case is dk_invoke(subk, v). */
typedef intptr_t (*DKHandler)(intptr_t env, intptr_t arg, DK *subk);

/* A suspending continuation frame (multi-suspension lowering, Track A): given
 * the value flowing in AND its run-time downstream chain `rest` (this node's
 * next, as spliced by dk_perform to the reinstalled handler tail), produce the
 * value.  dk_run_impl RETURNS its result rather than continuing, so the frame
 * owns delivery -- a nested control op in a lifted continuation threads `rest`
 * to find the correct enclosing handler and delivers exactly once. */
typedef intptr_t (*DKResumeFrame)(intptr_t env, intptr_t value, DK *rest);

/* ---- E3a: owning-env teardown hooks (cps-backend-owning-env-teardown) ------
 * A frame whose captured env holds an OWNING value (an rc handle, or an
 * aggregate with owning fields) carries a clone/drop pair so a multi-shot
 * resume gets its own correctly-refcounted copy instead of a shared shallow
 * alias.  Both default NULL, in which case dk_copy_node keeps today's shallow
 * env-pointer copy and dk_free never touches the env -- byte-identical to the
 * pre-E3a behavior for every Copy-only / borrow-only frame.
 *
 *   env_clone -- run on each dk_copy_node that copies an owning frame (capture
 *     via dk_copy_range, and every dk_invoke replay via dk_copy): return an
 *     owned copy of `env` (rc: incref + return the same handle; aggregate:
 *     deep-copy + clone each owning field).  Its result becomes the copy's env.
 *   env_drop  -- run on each dk_free of an owning frame: drop each owning field
 *     (rc: decref; aggregate: drop fields + free the struct).  Runs BEFORE the
 *     node itself is freed.
 *
 * Accounting (the E3a obligation): base populate = +1; each copy = +1 clone and
 * -1 drop at its dk_free; net zero once every copy AND the base chain are freed.
 * In the standalone machine the base chain is freed by its owner, so accounting
 * balances to zero; the emitted E3a path leaks only the base +1 until E3b's
 * region teardown frees the original delimited chain. */
typedef intptr_t (*DKEnvClone)(intptr_t env);
typedef void     (*DKEnvDrop)(intptr_t env);

/* ---- chain constructors (each returns a fresh heap node) -------------- */
DK *dk_done(void);
DK *dk_frame(DKFrame fn, intptr_t env, DK *next);
/* E3a: a plain frame whose env is owning -- carries the clone/drop pair fired by
 * dk_copy_node / dk_free.  Passing NULL for both is exactly dk_frame. */
DK *dk_frame_owning(DKFrame fn, intptr_t env,
                    DKEnvClone env_clone, DKEnvDrop env_drop, DK *next);
DK *dk_frame_resume(DKResumeFrame fn, intptr_t env, DK *next);
DK *dk_prompt(int tag, DK *next);
DK *dk_shift(int tag, DKBody body, intptr_t body_env, DK *next);
DK *dk_shift0(int tag, DKBody body, intptr_t body_env, DK *next);
/* A handler-prompt for algebraic effects: a marker keyed by effect `tag` that
 * carries the handler case `fn` (with env).  Transparent to a returning value
 * (like dk_prompt); a matching dk_perform runs `fn`.
 *
 * `dk_handler` is a DEEP handler: dk_perform re-installs the handler on the
 * captured sub-continuation, so a `resume` inside the case re-delimits under
 * the same handler (each subsequent perform in the resumed computation is
 * handled again).  This is the reinstall-on-resume behavior, the effect-side
 * analogue of `shift`.
 *
 * `dk_handler_shallow` is a SHALLOW handler: dk_perform does NOT re-install the
 * handler on the captured sub-continuation, so a `resume` runs the rest of the
 * computation with the handler already removed (a subsequent perform of the
 * same effect is no longer caught by this handler; it propagates to an enclosing
 * handler, or aborts as unhandled if none catches it).  This is the no-reinstall
 * behavior, the effect-side analogue of `shift0`.
 *
 * Both share the one DK substrate; the only difference is the reinstall bit
 * carried on the handler marker and read by dk_perform. */
DK *dk_handler(int tag, DKHandler fn, intptr_t env, DK *next);
DK *dk_handler_shallow(int tag, DKHandler fn, intptr_t env, DK *next);

/* ---- evaluation ------------------------------------------------------- */
/* Run chain `k` with seed value `v` (no implicit root prompt). */
intptr_t dk_run(DK *k, intptr_t v);
/* Run under an implicit root prompt: a shift to DK_ROOT_TAG with no explicit
 * enclosing prompt captures up to program entry. */
intptr_t dk_run_root(DK *k, intptr_t v);

/* Invoke a captured sub-continuation with `w` (multi-shot: copies internally). */
intptr_t dk_invoke(DK *sub, intptr_t w);

/* Perform effect `tag` (named `name`, for the unhandled-effect abort) with
 * argument `arg` against continuation `k`: find the
 * nearest enclosing dk_handler with a matching tag, reify the sub-continuation
 * from the perform point up to that handler (re-installing the handler on the
 * captured copy, for deep-handler semantics), run the handler case with (arg,
 * subk), and deliver its result to the handler's outer continuation.  Aborts on
 * an unhandled effect. */
intptr_t dk_perform(int tag, const char *name, intptr_t arg, DK *k);

/* Does the chain contain a prompt marker? (used to assert re-install). */
bool dk_has_prompt(const DK *k);

/* Does the chain contain a handler marker for `tag`? (used to assert deep vs
 * shallow re-install: a deep handler's captured sub-continuation carries the
 * re-installed handler marker, a shallow handler's does not). */
bool dk_has_handler(const DK *k, int tag);

/* ---- memory ----------------------------------------------------------- */
DK *dk_copy(const DK *k);   /* deep copy a chain */
void dk_free(DK *k);        /* free a chain */
/* Free a single node without following ->next.  Reclaims a one-off spliced node
 * (an abortive shift / perform node) whose ->next points into an enclosing
 * continuation, where dk_free would walk into (and double-free) that chain. */
void dk_free_node(DK *k);

#endif
