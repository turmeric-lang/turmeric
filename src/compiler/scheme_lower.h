#ifndef TUR_SCHEME_LOWER_H
#define TUR_SCHEME_LOWER_H

/* scheme_lower.h -- r7rs-lang-plan R2: the Scheme core forms, as a Form ->
 * Form lowering onto Turmeric's own binding and control forms.
 *
 * A `#lang r7rs` file reads (R1) into the same Form model every other dialect
 * uses; what makes it Scheme is what its special forms MEAN.  R2 answers that
 * for the core forms by rewriting them, before elaboration, into the Turmeric
 * forms with the same meaning:
 *
 *   (define (f a . r) body...)   -> (defn f [a & r : any] body'...)
 *   (define x e)                 -> (def [^mut] x [: any] e')
 *   (lambda (a b) body...)       -> (fn [a b] body')
 *   (let ((a i)...) body...)     -> (let [a i'...] body')     (parallel when it matters)
 *   (let loop ((a i)...) body)   -> (letrec [loop (fn [a...] body')] (loop i'...))
 *   (let* / letrec / letrec*)    -> nested let / letrec
 *   (do ((v i s)...) (t r...) c) -> a letrec loop
 *   (begin e...)                 -> (do e'...)      (spliced at top level)
 *   (if c t)                     -> (if c' t' nil)
 *   (cond ...) (case ...)        -> if chains, with `=>` and `else`
 *   (and ...) (or ...)           -> value-returning if chains (Scheme's, not Turmeric's bool ones)
 *   (when ...) (unless ...)      -> if
 *   (case-lambda ...)            -> a variadic fn dispatching on argument count
 *   (define-values / let-values / let*-values) -> temporaries over the Values carrier
 *   internal defines             -> letrec (lambdas) / let (values), R7RS letrec* order
 *
 * Only forms whose source file is LANG_R7RS are touched (the decision is
 * per-file, off the span, exactly like lang_span_is_dynamic); a Turmeric-shaped
 * form inside such a file -- `(defn ...)`, `(let [x 1] ...)` -- passes through
 * with its subforms lowered, so a Scheme file may still reach for a Turmeric
 * form by its Turmeric spelling.  Nothing under `quote` is touched.
 *
 * Two facts about the substrate shape what comes out:
 *
 *   - Turmeric's `set!` needs `^mut` at the binding site and a value of the
 *     binding's type.  A Scheme variable that is ever `set!` is therefore
 *     bound as `^mut name : any` (the whole file is scanned for `set!` targets
 *     first), and everything else keeps Saffron's local inference.
 *   - A stdlib name cannot be redefined at top level, and the R7RS prelude
 *     must define `car`, `list`, `length`, ... which the typed stdlib already
 *     owns on its carrier list.  So the prelude's procedures are spelled
 *     `r7rs-<name>` and this pass renames the Scheme spelling to the prelude
 *     one (scheme_lower_rename_table) wherever it occurs unquoted.  R7's
 *     `(import (scheme base))` maps onto the same table.
 *
 * Runs after `(load ...)` expansion in both the entry program and an imported
 * module, so a loaded or imported Scheme file is lowered too.  It never sees
 * the top-level `main` fold: a lowered Scheme expression at top level is an
 * ordinary statement, and elab_toplevel.c folds those into `main` as it does
 * for any file. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forms.h"
#include "runtime/arena.h"
#include "symbols.h"

/* Lower the Scheme forms among `forms` (a top-level sequence: a program or a
 * module).  Returns a fresh arena array and its length through `out_n`;
 * forms from non-Scheme files are passed through by pointer.  The length can
 * grow (a top-level `begin` or `define-values` splices) and never shrinks.
 * Diagnostics are emitted for malformed Scheme forms; the offending form is
 * dropped and elaboration continues so that diag_had_error() reports it. */
/* r7rs-define-library-cannot-export-syntax: the module path of a Scheme
 * library (`my/utils`) -> its source file, by the module loader's own search
 * (elab_scheme_library_path).  An importer reads the library's `syntax-rules`
 * macros from it, since they are expanded here, before the module is loaded.
 * `resolve` may be NULL: a library's macros are then not importable. */
