#ifndef TUR_LSP_COLLECT_H
#define TUR_LSP_COLLECT_H

#include <stdbool.h>

#include "lsp_sym.h"

struct Expr;

/* -------------------------------------------------------------------------
 * Symbol harvesting (LD1)
 *
 * Walks an elaborated program and records every global binding -- name,
 * rendered type, defining span, defining file -- into a caller-supplied
 * LspSymbol array. This is the whole of what the LSP means by "analysis": the
 * compiler does the work, this reads the result off the tree.
 *
 * Split out of the driver so the walk has exactly one implementation. `tur lsp`
 * reaches it through compile_to_c; the WASM playground reaches it through its
 * own front end (src/web/wasm_lsp.c), which has no CLI to inherit. Leaving the
 * walk welded to the CLI would have forced the second caller to grow a copy,
 * and a copy is how the two would quietly stop agreeing about what a symbol is.
 *
 * Single-threaded: one collection is in flight at a time, bracketed by
 * begin/end.
 * --------------------------------------------------------------------- */

/* Start collecting into out[0..cap-1]. *count_out is zeroed and then tracks
 * the number of symbols written (capped at cap). */
void lsp_collect_begin(LspSymbol *out, int cap, int *count_out);

/* True between begin() and end(). Compiler passes consult this so they only
 * pay for the walk when someone asked for symbols. */
bool lsp_collect_active(void);

/* Record every global binding reachable from `prog` (an EX_PROGRAM).
 * No-op when no collection is active or `prog` is NULL. */
void lsp_collect_program(const struct Expr *prog);

/* Stop collecting. The caller's array and counter keep whatever was written. */
void lsp_collect_end(void);

#endif
