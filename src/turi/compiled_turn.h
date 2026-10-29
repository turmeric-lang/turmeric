/* compiled_turn.h -- C2 of docs/upcoming/aot-compiled-repl-plan.md, step 1.
 *
 * Under --enable=compiled-repl a REPL turn is not evaluated by the
 * tree-walker.  It is elaborated against the session as before -- the
 * session's rules, diagnostics and result type -- and then compiled: the
 * session's definition table (the latest source of every top-level
 * definition, and its imports) plus a `main` that runs the turn's
 * expressions and prints the last one the way the REPL would, `=> 128`.
 * The whole program goes through the same compiler as `tur jit`, and runs.
 *
 * The program's own globals live in host slots (step 2, tur_repl_slot
 * below), so a `def`'s value and its mutations carry from turn to turn and
 * its initializer runs only when the slot is fresh; each turn's program stays
 * resident, since a slot may point into it.
 *
 * Like C1 (inline_c_jit.h) this half is engine-free and lives in tur_core;
 * the compile-and-run is the host's hook, so a build with no engine has no
 * hook and every turn stays interpreted. */
#ifndef TUR_TURI_COMPILED_TURN_H
#define TUR_TURI_COMPILED_TURN_H

#include "turi/env.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct Form;
struct Type;
struct Expr;

/* One entry of the definition table. */
typedef struct TuriCompiledDef {
    char *key;    /* "defn f", "definstance Show Pt", "import m", ... */
    char *text;   /* the form's source */
    char *type;   /* a def's value type, printed; NULL for anything else */
    bool  is_import;
} TuriCompiledDef;

typedef struct TuriCompiledTurnHook {
    /* Compile `src` -- a complete Turmeric program with a `main` -- in
     * process and run it.  Its output goes to stdout and stderr.  Returns 0
     * when it ran, with main's status in *out_rc; -1 when it did not compile
     * (the compiler's or c2mir's diagnostics are printed). */
    int (*run)(const char *src, size_t len, int *out_rc);
} TuriCompiledTurnHook;

/* Install the host's compile-and-run.  NULL uninstalls. */
void turi_set_compiled_turn_hook(const TuriCompiledTurnHook *hook);

/* True when a hook is installed. */
bool turi_compiled_turn_available(void);

/* True when this env's next turn compiles: the session is in compiled mode, a
 * hook is installed, and the turn is plain Turmeric (another #lang or reader
 * stays interpreted). */
bool turi_compiled_turn_applies(const TuriEnv *env);

/* Run one turn: forms[first, n) are the turn's new forms, spans into `src`,
 * and items[0, n_items) the program items they elaborated to.  `last_type`
 * is the elaborated type of the turn's last item, NULL when it has none;
 * `last_is_def` says the last item is a definition.  Returns nil (with
 * env->last_turn_printed set when the program printed the result), or an
 * error value when the program did not compile or exited non-zero; the
 * definition table changes only on success.
 *
 * A turn that redefines a type drops every def whose value has that type
 * (the type printed, as elaborated when the def was made): its slot holds
 * the old layout, and its source may now mean something else -- `(S 1)` is a
 * partial application once S grows a field.  The REPL says which. */
TuriValue turi_compiled_turn_run(TuriEnv *env, struct Form **forms,
                                 uint32_t first, uint32_t n, const char *src,
                                 struct Expr *const *items, uint32_t n_items,
                                 const struct Type *last_type, bool last_is_def);

/* Drop the definition table and the global slots (turi_env_free, :reset). */
void turi_compiled_defs_clear(TuriEnv *env);

/* Step 2: a compiled turn's top-level `def` lives in a host slot, not in its
 * program, so its value -- and a mutation of it -- carries to the next turn.
 * The emitter (g_emit_repl_slots) binds each user global to the slot this
 * returns for its Turmeric name and runs the initializer only when *fresh is
 * set: the name is new, its type or size changed (a new slot), or the turn
 * redefines it (the same slot, initialized again).  Slots are never freed
 * while a session runs: a function from an earlier turn may still read one. */
void *tur_repl_slot(const char *name, const char *typesig, size_t size, int *fresh);

/* The turn's program renders its result through this, piece by piece, into
 * env->last_turn_text: the host shows it -- `tur repl` as its `=> ` line, the
 * page as the prompt's result -- after the program's own output. */
void tur_repl_result_piece(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* TUR_TURI_COMPILED_TURN_H */