typedef bool (*SchemeLibResolveFn)(void *ud, const char *module, char *path, size_t cap);
/* What a global of the environment the program joins is
 * (elab_scheme_global_kind).  An interpreter or REPL session elaborated its
 * stdlib, and the user's earlier prompt turns, in earlier calls, so neither
 * is among `forms`:
 *   - a program global spelled like a STDLIB one is respelled for the
 *     program (`(define (None x) ...)` defines `None--user`), as it is when
 *     the stdlib's forms are in the stream;
 *   - a name an EARLIER TURN respelled that way (`square--user`) is what the
 *     name means in every later turn.
 * May be NULL (a library's module: its names are not respelled). */
typedef enum { SCHEME_GLOBAL_NONE, SCHEME_GLOBAL_STDLIB, SCHEME_GLOBAL_EARLIER_TURN } SchemeGlobalKind;
typedef SchemeGlobalKind (*SchemeGlobalFn)(void *ud, const char *name);
/* r7rs-turmeric-syntax-leaks item 8: the auto-loaded stdlib file (its
 * basename without `.tur`, e.g. "vec") that defines the global `name`, for a
 * name the stream's own forms do not show -- an interpreter or REPL session
 * elaborated its stdlib earlier, and a library module's stream holds only the
 * library (elab_scheme_stdlib_file).  False when `name` is no such global.
 * May be NULL. */
typedef bool (*SchemeStdlibFileFn)(void *ud, const char *name, char *out, size_t cap);

Form **scheme_lower_program(Arena *a, SymbolTable *st,
                            Form *const *forms, uint32_t n, uint32_t *out_n,
                            SchemeLibResolveFn resolve, SchemeGlobalFn global_kind,
                            SchemeStdlibFileFn stdlib_file, void *resolve_ud);

/* r7rs-repl-forgets-macros-and-set: the raw (unlowered) forms of a REPL or
 * `eval` session's earlier turns.  An incremental elaboration lowers only the
 * new turn, so the interpreter names the turns before it here, around that
 * call, and clears it (NULL) after.  A REPL turn's lowering re-registers the
 * `define-syntax` macros they defined. */
void scheme_lower_set_session_prior(Form *const *forms, uint32_t n);

/* True when any form in `forms` belongs to a LANG_R7RS file -- a cheap test a
 * caller can make before paying for the pass. */
bool scheme_lower_needed(Form *const *forms, uint32_t n);

/* r7rs-lang-plan R7: the stdlib/r7rs/<lib>.tur files a top-level form of a
 * Scheme file imports (the on-demand libraries: time, process-context,
 * file).  Called by the load expander before the lowering, which splices
 * each file in as though the program had `(load ...)`ed it.  Returns the
 * count written to `out` (interned string literals; do not free). */
uint32_t scheme_import_library_files(const Form *f, const char **out, uint32_t cap);

/* r7rs-type-errors-are-uncatchable-panics: the Scheme spelling of a prelude
 * procedure (`r7rs-car` -> "car"), from the rename table; NULL when the
 * prelude name is not one a Scheme program writes.  An error message names
 * the procedure the way the program did. */
const char *scheme_public_name(const char *prelude_name);
/* The name a Scheme program wrote for `name` after the lowering: the public
 * name of a prelude procedure, a local binder without its `__v<N>` suffix, a
 * global without its `--user` respelling.  Writes into `buf` when it has to
 * trim; returns `name` itself when there is nothing to undo. */
const char *scheme_source_name(const char *name, char *buf, size_t cap);

/* r7rs-too-few-arguments-returns-a-procedure: true for a span in a Scheme
 * program or library the user wrote -- `#lang r7rs` source outside the
 * Turmeric-shaped prelude files (stdlib/r7rs/, the REPL's pinned preload),
 * which use Turmeric's own semantics, partial application included. */
bool scheme_span_is_user_source(Span sp);

/* r7rs-callcc-memory-never-freed: set once a Scheme program in this process
 * imports a Turmeric module or a library of its own -- code that can reach
 * natives which keep values in memory the interpreter's continuation-image
 * sweep cannot read.  The sweep then stays off.  Never reset. */
extern bool g_scheme_lower_opened_seam;

#endif /* TUR_SCHEME_LOWER_H */
