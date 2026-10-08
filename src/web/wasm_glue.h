#ifndef TURI_WASM_GLUE_H
#define TURI_WASM_GLUE_H

/* Version string for the WASM module */
#define TURMERIC_VERSION "0.63.7"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Initialization and lifecycle
 * ---------------------------------------------------------------------------
 */

/* Initialize the Turmeric runtime for WASM.
 * Must be called once before any other turi_wasm_* function.
 * Returns 0 on success, non-zero on failure. */
int turi_wasm_init(void);

/* Reset the evaluation environment to a fresh state.
 * This clears all definitions and returns to a clean slate. */
void turi_wasm_reset(void);

/* Rewind the session to the preloaded stdlib, keeping the env: everything a
 * Run or prompt line defined is forgotten -- source, elaboration session, and
 * runtime bindings -- and the stdlib is not reloaded.  What the Run button does
 * before running the editor's program (playground-session-hygiene-plan PS5). */
void turi_wasm_rewind_to_prelude(void);

/* Shutdown the Turmeric WASM runtime.
 * Frees all resources. Call turi_wasm_init() again to restart. */
void turi_wasm_shutdown(void);

/* Get the version of the Turmeric runtime.
 * Returns a static string - do not free. */
const char *turi_wasm_version(void);

/* ---------------------------------------------------------------------------
 * Evaluation functions
 * ---------------------------------------------------------------------------
 */

/* Evaluate a Turmeric source string and return the result as a string.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 * 
 * Returns:
 *   A malloc'd string containing the result representation, or NULL on error.
 *   The caller is responsible for freeing this with turi_wasm_free_string().
 * 
 * On parse/runtime error, the error message is returned as the result string.
 */
char *turi_wasm_eval(const char *input);

/* Evaluate a Turmeric source string and return both result and any error.
 * 
 * Arguments:
 *   input - The Turmeric source code to evaluate (NUL-terminated)
 *   out_result - Output pointer for the result string (malloc'd, NULL on error)
 *   out_error - Output pointer for the error string (malloc'd, NULL on success)
 * 
 * Returns:
 *   0 on success (result in *out_result, *out_error is NULL)
 *   1 on error (error message in *out_error, *out_result is NULL)
 * 
 * Caller must free both *out_result and *out_error with turi_wasm_free_string().
 */
int turi_wasm_eval_ex(const char *input, char **out_result, char **out_error);

/* Evaluate multiple expressions in sequence.
 * 
 * Arguments:
 *   inputs - Array of NUL-terminated Turmeric source strings
 *   count - Number of inputs in the array
 *   outputs - Pre-allocated array of char* with count elements
 *             Each element will be set to a malloc'd result string
 * 
 * Returns:
 *   The number of successfully evaluated expressions.
 *   Caller must free each non-NULL output string with turi_wasm_free_string().
 */
int turi_wasm_eval_batch(const char **inputs, int count, char **outputs);

/* ---------------------------------------------------------------------------
 * Formatting
 * ---------------------------------------------------------------------------
 */

/* Format a Turmeric source string.
 *
 * Arguments:
 *   input - The Turmeric source code to format (NUL-terminated)
 *
 * Returns:
 *   A malloc'd string with the formatted output, or NULL on parse error.
 *   The error message (if any) is written to the printErr callback.
 *   Caller must free the result with turi_wasm_free_string().
 */
char *turi_wasm_format(const char *input);

/* ---------------------------------------------------------------------------
 * Memory management
 * ---------------------------------------------------------------------------
 */

/* Free a string allocated by turi_wasm_eval, turi_wasm_eval_ex, or
 * turi_wasm_eval_batch. */
void turi_wasm_free_string(char *s);

/* ---------------------------------------------------------------------------
 * Reader language mode
 * ---------------------------------------------------------------------------
 */

/* Set the reader language mode for subsequent evaluations.
 *
 * Arguments:
 *   name - the `#lang` directive tail: a base dialect name ("turmeric",
 *          "turmeric/curly-infix", "turmeric/neoteric", "turmeric/sweet",
 *          "saffron" and its readers, or the legacy alias "sweet-exp"), and
 *          nothing else -- `#lang` takes one base.
 *
 * Returns:
 *   0 on success, 1 if the base is not recognised or anything trails it.
 *
 * When the base changes the accumulated session source is cleared (same
 * behaviour as typing '#lang ...' in the interactive REPL).
 * This function is equivalent to including a '#lang <name>' line at the top
 * of a submitted code block and is useful for programmatic mode-switching
 * (e.g. a language selector in the web REPL UI).
 */
