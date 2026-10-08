/* srfi_prune.h -- drop the SRFI definitions a program never reaches
 * (r7rs-srfi-plan S3).
 *
 * `(import (srfi N))` splices stdlib/srfi/N.scm's definitions into the
 * program once per compile (D3).  Emitted as they stand, every one of them
 * reaches the C whether the program uses it or not, and gcc cannot drop them
 * either: a procedure used as a value anywhere, dead code included, gets a
 * static fat box that `__tur_fatbox_init` fills at startup, which keeps it
 * and everything it calls alive.  For SRFI 1 that cost every importing
 * program about 1.6 s of build time (S0's measurement).
 *
 * This pass runs on the whole program after the transform passes and before
 * emission, where every module is loaded and the program is one translation
 * unit.  It removes each top-level definition from a `stdlib/srfi/` file that
 * nothing outside those files reaches, transitively through the SRFI's own
 * definitions and the lambdas lifted out of them.
 *
 * It is conservative in three ways:
 *
 *   - only a definition from a `stdlib/srfi/` file is ever removed, and only
 *     a `defn` or a `def` whose initializer has no side effect -- a value, or
 *     a call that only allocates (r7rs-srfi-18-216-sicp-plan T0b: SRFI 27's
 *     `default-random-source`; see alloc_only in srfi_prune.c);
 *   - a definition with C linkage (`retain_c_linkage`, `c_export_name`) or
 *     an exported one, when exports are kept, stays;
 *   - the walk models every expression kind.  Should it meet one it does not
 *     (a kind added later), it leaves the program as it found it.
 *
 * Returns the number of items removed, or 0 when there was nothing to do (or
 * the walk gave up).  TUR_NO_SRFI_PRUNE=1 turns the pass off, for measuring. */
#ifndef TUR_SRFI_PRUNE_H
#define TUR_SRFI_PRUNE_H

#include <stdbool.h>
#include "arena.h"
#include "expr.h"

uint32_t srfi_prune_program(Arena *arena, Expr *prog, bool keep_exported);

#endif
