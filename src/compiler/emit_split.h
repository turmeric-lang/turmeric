#ifndef TUR_EMIT_SPLIT_H
#define TUR_EMIT_SPLIT_H

/* r7rs-programs-compile-slowly: a `#lang r7rs` program built as two C units.
 *
 * Every Scheme program reaches most of the R7RS prelude before it does
 * anything -- the uncaught-error printer alone pulls in `write` and the
 * numeric tower -- so compiling the prelude once and linking it is the only
 * way the build stops costing seconds.  The emitter writes the same
 * elaborated program twice:
 *
 *   the LIBRARY unit  -- the runtime preamble and every auto-loaded stdlib
 *                        definition (the prelude among them), with external
 *                        linkage, and nothing of the program.  It does not
 *                        depend on the program, so its object is cached by a
 *                        hash of its text and compiled once.
 *   the CLIENT unit   -- the program, declaring what it uses from the library
 *                        unit instead of defining it.
 *
 * Runtime state lives in the library unit only.  Stateless helpers (the
 * runtime preamble's `static` functions, typedefs, constructors of ADTs) are
 * written into both and each unit compiles the ones it calls.
 * emit_split_state is the text transform that gets the state right for text
 * the emitter writes verbatim: the runtime preamble, hoisted inline-C and a
 * stdlib file's file-scope C block. */

#include <stdbool.h>
#include <stddef.h>

#include "buf.h"

/* r7rs-imported-libraries-recompiled-every-build: a program that imports a
 * library from the stdlib -- `(srfi 13)`, `(scheme eval)` -- is written as
 * THREE units.  The library unit stays exactly what it is for every program;
 * between it and the program sits
 *
 *   the IMPORTED-LIBRARY unit -- the definitions of every stdlib library the
 *                        program imports, with external linkage.  It is a
 *                        client of the library unit (declares the prelude,
 *                        carries none of its state) and the owner of its own
 *                        items, exactly as the library unit owns the
 *                        prelude's.  Its text depends on what is imported,
 *                        not on the rest of the program, so its object is
 *                        cached too: by every program importing the same
 *                        libraries -- for an SRFI, pruned to the same
 *                        procedures (srfi_prune_program) -- and by every
 *                        rebuild of one.
 *
 * The program unit declares what either one defines.  The modes are ordered:
 * a unit defines its own items, declares those of the units before it, and
 * never sees those of the units after it. */
typedef enum {
    EMIT_SPLIT_NONE = 0,   /* one unit, as always */
    EMIT_SPLIT_LIB,        /* the library unit */
    EMIT_SPLIT_IMP,        /* the imported-library unit */
    EMIT_SPLIT_CLIENT,     /* the program unit */
} EmitSplitMode;

/* Rewrite the file-scope state in `src[0..len)` for one side of the split,
 * appending the result to `out` (`mode` is LIB for text a unit defines, CLIENT
 * for text a unit after the one defining it declares -- IMP is passed as
 * neither):
 *
 *   LIB     every file-scope variable loses `static`, so this unit defines it
 *           with external linkage.
 *   CLIENT  every file-scope variable becomes an `extern` declaration (its
 *           initializers dropped), every externally-linked function
 *           definition becomes its prototype, and a constructor or destructor
 *           is demoted to an ordinary (unused) function, so the program unit
 *           runs on the library unit's single instance of each and runs its
 *           start-up code once.
 *
 * Passed through untouched in both: `static` functions, typedefs, prototypes,
 * `extern` declarations, read-only (`const`-qualified) data, preprocessor
 * lines, comments, and everything inside a function body.  A function-local
 * `static` is therefore NOT shared -- tests/check-r7rs-prelude-split.sh finds
 * one by comparing the data symbols, local and global, the two objects
 * define. */
void emit_split_state(const char *src, size_t len, EmitSplitMode mode, Buf *out);

/* r7rs-split-units-each-compile-the-runtime: copy `src[0..len)` to `out`
 * with every `static` function it DEFINES made external -- the definition
 * and each prototype of it lose `static`, and its name is recorded with
 * emit_split_note_export.  For runtime text pasted into every unit whose
 * functions are not per-unit (the r7rs-gc collector, the DK runtime, the
 * deep-stack helper; emit_split_once_fns in emit_module.c): run before
 * emit_split_state, which then keeps the definitions in the library unit and
 * turns them into prototypes in every later one, so the code is compiled
 * once instead of once per unit.  Left `static`: `inline` functions (each
 * unit inlines its own copy), a constructor or destructor (emit_split_state
 * arms it in the library unit only), and a function only prototyped here
 * (another part of the unit defines it). */
void emit_split_export_static_fns(const char *src, size_t len, Buf *out);

/* Every symbol the library unit exports is renamed with this prefix, in both
 * units.  In one unit the runtime's state and the stdlib's functions are
 * `static`; exported, their names are the same ones the runtime archives
 * define -- libturi.a carries the S2 split runtime, with its own
 * `g_tur_any_types`, `tur_scheduler` and the rest -- and the linker would
 * bind the embedded evaluator to the program's copies instead of its own. */
#define EMIT_SPLIT_PREFIX "tur_sl_"

/* Record a name the library unit gives external linkage.  emit_split_state
 * records the ones it exports itself (in EMIT_SPLIT_LIB mode); the emitter
 * records the stdlib definitions. */
void emit_split_note_export(const char *name);
void emit_split_exports_clear(void);
/* How many names are recorded: what the imported-library unit adds to the
 * library unit's is a measure of how much it defines. */
size_t emit_split_exports_count(void);

/* Copy `src[0..len)` to `out` with every identifier recorded by
 * emit_split_note_export (or the LIB-mode transform) given EMIT_SPLIT_PREFIX.
 * String and character literals and comments are left as written. */
void emit_split_rename(const char *src, size_t len, Buf *out);

/* r7rs-prelude-library-cold-compile: the library unit as `n` pieces that
 * compile concurrently and link as one object.  Almost all of the unit's
 * compile is optimizing its ~1,200 external functions, so each piece keeps
 * the whole unit's declarations and static helpers and defines only its share
 * of those, balanced by size:
 *
 *   - an external function is defined in one piece and prototyped in the
 *     others;
 *   - a file-scope variable is defined in piece 0 and declared `extern` in
 *     the others -- a `static` one loses `static` and moves to the private
 *     prefix EMIT_SPLIT_PIECE_PREFIX, so nothing the runtime archives define
 *     can capture it; read-only data is copied freely;
 *   - a constructor or destructor runs from one piece (piece 0 for a static
 *     one), demoted to an ordinary function elsewhere;
 *   - a SMALL external function (body at most `dup_max` bytes, no local
 *     `static`, no `__func__`) is also a `static` copy in every other piece,
 *     reached by a function-like macro so that only direct calls use it:
 *     `-O2` inlines it there as it did in one unit, and taking its address
 *     still names the one external definition.
 *
 * Writes pieces[0..n) (each buf_init'd by the caller) and returns 0, or
 * nonzero when the text does not split (then compile it whole). */
#define EMIT_SPLIT_PIECE_PREFIX "tur_sp_"
/* The imported-library unit's pieces use their own prefix: its object links
 * beside the library unit's, so a static of the same name in both must not
 * become one external symbol defined twice. */
#define EMIT_SPLIT_IMP_PIECE_PREFIX "tur_spi_"
int emit_split_pieces(const char *src, size_t len, int n, size_t dup_max,
                      const char *prefix, Buf *pieces);

#endif /* TUR_EMIT_SPLIT_H */