int turi_wasm_set_lang(const char *name);

/* Return the current reader language name as a static string.
 * The returned pointer must NOT be freed. */
const char *turi_wasm_get_lang(void);

/* Return the `#lang` registry (the base dialects) as a JSON string:
 *
 *   {"bases":[{"name":"turmeric","label":"S-expression",
 *              "language":"turmeric","experiment":null},...]}
 *
 * Built live from the C-side base axis (lang_base_at) so a UI rendering it
 * never becomes a second source of truth.  The returned pointer is owned by
 * the module; do NOT free it. */
const char *turi_wasm_lang_registry(void);

/* ---------------------------------------------------------------------------
 * Doc lookup (D6: autodoc bridge)
 * ---------------------------------------------------------------------------
 */

/* Look up the documentation string for a Turmeric stdlib name.
 *
 * Arguments:
 *   name - NUL-terminated function/macro/struct name to look up
 *
 * Returns:
 *   A static (not malloc'd) C string with the documentation, or NULL if the
 *   name is not found in the doc table.  Do NOT free the returned pointer.
 *
 * Checks the builtin/special-form table, then reads the auto-generated
 * stdlib/docstrings.tur table directly from C (src/turi/docstrings.c).  It never
 * evaluates anything, so a lookup cannot change the session.
 *
 * Exported with EMSCRIPTEN_KEEPALIVE so it is callable from JavaScript:
 *   const docStr = Module.ccall('turi_doc_lookup', 'string', ['string'], [name]);
 */
const char *turi_doc_lookup(const char *name);

/* ---------------------------------------------------------------------------
 * Helper functions (internal to the glue layer)
 * ---------------------------------------------------------------------------
 */

/* Allocate a copy of a string using malloc.
 * The result must be freed with turi_wasm_free_string(). */
char *turi_wasm_strdup(const char *s);

/* SX8c: answer an SMT-LIB2 script with the refinement solver already linked
 * into this module.
 *
 * The semantics are `tur smt`'s to the letter -- the same session reader (so
 * `(push)`/`(pop)` scope assertions and each `(check-sat)` is answered where it
 * appears), the same S0..S3 chain, the same bounded model search, and the same
 * "a script with no `(check-sat)` is decided once at the end".  Scope is the
 * corpus subset of SMT-LIB2 over QF_UFLIA / QF_UFLRA; a script outside it is
 * refused WHOLE (an `error` key, no results) rather than partially parsed, and
 * `unknown` is a first-class answer.
 *
 * Read-only: nothing reachable from here touches elaboration or discharge, so
 * no answer can elide a runtime check.
 *
 * Returns a malloc'd JSON string; free it with turi_wasm_free_string.
 *
 *   {"schema":1,"results":[{"answer":"unsat","decided_by":"S2 (arithmetic)"}]}
 *   {"schema":1,"results":[{"answer":"sat","model":[{"name":"x","sort":"Int","value":1}],
 *                           "decided_by":"bounded model search"}]}
 *   {"schema":1,"results":[],"error":"unsupported command"}
 *
 * `schema` is 1, the stable shape since SX9, matching --dump-refine=json.
 */
char *turi_smt_check(const char *smtlib);

/* ---------------------------------------------------------------------------
 * Time-travel tracer (docs/archive/try-turmeric-tracer-plan.md, T3)
 *
 * turi_wasm_trace_run records a program; everything else answers questions
 * about the recording it left behind.  The JSON returns are owned by the glue
 * layer and are invalidated by the next call to the same function -- do NOT
 * free them.
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

int             turi_wasm_trace_run(const char *input, uint32_t max_steps,
                                    int has_main);
const char     *turi_wasm_trace_stats(void);
uint32_t        turi_wasm_trace_seek(uint32_t index);
const char     *turi_wasm_trace_state(void);
const char     *turi_wasm_trace_site_at(uint32_t index);
const char     *turi_wasm_trace_find_line(int dir, const char *file,
                                          uint32_t line);
const char     *turi_wasm_trace_output_full(void);
const uint8_t  *turi_wasm_trace_buffer(void);
uint32_t        turi_wasm_trace_buffer_len(void);
void            turi_wasm_trace_release(void);

#ifdef __cplusplus
}
#endif

#endif /* TURI_WASM_GLUE_H */
