/* scheme_lower.c -- r7rs-lang-plan R2: lower the Scheme core forms onto
 * Turmeric's binding and control forms.  See scheme_lower.h for the map. */
#include "scheme_lower.h"
#include "runtime/globals.h"   /* R9: g_synthetic_user_from_line */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "lang_dialects.h"
#include "reader.h"            /* include / include-ci: read a file as Scheme */
#include "expr.h"              /* R10: tur_name_is_reserved_special_form */
#include "stdlib_autoload.h"   /* R3: which `(turmeric stdlib/x)` imports are no-ops */
#include "builtins.h"          /* r7rs-turmeric-syntax-leaks item 8: builtin_first_with_name */

/* elab_core.c: the builtin type a name spells, or TY_UNKNOWN (elab_internal.h). */
TypeKind typekind_from_symbol(const char *name);

/* ---------------------------------------------------------------------------
 * The rename table: Scheme spelling -> prelude spelling.
 *
 * The R7RS prelude (stdlib/r7rs/prelude.tur) cannot define `car` -- the typed
 * stdlib's list.tur already does, on the carrier list, and a top-level
 * redefinition of an auto-loaded name is an error -- so every procedure the
 * prelude provides is spelled `r7rs-<name>` and the Scheme spelling is mapped
 * here.  The table is the prelude's export list; a procedure added to the
 * prelude is added here in the same change, or Scheme code cannot reach it.
 * A user `(define (car x) ...)` maps the same way and therefore collides with
 * the prelude's, which is the Turmeric rule for a stdlib name and the
 * documented R2 deviation from R7RS 5.3.1.
 * ------------------------------------------------------------------------- */
static const char *const RENAMES[][2] = {
    { "car",              "r7rs-car" },
    { "cdr",              "r7rs-cdr" },
    { "caar",             "r7rs-caar" },
    { "cadr",             "r7rs-cadr" },
    { "cdar",             "r7rs-cdar" },
    { "cddr",             "r7rs-cddr" },
    { "caddr",            "r7rs-caddr" },
    { "cons",             "r7rs-cons" },
    { "list",             "r7rs-list" },
    { "null?",            "r7rs-null?" },
    { "pair?",            "r7rs-pair?" },
    { "list?",            "r7rs-list?" },
    { "length",           "r7rs-length" },
    { "list-ref",         "r7rs-list-ref" },
    { "list-tail",        "r7rs-list-tail" },
    { "append",           "r7rs-append" },
    { "reverse",          "r7rs-reverse" },
    { "map",              "r7rs-map" },
    { "for-each",         "r7rs-for-each" },
    { "memv",             "r7rs-memv" },
    { "memq",             "r7rs-memq" },
    { "member",           "r7rs-member" },
    { "assv",             "r7rs-assv" },
    { "assq",             "r7rs-assq" },
    { "assoc",            "r7rs-assoc" },
    { "display",          "r7rs-display" },
    { "write",            "r7rs-write" },
    { "newline",          "r7rs-newline" },
    { "not",              "r7rs-not" },
    { "eqv?",             "r7rs-eqv?" },
    { "eq?",              "r7rs-eq?" },
    { "equal?",           "r7rs-equal?" },
    { "boolean?",         "r7rs-boolean?" },
    { "number?",          "r7rs-number?" },
    { "integer?",         "r7rs-integer?" },
    { "real?",            "r7rs-real?" },
    { "string?",          "r7rs-string?" },
    { "symbol?",          "r7rs-symbol?" },
    { "procedure?",       "r7rs-procedure?" },
    { "zero?",            "r7rs-zero?" },
    { "positive?",        "r7rs-positive?" },
    { "negative?",        "r7rs-negative?" },
    { "even?",            "r7rs-even?" },
    { "odd?",             "r7rs-odd?" },
    { "abs",              "r7rs-abs" },
    { "min",              "r7rs-min" },
    { "max",              "r7rs-max" },
    { "values",           "r7rs-values" },
    { "call-with-values", "r7rs-call-with-values" },
    { "apply",            "r7rs-apply" },
    { "error",            "r7rs-error" },
    { "void",             "r7rs-void" },
    /* R3: data. */
    { "set-car!",         "r7rs-set-car!" },
    { "set-cdr!",         "r7rs-set-cdr!" },
    { "list-copy",        "r7rs-list-copy" },
    { "char?",            "r7rs-char?" },
    { "char->integer",    "r7rs-char->integer" },
    { "integer->char",    "r7rs-integer->char" },
    { "char=?",           "r7rs-char=?" },
    { "char<?",           "r7rs-char<?" },
    { "char>?",           "r7rs-char>?" },
    { "char-upcase",      "r7rs-char-upcase" },
    { "char-downcase",    "r7rs-char-downcase" },
    { "char-alphabetic?", "r7rs-char-alphabetic?" },
    { "char-numeric?",    "r7rs-char-numeric?" },
    { "char-whitespace?", "r7rs-char-whitespace?" },
    { "string-length",    "r7rs-string-length" },
    { "string-ref",       "r7rs-string-ref" },
    { "string-append",    "r7rs-string-append" },
    { "substring",        "r7rs-substring" },
    { "string-copy",      "r7rs-string-copy" },
    /* T3: the mutators, on the R7rsString a procedure newly allocates. */
    { "string-set!",      "r7rs-string-set!" },
    { "string-fill!",     "r7rs-string-fill!" },
    { "string-copy!",     "r7rs-string-copy!" },
    { "string=?",         "r7rs-string=?" },
    { "string<?",         "r7rs-string<?" },
    { "string->symbol",   "r7rs-string->symbol" },
    { "symbol->string",   "r7rs-symbol->string" },
    { "string->list",     "r7rs-string->list" },
    { "list->string",     "r7rs-list->string" },
    { "number->string",   "r7rs-number->string" },
    { "vector",           "r7rs-vector" },
    { "vector?",          "r7rs-vector?" },
    { "make-vector",      "r7rs-make-vector" },
    { "vector-ref",       "r7rs-vector-ref" },
    { "vector-set!",      "r7rs-vector-set!" },
    { "vector-length",    "r7rs-vector-length" },
    { "vector->list",     "r7rs-vector->list" },
    { "list->vector",     "r7rs-list->vector" },
    { "vector-fill!",     "r7rs-vector-fill!" },
    { "bytevector",       "r7rs-bytevector" },
    { "bytevector?",      "r7rs-bytevector?" },
    { "make-bytevector",  "r7rs-make-bytevector" },
    { "bytevector-u8-ref",  "r7rs-bytevector-u8-ref" },
    { "bytevector-u8-set!", "r7rs-bytevector-u8-set!" },
    { "bytevector-length",  "r7rs-bytevector-length" },
    { "eof-object",       "r7rs-eof-object" },
    { "eof-object?",      "r7rs-eof-object?" },
    /* R7: the rest of (scheme base) that is not a port, and the pure
     * libraries -- (scheme char), (scheme cxr), (scheme complex) -- which
     * live in the prelude like base does. */
    { "boolean=?", "r7rs-boolean=?" },
    { "symbol=?", "r7rs-symbol=?" },
    { "char<=?", "r7rs-char<=?" },
    { "char>=?", "r7rs-char>=?" },
    { "list-set!", "r7rs-list-set!" },
    { "make-list", "r7rs-make-list" },
    { "make-string", "r7rs-make-string" },
    { "string", "r7rs-string" },
    { "string->utf8", "r7rs-string->utf8" },
    { "utf8->string", "r7rs-utf8->string" },
    { "string->vector", "r7rs-string->vector" },
    { "vector->string", "r7rs-vector->string" },
    { "string-for-each", "r7rs-string-for-each" },
    { "string-map", "r7rs-string-map" },
    { "string<=?", "r7rs-string<=?" },
    { "string>=?", "r7rs-string>=?" },
    { "string>?", "r7rs-string>?" },
    { "vector-append", "r7rs-vector-append" },
    { "vector-copy", "r7rs-vector-copy" },
    { "vector-copy!", "r7rs-vector-copy!" },
    { "vector-for-each", "r7rs-vector-for-each" },
    { "vector-map", "r7rs-vector-map" },
    { "bytevector-append", "r7rs-bytevector-append" },
    { "bytevector-copy", "r7rs-bytevector-copy" },
    { "bytevector-copy!", "r7rs-bytevector-copy!" },
    { "features", "r7rs-features" },
    { "rationalize", "r7rs-rationalize" },
    { "write-simple", "r7rs-write-simple" },
    { "char-ci<=?", "r7rs-char-ci<=?" },
    { "char-ci<?", "r7rs-char-ci<?" },
    { "char-ci=?", "r7rs-char-ci=?" },
    { "char-ci>=?", "r7rs-char-ci>=?" },
    { "char-ci>?", "r7rs-char-ci>?" },
    { "char-foldcase", "r7rs-char-foldcase" },
    { "char-lower-case?", "r7rs-char-lower-case?" },
    { "char-upper-case?", "r7rs-char-upper-case?" },
    { "digit-value", "r7rs-digit-value" },
    { "string-ci<=?", "r7rs-string-ci<=?" },
    { "string-ci<?", "r7rs-string-ci<?" },
    { "string-ci=?", "r7rs-string-ci=?" },
    { "string-ci>=?", "r7rs-string-ci>=?" },
    { "string-ci>?", "r7rs-string-ci>?" },
    { "string-downcase", "r7rs-string-downcase" },
    { "string-foldcase", "r7rs-string-foldcase" },
    { "string-upcase", "r7rs-string-upcase" },
    { "angle", "r7rs-angle" },
    { "imag-part", "r7rs-imag-part" },
    { "magnitude", "r7rs-magnitude" },
    { "make-polar", "r7rs-make-polar" },
    { "make-rectangular", "r7rs-make-rectangular" },
    { "real-part", "r7rs-real-part" },
    { "caaar", "r7rs-caaar" },
    { "caadr", "r7rs-caadr" },
    { "cadar", "r7rs-cadar" },
    { "cdaar", "r7rs-cdaar" },
    { "cdadr", "r7rs-cdadr" },
    { "cddar", "r7rs-cddar" },
    { "cdddr", "r7rs-cdddr" },
    { "caaaar", "r7rs-caaaar" },
    { "caaadr", "r7rs-caaadr" },
    { "caadar", "r7rs-caadar" },
    { "caaddr", "r7rs-caaddr" },
    { "cadaar", "r7rs-cadaar" },
    { "cadadr", "r7rs-cadadr" },
    { "caddar", "r7rs-caddar" },
    { "cadddr", "r7rs-cadddr" },
    { "cdaaar", "r7rs-cdaaar" },
    { "cdaadr", "r7rs-cdaadr" },
    { "cdadar", "r7rs-cdadar" },
    { "cdaddr", "r7rs-cdaddr" },
    { "cddaar", "r7rs-cddaar" },
    { "cddadr", "r7rs-cddadr" },
    { "cdddar", "r7rs-cdddar" },
    { "cddddr", "r7rs-cddddr" },
    /* R8: ports (display, write, newline and write-simple are above). */
    { "current-input-port", "r7rs-current-input-port" },
    { "current-output-port", "r7rs-current-output-port" },
    { "current-error-port", "r7rs-current-error-port" },
    { "port?", "r7rs-port?" },
    { "input-port?", "r7rs-input-port?" },
    { "output-port?", "r7rs-output-port?" },
    { "textual-port?", "r7rs-textual-port?" },
    { "binary-port?", "r7rs-binary-port?" },
    { "input-port-open?", "r7rs-input-port-open?" },
    { "output-port-open?", "r7rs-output-port-open?" },
    { "close-port", "r7rs-close-port" },
    { "close-input-port", "r7rs-close-input-port" },
    { "close-output-port", "r7rs-close-output-port" },
    { "call-with-port", "r7rs-call-with-port" },
    { "open-input-string", "r7rs-open-input-string" },
    { "open-output-string", "r7rs-open-output-string" },
    { "get-output-string", "r7rs-get-output-string" },
    { "open-input-bytevector", "r7rs-open-input-bytevector" },
    { "open-output-bytevector", "r7rs-open-output-bytevector" },
    { "get-output-bytevector", "r7rs-get-output-bytevector" },
    { "read-char", "r7rs-read-char" },
    { "peek-char", "r7rs-peek-char" },
    { "read-line", "r7rs-read-line" },
    { "read-string", "r7rs-read-string" },
    { "read-u8", "r7rs-read-u8" },
    { "peek-u8", "r7rs-peek-u8" },
    { "char-ready?", "r7rs-char-ready?" },
    { "u8-ready?", "r7rs-u8-ready?" },
    { "read-bytevector", "r7rs-read-bytevector" },
    { "read-bytevector!", "r7rs-read-bytevector!" },
    { "write-char", "r7rs-write-char" },
    { "write-string", "r7rs-write-string" },
    { "write-u8", "r7rs-write-u8" },
    { "write-bytevector", "r7rs-write-bytevector" },
    { "flush-output-port", "r7rs-flush-output-port" },
    { "write-shared", "r7rs-write-shared" },
    /* R6: control.  `guard`, `parameterize`, `delay` and `delay-force` are
     * forms (lower_guard / lower_parameterize / lower_delay); these are the
     * procedures. */
    { "call/cc",                        "r7rs-call/cc" },
    { "call-with-current-continuation", "r7rs-call/cc" },
    { "dynamic-wind",                   "r7rs-dynamic-wind" },
    { "with-exception-handler",         "r7rs-with-exception-handler" },
    { "raise",                          "r7rs-raise" },
    { "raise-continuable",              "r7rs-raise-continuable" },
    { "error-object?",                  "r7rs-error-object?" },
    { "error-object-message",           "r7rs-error-object-message" },
    { "error-object-irritants",         "r7rs-error-object-irritants" },
    { "read-error?",                    "r7rs-read-error?" },
    { "file-error?",                    "r7rs-file-error?" },
    { "make-parameter",                 "r7rs-make-parameter" },
    { "make-promise",                   "r7rs-make-promise" },
    { "promise?",                       "r7rs-promise?" },
    { "force",                          "r7rs-force" },
    /* R5: numbers.  The operators + - * / = < > <= >= are not rows: in call
     * position the lowering folds them onto the binary helpers, and in value
     * position it names the variadic procedures (lower_operator / sl->ops). */
    { "exact?", "r7rs-exact?" },
    { "inexact?", "r7rs-inexact?" },
    { "exact-integer?", "r7rs-exact-integer?" },
    { "exact-rational?", "r7rs-exact-rational?" },
    { "nan?", "r7rs-nan?" },
    { "infinite?", "r7rs-infinite?" },
    { "finite?", "r7rs-finite?" },
    { "rational?", "r7rs-rational?" },
    { "complex?", "r7rs-complex?" },
    { "exact", "r7rs-exact" },
    { "inexact", "r7rs-inexact" },
    { "exact->inexact", "r7rs-exact->inexact" },
    { "inexact->exact", "r7rs-inexact->exact" },
    { "floor", "r7rs-floor" },
    { "ceiling", "r7rs-ceiling" },
    { "round", "r7rs-round" },
    { "truncate", "r7rs-truncate" },
    { "quotient", "r7rs-quotient" },
    { "remainder", "r7rs-remainder" },
    { "modulo", "r7rs-modulo" },
    { "floor/", "r7rs-floor/" },
    { "truncate/", "r7rs-truncate/" },
    { "floor-quotient", "r7rs-floor-quotient" },
    { "floor-remainder", "r7rs-floor-remainder" },
    { "truncate-quotient", "r7rs-truncate-quotient" },
    { "truncate-remainder", "r7rs-truncate-remainder" },
    { "gcd", "r7rs-gcd" },
    { "lcm", "r7rs-lcm" },
    { "expt", "r7rs-expt" },
    { "exp", "r7rs-exp" },
    { "log", "r7rs-log" },
    { "sin", "r7rs-sin" },
    { "cos", "r7rs-cos" },
    { "tan", "r7rs-tan" },
    { "asin", "r7rs-asin" },
    { "acos", "r7rs-acos" },
    { "atan", "r7rs-atan" },
    { "sqrt", "r7rs-sqrt" },
    { "exact-integer-sqrt", "r7rs-exact-integer-sqrt" },
    { "square", "r7rs-square" },
    { "string->number", "r7rs-string->number" },
    { "numerator", "r7rs-numerator" },
    { "denominator", "r7rs-denominator" },
};
#define N_RENAMES (sizeof(RENAMES) / sizeof(RENAMES[0]))

const char *scheme_public_name(const char *prelude_name) {
    if (!prelude_name) return NULL;
    for (size_t i = 0; i < sizeof RENAMES / sizeof RENAMES[0]; i++)
        if (strcmp(RENAMES[i][1], prelude_name) == 0) return RENAMES[i][0];
    return NULL;
}

/* r7rs-turmeric-syntax-leaks item 4: see caret_spelling. */
#define SCHEME_CARET_PREFIX "__scheme_caret_"
const char *scheme_source_name(const char *name, char *buf, size_t cap) {
    if (!name || !buf || cap == 0) return name;
    const char *pub = scheme_public_name(name);
    if (pub) return pub;
    size_t n = strlen(name);
    /* A `^` identifier (bind_name, caret_spelling). */
    bool caret = strncmp(name, SCHEME_CARET_PREFIX, sizeof SCHEME_CARET_PREFIX - 1) == 0;
    if (caret) { name += sizeof SCHEME_CARET_PREFIX - 1; n -= sizeof SCHEME_CARET_PREFIX - 1; }
    /* A local binder renamed apart (bind_name): `<name>__v<digits>`. */
    size_t d = n;
    while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9') d--;
    if (d < n && d >= 3 && memcmp(name + d - 3, "__v", 3) == 0) n = d - 3;
    /* A global respelled apart from a stdlib or Turmeric-form name. */
    else if (n > 6 && memcmp(name + n - 6, "--user", 6) == 0) n -= 6;
    if (n == 0 || n + (caret ? 1 : 0) >= cap) return name;
    size_t at = 0;
    if (caret) buf[at++] = '^';
    memcpy(buf + at, name, n);
    buf[at + n] = '\0';
    return buf;
}

/* R7: the R7RS-small libraries.  A RESIDENT library's procedures live in the
 * prelude, so importing it is a scoping statement only.  An ON-DEMAND
 * library is its own file under stdlib/r7rs/, spliced in by the load
 * expander when a Scheme file imports it (scheme_import_library_files), so a
 * program that does not import (scheme time) carries none of it; its names
 * rename only once it is imported.  A DEFERRED library is refused at the
 * import with the reason. */
enum { LIB_RESIDENT, LIB_ONDEMAND, LIB_DEFERRED };
static const struct { const char *name; int kind; const char *what; } SCHEME_LIBS[] = {
    { "base",            LIB_RESIDENT, NULL },
    { "case-lambda",     LIB_RESIDENT, NULL },
    { "char",            LIB_RESIDENT, NULL },
    { "complex",         LIB_RESIDENT, NULL },
    { "cxr",             LIB_RESIDENT, NULL },
    { "inexact",         LIB_RESIDENT, NULL },
    { "lazy",            LIB_RESIDENT, NULL },
    { "write",           LIB_RESIDENT, NULL },
    { "time",            LIB_ONDEMAND, "stdlib/r7rs/time.tur" },
    { "process-context", LIB_ONDEMAND, "stdlib/r7rs/process-context.tur" },
    { "file",            LIB_ONDEMAND, "stdlib/r7rs/file.tur" },
    /* r7rs-lang-plan T4: the evaluator libraries share one file, whose
     * inline C links the interpreter into a compiled program -- only one
     * that imports them. */
    { "eval",            LIB_ONDEMAND, "stdlib/r7rs/eval.tur" },
    { "repl",            LIB_ONDEMAND, "stdlib/r7rs/eval.tur" },
    { "load",            LIB_ONDEMAND, "stdlib/r7rs/eval.tur" },
    { "r5rs",            LIB_ONDEMAND, "stdlib/r7rs/eval.tur" },
    { "read",            LIB_ONDEMAND, "stdlib/r7rs/read.tur" },
};
#define N_SCHEME_LIBS (sizeof(SCHEME_LIBS) / sizeof(SCHEME_LIBS[0]))
/* The procedures of the on-demand libraries: Scheme name, prelude-style
 * target, library. */
static const char *const ONDEMAND[][3] = {
    { "current-second",            "r7rs-current-second",            "time" },
    { "current-jiffy",             "r7rs-current-jiffy",             "time" },
    { "jiffies-per-second",        "r7rs-jiffies-per-second",        "time" },
    { "command-line",              "r7rs-command-line",              "process-context" },
    { "exit",                      "r7rs-exit",                      "process-context" },
    { "emergency-exit",            "r7rs-emergency-exit",            "process-context" },
    { "get-environment-variable",  "r7rs-get-environment-variable",  "process-context" },
    { "get-environment-variables", "r7rs-get-environment-variables", "process-context" },
    { "file-exists?",              "r7rs-file-exists?",              "file" },
    { "delete-file",               "r7rs-delete-file",               "file" },
    { "open-input-file", "r7rs-open-input-file", "file" },
    { "open-output-file", "r7rs-open-output-file", "file" },
    { "open-binary-input-file", "r7rs-open-binary-input-file", "file" },
    { "open-binary-output-file", "r7rs-open-binary-output-file", "file" },
    { "call-with-input-file", "r7rs-call-with-input-file", "file" },
    { "call-with-output-file", "r7rs-call-with-output-file", "file" },
    { "with-input-from-file", "r7rs-with-input-from-file", "file" },
    { "with-output-to-file", "r7rs-with-output-to-file", "file" },
    { "read", "r7rs-read", "read" },
    { "eval",                      "r7rs-eval",                      "eval" },
    { "environment",               "r7rs-environment",               "eval" },
    { "interaction-environment",   "r7rs-interaction-environment",   "repl" },
    { "load",                      "r7rs-load",                      "load" },
    { "eval",                      "r7rs-eval",                      "r5rs" },
    { "interaction-environment",   "r7rs-interaction-environment",   "r5rs" },
    { "load",                      "r7rs-load",                      "r5rs" },
    { "null-environment",          "r7rs-null-environment",          "r5rs" },
    { "scheme-report-environment", "r7rs-scheme-report-environment", "r5rs" },
};
#define N_ONDEMAND (sizeof(ONDEMAND) / sizeof(ONDEMAND[0]))

/* r7rs-srfi-plan D1/D2/D4: every SRFI `(import (srfi N))` knows -- Racket's
 * documented list (its `srfi` collection), plus 0 and 105 -- and what an
 * import of each does.  One table drives the import (srfi_import),
 * cond-expand's `srfi-N` and `(library (srfi N))` (feature_holds), the list
 * `(features)` returns (the prelude's r7rs-features, kept equal by
 * tests/check-r7rs-srfi-sync.sh), the load expander's splice
 * (lib_files_of_set), and the guide's support table, which
 * tests/check-r7rs-srfi-sync.sh compares against it.
 *
 *   BUILTIN    R7RS already is the SRFI: the import binds its names to the
 *              (scheme ...) bindings they re-export and emits nothing.
 *   ALIAS      a few new names for built-in procedures (38, 45).
 *   LIBRARY    a real implementation, spliced in when imported.
 *   NOLIB      the syntax is always on and there is no library, as in
 *              Racket (62); 0 and 105, which Racket has no module for either.
 *   NOTPLANNED refused, with `why`.
 *   NOTYET     refused until the plan stage in `why` lands.
 *
 * An importable row's `file` is its define-library, stdlib/srfi/<N>.scm.
 * Where each NOTYET row's implementation will come from, under what
 * licence, and which test suite exists for it: r7rs-srfi-plan Appendix C
 * (the S0 inventory). */
enum { SRFI_BUILTIN, SRFI_ALIAS, SRFI_LIBRARY, SRFI_NOLIB, SRFI_NOTPLANNED, SRFI_NOTYET };
/* r7rs-srfi-18-216-sicp-plan D4: `(sicp extras)` -- Racket's `#lang sicp`
 * extras (inc, dec, identity, amb) plus amb-reset! -- is not an SRFI, but it
 * is built in the same way: one file, stdlib/sicp/extras.scm, spliced in
 * when imported.  It rides the SRFI machinery under this number, which no
 * SRFI will reach; lib_label names it in messages, it is never a `srfi-N`
 * feature, and its definitions are spelled sicpx--<name>. */
#define SICP_EXTRAS_NUM 100000
typedef struct { int num; int kind; const char *title; const char *file; const char *why; } SrfiRow;
static const SrfiRow SRFI_LIBS[] = {
    {   0, SRFI_NOLIB,      "Feature-based conditional expansion construct", NULL,
        "cond-expand is R7RS syntax, available without an import" },
    {   1, SRFI_LIBRARY,    "List Library", "stdlib/srfi/1.scm", NULL },
    {   2, SRFI_LIBRARY,    "AND-LET*", "stdlib/srfi/2.scm", NULL },
    {   4, SRFI_LIBRARY,    "Homogeneous numeric vector datatypes", "stdlib/srfi/4.scm", NULL },
    {   5, SRFI_NOTYET,     "A compatible let form with signatures and rest arguments", NULL, "S8" },
    {   6, SRFI_BUILTIN,    "Basic String Ports", "stdlib/srfi/6.scm", NULL },
    {   7, SRFI_NOTYET,     "Feature-based program configuration language", NULL, "S8" },
    {   8, SRFI_LIBRARY,    "RECEIVE: Binding to multiple values", "stdlib/srfi/8.scm", NULL },
    {   9, SRFI_BUILTIN,    "Defining Record Types", "stdlib/srfi/9.scm", NULL },
    {  11, SRFI_BUILTIN,    "Syntax for receiving multiple values", "stdlib/srfi/11.scm", NULL },
    {  13, SRFI_LIBRARY,    "String Libraries", "stdlib/srfi/13.scm", NULL },
    {  14, SRFI_LIBRARY,    "Character-set Library", "stdlib/srfi/14.scm", NULL },
    {  16, SRFI_BUILTIN,    "Syntax for procedures of variable arity", "stdlib/srfi/16.scm", NULL },
    {  17, SRFI_LIBRARY,    "Generalized set!", "stdlib/srfi/17.scm", NULL },
    {  18, SRFI_LIBRARY,    "Multithreading support", "stdlib/srfi/18.scm", NULL },
    {  19, SRFI_NOTYET,     "Time Data Types and Procedures", NULL, "S8" },
    {  23, SRFI_BUILTIN,    "Error reporting mechanism", "stdlib/srfi/23.scm", NULL },
    {  25, SRFI_NOTYET,     "Multi-dimensional Array Primitives", NULL, "S8" },
    {  26, SRFI_LIBRARY,    "Notation for Specializing Parameters without Currying", "stdlib/srfi/26.scm", NULL },
    {  27, SRFI_LIBRARY,    "Sources of Random Bits", "stdlib/srfi/27.scm", NULL },
    {  28, SRFI_LIBRARY,    "Basic Format Strings", "stdlib/srfi/28.scm", NULL },
    {  29, SRFI_NOTYET,     "Localization", NULL, "S8" },
    {  30, SRFI_BUILTIN,    "Nested Multi-line Comments", "stdlib/srfi/30.scm", NULL },
    {  31, SRFI_LIBRARY,    "A special form rec for recursive evaluation", "stdlib/srfi/31.scm", NULL },
    {  34, SRFI_BUILTIN,    "Exception Handling for Programs", "stdlib/srfi/34.scm", NULL },
    {  35, SRFI_LIBRARY,    "Conditions", "stdlib/srfi/35.scm", NULL },
    {  38, SRFI_ALIAS,      "External Representation for Data With Shared Structure", "stdlib/srfi/38.scm", NULL },
    {  39, SRFI_BUILTIN,    "Parameter objects", "stdlib/srfi/39.scm", NULL },
    {  40, SRFI_NOTPLANNED, "A Library of Streams", NULL,
        "its author deprecated it in favour of SRFI 41 (Streams); import (srfi 41) instead" },
    {  41, SRFI_LIBRARY,    "Streams", "stdlib/srfi/41.scm", NULL },
    {  42, SRFI_LIBRARY,    "Eager Comprehensions", "stdlib/srfi/42.scm", NULL },
    {  43, SRFI_NOTYET,     "Vector Library", NULL, "S8" },
    {  45, SRFI_ALIAS,      "Primitives for Expressing Iterative Lazy Algorithms", "stdlib/srfi/45.scm", NULL },
    {  48, SRFI_LIBRARY,    "Intermediate Format Strings", "stdlib/srfi/48.scm", NULL },
    {  54, SRFI_NOTYET,     "Formatting", NULL, "S8" },
    {  57, SRFI_NOTYET,     "Records", NULL, "S8" },
    {  59, SRFI_NOTYET,     "Vicinity", NULL, "S8" },
    {  60, SRFI_LIBRARY,    "Integers as Bits", "stdlib/srfi/60.scm", NULL },
    {  61, SRFI_LIBRARY,    "A more general cond clause", "stdlib/srfi/61.scm", NULL },
    {  62, SRFI_NOLIB,      "S-expression comments", NULL,
        "its `#;` datum comments are part of the reader and always on, as in Racket, which has no library for it either; this import can be deleted" },
    {  63, SRFI_NOTYET,     "Homogeneous and Heterogeneous Arrays", NULL, "S8" },
    {  64, SRFI_LIBRARY,    "A Scheme API for test suites", "stdlib/srfi/64.scm", NULL },
    {  66, SRFI_ALIAS,      "Octet Vectors", "stdlib/srfi/66.scm", NULL },
    {  67, SRFI_NOTYET,     "Compare Procedures", NULL, "S8" },
    {  69, SRFI_LIBRARY,    "Basic hash tables", "stdlib/srfi/69.scm", NULL },
    {  71, SRFI_NOTYET,     "Extended LET-syntax for multiple values", NULL, "S8" },
    {  74, SRFI_NOTYET,     "Octet-Addressed Binary Blocks", NULL, "S8" },
    {  78, SRFI_LIBRARY,    "Lightweight testing", "stdlib/srfi/78.scm", NULL },
    {  86, SRFI_NOTYET,     "MU and NU simulating VALUES and CALL-WITH-VALUES", NULL, "S8" },
    {  87, SRFI_BUILTIN,    "=> in case clauses", "stdlib/srfi/87.scm", NULL },
    {  98, SRFI_BUILTIN,    "An interface to access environment variables", "stdlib/srfi/98.scm", NULL },
    { 105, SRFI_NOLIB,      "Curly-infix-expressions", NULL,
        "`{a + b}` reads in every #lang, #lang r7rs included" },
    { 216, SRFI_LIBRARY,    "SICP Prerequisites (Portable)", "stdlib/srfi/216.scm", NULL },
    { SICP_EXTRAS_NUM, SRFI_LIBRARY, "SICP extras", "stdlib/sicp/extras.scm", NULL },
};
#define N_SRFI_LIBS (sizeof(SRFI_LIBS) / sizeof(SRFI_LIBS[0]))
/* r7rs-srfi-18-216-sicp-plan D1: an SRFI whose primitives are C has them in
 * a prelude-shaped file under stdlib/r7rs/, spliced in ahead of the SRFI's
 * own (lib_files_of_set), whose definitions the SRFI calls by their
 * r7rs-...__ names. */
static const struct { int num; const char *file; } SRFI_HELPERS[] = {
    { 18, "stdlib/r7rs/thread.tur" },
};
static const SrfiRow *srfi_row(int64_t num) {
    for (size_t i = 0; i < N_SRFI_LIBS; i++) if (SRFI_LIBS[i].num == num) return &SRFI_LIBS[i];
    return NULL;
}
static bool srfi_importable(const SrfiRow *r) {
    return r && (r->kind == SRFI_BUILTIN || r->kind == SRFI_ALIAS || r->kind == SRFI_LIBRARY);
}
/* `srfi-N` holds in cond-expand: the SRFI is here, importable or always on. */
static bool srfi_supported(const SrfiRow *r) { return srfi_importable(r) || (r && r->kind == SRFI_NOLIB); }
/* The N of a `(srfi N)` library name -- SICP_EXTRAS_NUM for `(sicp
 * extras)` -- or -1 when `set` is not one. */
static int64_t srfi_libname_num(const Form *set) {
    if (!set || set->tag != F_LIST || set->as.list.len != 2) return -1;
    const Form *h = set->as.list.items[0], *n = set->as.list.items[1];
    if (h->tag != F_SYM) return -1;
    if (strcmp(h->as.sym->name, "sicp") == 0)
        return n->tag == F_SYM && strcmp(n->as.sym->name, "extras") == 0 ? SICP_EXTRAS_NUM : -1;
    if (strcmp(h->as.sym->name, "srfi") != 0 || n->tag != F_INT || n->as.i < 0 || n->as.i >= SICP_EXTRAS_NUM) return -1;
    return n->as.i;
}
static bool is_sicp_libname(const Form *set) {
    return set && set->tag == F_LIST && set->as.list.len >= 1 && set->as.list.items[0]->tag == F_SYM &&
           strcmp(set->as.list.items[0]->as.sym->name, "sicp") == 0;
}
static bool is_srfi_libname(const Form *set) {
    return (set && set->tag == F_LIST && set->as.list.len >= 1 && set->as.list.items[0]->tag == F_SYM &&
            strcmp(set->as.list.items[0]->as.sym->name, "srfi") == 0) || is_sicp_libname(set);
}
/* How a message names the library numbered `num`: "(srfi N)" or "(sicp
 * extras)".  Returned by value -- `lib_label(n).s` lives to the end of the
 * full expression -- so one message may name two libraries, and no static
 * buffer is shared (tests/check-static-cname-buffers.sh). */
typedef struct { char s[32]; } LibLabel;
static LibLabel lib_label(int64_t num) {
    LibLabel l;
    if (num == SICP_EXTRAS_NUM) snprintf(l.s, sizeof l.s, "(sicp extras)");
    else snprintf(l.s, sizeof l.s, "(srfi %lld)", (long long)num);
    return l;
}
/* The message for a library name that names no importable library. */
static const char *bad_libname_msg(const Form *set) {
    return is_sicp_libname(set) ? "the SICP library is (sicp extras)" : "an SRFI is named by its number, e.g. (srfi 1)";
}

/* The SCHEME_LIBS row of a `(scheme <x>)` library name, or -1. */
static int scheme_lib_index(const Form *set) {
    if (!set || set->tag != F_LIST || set->as.list.len != 2) return -1;
    const Form *h = set->as.list.items[0], *t = set->as.list.items[1];
    if (h->tag != F_SYM || t->tag != F_SYM || strcmp(h->as.sym->name, "scheme") != 0) return -1;
    for (size_t i = 0; i < N_SCHEME_LIBS; i++)
        if (strcmp(t->as.sym->name, SCHEME_LIBS[i].name) == 0) return (int)i;
    return -1;
}
static bool is_scheme_libname(const Form *set) {
    return set && set->tag == F_LIST && set->as.list.len >= 1 && set->as.list.items[0]->tag == F_SYM &&
           strcmp(set->as.list.items[0]->as.sym->name, "scheme") == 0;
}

/* A growable item buffer for building lists. */
typedef struct FB { Form **items; uint32_t n, cap; } FB;

/* R10 (hygiene): one lexical scope of the user's LOCAL bindings -- the
 * source name and the unique name the lowering gives it.  Frames chain to
 * their parent and live in the arena; a macro keeps the frame it was defined
 * in, so its template's free identifiers can be resolved there (R7RS 4.3.2,
 * referential transparency).  A body's frame is filled after the macros at
 * its start are defined, so they see the body's own defines. */
typedef struct LFrame {
    struct LFrame  *parent;
    const Symbol  **src, **uq;
    uint32_t        n, cap;
} LFrame;

typedef struct SL {
    Arena       *a;
    SymbolTable *st;
    /* Scheme heads. */
    const Symbol *s_define, *s_lambda, *s_let, *s_letstar, *s_letrec,
                 *s_letrecstar, *s_do, *s_begin, *s_set, *s_if, *s_cond,
                 *s_case, *s_and, *s_or, *s_when, *s_unless, *s_case_lambda,
                 *s_define_values, *s_let_values, *s_letstar_values,
                 *s_else, *s_arrow, *s_dot, *s_main, *s_define_syntax,
                 *s_let_syntax, *s_letrec_syntax, *s_import,
                 *s_define_library, *s_define_record_type, *s_quasiquote,
                 *s_unquote, *s_unquote_splicing;
    /* Turmeric heads and markers. */
    const Symbol *t_defn, *t_def, *t_fn, *t_let, *t_letrec, *t_do, *t_if,
                 *t_set, *t_mut, *t_amp, *t_any, *t_int, *t_bool, *t_true,
                 *t_panic;
    /* Prelude names the lowering itself emits. */
    const Symbol *p_eqv, *p_list, *p_length, *p_list_ref, *p_list_tail,
                 *p_chain_to_list, *p_values_ref, *p_values_rest, *p_cons, *p_append, *p_vector,
                 *p_list_to_vector, *p_char, *p_big, *p_ratio, *p_complex, *s_cond_expand, *s_export,
                 *s_include, *t_import, *t_defmodule, *t_export, *t_refer,
                 *t_as, *t_defstruct, *t_heap, *t_is, *t_nil_sym;
    /* R3: an `(import ...)` was lowered, so the program is wrapped in a
     * defmodule (Turmeric's import is only legal there). */
    bool          needs_module;
    /* R3: `(prefix <set> p)` -- a symbol spelled `p<rest>` reads as
     * `<alias>/<rest>`; `(rename <set> (a b))` -- `b` reads as `a`. */
    struct { const char *prefix; size_t plen; const Symbol *alias;
             const struct LibSyntax *lx; /* the library's respelled exports, or NULL */ } prefixes[16];
    uint32_t n_prefixes;
    struct { const Symbol *from, *to; } renames[64];
    uint32_t n_renames;
    /* R7RS 5.2 `except`: a library name the program keeps for itself. */
    const Symbol *excluded[64];
    uint32_t n_excluded;
    /* R10: a program's top-level define whose name an auto-loaded Turmeric
     * stdlib module also defines (`list-length`, from tur/list) -- the
     * program's name is spelled `<name>--user` throughout, so the two do not
     * collide at C level ("already defined by an auto-loaded stdlib
     * module").  The stdlib name is not in the Scheme namespace to begin
     * with; the program's definition is the only one it sees. */
    const Symbol **clash_from, **clash_to;
    uint32_t n_clash, cap_clash;
    bool in_user;   /* lowering the user's forms, not the prelude's */
    /* r7rs-repl-forgets-macros-and-set: a REPL or `eval` turn -- a Scheme
     * form read from a synthetic `<...>` source.  Its top-level variables
     * are all mutable: a LATER turn may `set!` one, and nothing this turn
     * can see says so. */
    bool repl_turn;
    /* r7rs-srfi-plan S2: SRFI 61's `(generator guard => receiver)` cond
     * clause, on in a unit that imports (srfi 61)'s `cond`; SRFI 17's
     * `(set! (f arg ...) v)`, on in a unit that imports (srfi 17)'s `set!`. */
    bool srfi61_cond;
    bool srfi17_set;
    /* R10 (hygiene): the innermost lexical scope, and the identifiers a
     * template inserted that must mean their GLOBAL (or keyword) binding
     * although the use site binds the same name locally: alias -> name. */
    LFrame        *scope;
    const Symbol **ga_from, **ga_to; uint32_t n_ga, cap_ga;
    /* R10: literals and pattern variables of a `syntax-rules` a template
     * inserts are renamed like binders; a literal still MATCHES by its
     * original name (free-identifier=?): renamed -> original. */
    const Symbol **lit_from, **lit_to; uint32_t n_lit, cap_lit;
    /* R3: the import forms produced so far (placed first in the module), and
     * a define-library under construction. */
    FB            imports;
    bool          has_library;
    const Symbol *lib_name;
    FB            lib_exports;
    FB            lib_body;
    /* The top-level stream the form being lowered goes to (the program's,
     * or a library body): where a body's `define-record-type` lifts its
     * declarations (r7rs-define-record-type-not-an-internal-definition). */
    FB           *lift_out;
    bool          user_main;
    /* Rename table, interned. */
    const Symbol *rn_from[N_RENAMES];
    const Symbol *rn_to[N_RENAMES];
    /* Every `set!` target in the unit (a per-name over-approximation). */
    const Symbol **muts;
    uint32_t       n_muts, cap_muts;
    /* toplevel-def-initializers-run-before-toplevel-expressions: set once a
     * program's top-level EXPRESSION has been lowered.  A `define` after it
     * runs its initializer as a statement in source order (a `set!` in the
     * program body) instead of a module-level `def`, whose initializers all
     * run before the body. */
    bool           toplevel_expr_seen;
    uint32_t       next_tmp;
    /* R4: the syntax-rules macros in scope, innermost last.  A body or a
     * let-syntax records n_macros on entry and restores it on exit; lookup
     * walks from the end so an inner definition shadows an outer one. */
    struct SMacro **macros;
    uint32_t        n_macros, cap_macros;
    /* R4: macros whose templates set! a pattern variable (see collect_muts). */
    const Symbol  **setters;
    uint32_t        n_setters, cap_setters;
    uint32_t        expand_depth;
    const Symbol   *s_syntax_rules, *s_ellipsis, *s_underscore, *s_syntax_error,
                   *s_quote, *s_er_macro_transformer;
    /* R6: the control forms. */
    const Symbol   *s_guard, *s_parameterize, *s_delay, *s_delay_force;
    /* R7: which SCHEME_LIBS rows this unit has imported. */
    bool            lib_imported[N_SCHEME_LIBS];
    /* Which (scheme ...) libraries the program's own import forms name,
     * known before lowering sets lib_imported (r7rs-redefining-eval-with-
     * scheme-eval-fails-to-compile). */
    bool            lib_named[N_SCHEME_LIBS];
    /* r7rs-saved-standard-procedure-follows-redefinition: a standard name the
     * program redefines, with the index of the top-level form that does it.
     * A reference evaluated EAGERLY (deferred == 0) by an earlier top-level
     * form (cur_top < the index) means the standard procedure -- the one
     * bound when it runs -- so `(define saved apply)` ahead of the program's
     * own `apply` keeps R7RS's.  Lambda and promise bodies run later and see
     * the program's definition, as before. */
    const Symbol  **early_sym;
    uint32_t       *early_at;
    uint32_t        n_early, cap_early;
    uint32_t        cur_top;
    uint32_t        deferred;
    bool            eager_thunk;   /* the next lambda body runs now (a top-level statement) */
    const Symbol   *od_from[N_ONDEMAND], *od_to[N_ONDEMAND];
    int             od_lib[N_ONDEMAND];
    /* R5: the nine numeric operators -- the Scheme spelling, the binary
     * prelude helper a call folds onto, and the variadic prelude procedure a
     * bare operator in value position names. */
    const Symbol   *ops[9], *ops_bin[9], *ops_val[9];
    /* r7rs-define-library-cannot-export-syntax: where an imported library's
     * source is (the module loader's search), and what was read from each
     * library this pass imported. */
    SchemeLibResolveFn   lib_resolve;
    void                *lib_resolve_ud;
    SchemeGlobalFn       global_kind;      /* the joined environment's globals */
    struct LibSyntax   **libsyn;
    uint32_t             n_libsyn, cap_libsyn;
    /* r7rs-srfi-plan S1: what each `(import (srfi N))` bound -- the visible
     * name, what it means, and the SRFI it came from (for D5's messages) --
     * the SRFI files read this pass, and the two facts about the unit D5's
     * checks need: its import sets of (scheme base), and its own top-level
     * definitions. */
    const Symbol       **srfi_from, **srfi_to;
    int64_t             *srfi_by;
    uint32_t             n_srfi, cap_srfi;
    struct SrfiLib     **srfilib;
    uint32_t             n_srfilib, cap_srfilib;
    FB                   base_sets;
    FB                   user_globals;
    /* r7rs-turmeric-syntax-leaks item 8: a user Scheme source sees the
     * auto-loaded stdlib only through `(turmeric stdlib/<file>)`.  The
     * stdlib's globals and the file each is from (from the stream's own
     * stdlib forms, else stdlib_file), and what this unit's imports made
     * visible: names one by one (`only`), or whole files less the names an
     * `except` or `rename` took away. */
    SchemeStdlibFileFn   stdlib_file;
    const Symbol       **std_keys;
    const Symbol       **std_files;
    uint32_t             std_cap, std_n;
    const Symbol       **granted;
    uint32_t             n_granted, cap_granted;
    const Symbol        *granted_files[32];
    uint32_t             n_granted_files;
    const Symbol       **denied;
    uint32_t             n_denied, cap_denied;
} SL;

static const Symbol *I(SL *sl, const char *s) {
    return symtab_intern(sl->st, strslice(s, (uint32_t)strlen(s)));
}

/* r7rs-turmeric-syntax-leaks item 8: the stdlib-name table (open addressing
 * on the interned Symbol's address) and the grant lists. */
static uint32_t std_slot(const SL *sl, const Symbol *s) {
    uintptr_t h = (uintptr_t)s;
    h ^= h >> 17; h *= (uintptr_t)0x9E3779B97F4A7C15ULL; h ^= h >> 29;
    return (uint32_t)h & (sl->std_cap - 1);
}
static void std_put(SL *sl, const Symbol *s, const Symbol *file) {
    if (sl->std_n * 2 >= sl->std_cap) {
        uint32_t oc = sl->std_cap;
        const Symbol **ok = sl->std_keys, **of = sl->std_files;
        sl->std_cap = oc ? oc * 2 : 1024;
        sl->std_keys = (const Symbol **)calloc(sl->std_cap, sizeof(Symbol *));
        sl->std_files = (const Symbol **)calloc(sl->std_cap, sizeof(Symbol *));
        if (!sl->std_keys || !sl->std_files) { fprintf(stderr, "tur: oom\n"); abort(); }
        sl->std_n = 0;
        for (uint32_t i = 0; i < oc; i++) if (ok[i]) std_put(sl, ok[i], of[i]);
        free((void *)ok); free((void *)of);
    }
    uint32_t i = std_slot(sl, s);
    while (sl->std_keys[i] && sl->std_keys[i] != s) i = (i + 1) & (sl->std_cap - 1);
    if (!sl->std_keys[i]) sl->std_n++;
    sl->std_keys[i] = s;
    sl->std_files[i] = file;
}
static bool stdlib_autoloaded(const char *name);
/* The auto-loaded stdlib file `s` is a global of, or NULL. */
static const Symbol *std_file_of(SL *sl, const Symbol *s) {
    if (sl->std_cap) {
        uint32_t i = std_slot(sl, s);
        while (sl->std_keys[i]) {
            if (sl->std_keys[i] == s) return sl->std_files[i];
            i = (i + 1) & (sl->std_cap - 1);
        }
    }
    if (sl->stdlib_file) {
        char buf[128];
        if (sl->stdlib_file(sl->lib_resolve_ud, s->name, buf, sizeof buf) && stdlib_autoloaded(buf))
            return I(sl, buf);
    }
    return NULL;
}
static void sym_list_push(const Symbol ***items, uint32_t *n, uint32_t *cap, const Symbol *s) {
    for (uint32_t i = 0; i < *n; i++) if ((*items)[i] == s) return;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *items = (const Symbol **)realloc((void *)*items, *cap * sizeof(Symbol *));
        if (!*items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    (*items)[(*n)++] = s;
}
static bool sym_list_has(const Symbol *const *items, uint32_t n, const Symbol *s) {
    for (uint32_t i = 0; i < n; i++) if (items[i] == s) return true;
    return false;
}

static void sl_init(SL *sl, Arena *a, SymbolTable *st) {
    memset(sl, 0, sizeof *sl);
    sl->a = a; sl->st = st;
    sl->s_define = I(sl, "define");     sl->s_lambda = I(sl, "lambda");
    sl->s_let = I(sl, "let");           sl->s_letstar = I(sl, "let*");
    sl->s_letrec = I(sl, "letrec");     sl->s_letrecstar = I(sl, "letrec*");
    sl->s_do = I(sl, "do");             sl->s_begin = I(sl, "begin");
    sl->s_set = I(sl, "set!");          sl->s_if = I(sl, "if");
    sl->s_cond = I(sl, "cond");         sl->s_case = I(sl, "case");
    sl->s_and = I(sl, "and");           sl->s_or = I(sl, "or");
    sl->s_when = I(sl, "when");         sl->s_unless = I(sl, "unless");
    sl->s_case_lambda = I(sl, "case-lambda");
    sl->s_define_values = I(sl, "define-values");
    sl->s_let_values = I(sl, "let-values");
    sl->s_letstar_values = I(sl, "let*-values");
    sl->s_else = I(sl, "else");         sl->s_arrow = I(sl, "=>");
    sl->s_dot = I(sl, ".");             sl->s_main = I(sl, "main");
    sl->s_define_syntax = I(sl, "define-syntax");
    sl->s_let_syntax = I(sl, "let-syntax");
    sl->s_letrec_syntax = I(sl, "letrec-syntax");
    sl->s_import = I(sl, "import");
    sl->s_define_library = I(sl, "define-library");
    sl->s_define_record_type = I(sl, "define-record-type");
    sl->s_quasiquote = I(sl, "quasiquote");
    sl->s_unquote = I(sl, "unquote");
    sl->s_unquote_splicing = I(sl, "unquote-splicing");
    sl->s_syntax_rules = I(sl, "syntax-rules");
    sl->s_ellipsis = I(sl, "...");
    sl->s_underscore = I(sl, "_");
    sl->s_syntax_error = I(sl, "syntax-error");
    sl->s_quote = I(sl, "quote");
    sl->s_er_macro_transformer = I(sl, "er-macro-transformer");
    sl->s_guard = I(sl, "guard");
    sl->s_parameterize = I(sl, "parameterize");
    sl->s_delay = I(sl, "delay");
    sl->s_delay_force = I(sl, "delay-force");
    {
        static const char *const OPS[9][3] = {
            { "+",  "r7rs-add2__",   "r7rs-+"  }, { "-",  "r7rs-sub2__",   "r7rs--"  },
            { "*",  "r7rs-mul2__",   "r7rs-*"  }, { "/",  "r7rs-div2__",   "r7rs-/"  },
            { "=",  "r7rs-numeq2__", "r7rs-="  }, { "<",  "r7rs-lt2__",    "r7rs-<"  },
            { ">",  "r7rs-gt2__",    "r7rs->"  }, { "<=", "r7rs-le2__",    "r7rs-<=" },
            { ">=", "r7rs-ge2__",    "r7rs->=" },
        };
        for (int i = 0; i < 9; i++) {
            sl->ops[i] = I(sl, OPS[i][0]);
            sl->ops_bin[i] = I(sl, OPS[i][1]);
            sl->ops_val[i] = I(sl, OPS[i][2]);
        }
    }

    sl->t_defn = I(sl, "defn");   sl->t_def = I(sl, "def");
    sl->t_fn = I(sl, "fn");       sl->t_let = I(sl, "let");
    sl->t_letrec = I(sl, "letrec"); sl->t_do = I(sl, "do");
    sl->t_if = I(sl, "if");       sl->t_set = I(sl, "set!");
    sl->t_mut = I(sl, "^mut");    sl->t_amp = I(sl, "&");
    sl->t_any = I(sl, "any");     sl->t_int = I(sl, "int");
    sl->t_bool = I(sl, "bool");   sl->t_true = I(sl, "true");
    sl->t_panic = I(sl, "panic");

    sl->p_eqv = I(sl, "r7rs-eqv?");       sl->p_list = I(sl, "r7rs-list");
    sl->p_length = I(sl, "r7rs-length");  sl->p_list_ref = I(sl, "r7rs-list-ref");
    sl->p_list_tail = I(sl, "r7rs-list-tail");
    sl->p_chain_to_list = I(sl, "r7rs-chain->list__");
    sl->p_values_ref = I(sl, "r7rs-values-ref");
    sl->p_values_rest = I(sl, "r7rs-values-rest");
    sl->p_cons = I(sl, "r7rs-cons");         sl->p_append = I(sl, "r7rs-append2__");
    sl->p_vector = I(sl, "r7rs-vector");     sl->p_list_to_vector = I(sl, "r7rs-list->vector");
    sl->p_char = I(sl, "r7rs-char__");
    sl->p_big = I(sl, "r7rs-big__");
    sl->p_ratio = I(sl, "r7rs-ratio__");
    sl->p_complex = I(sl, "r7rs-complex__");
    sl->s_cond_expand = I(sl, "cond-expand"); sl->s_export = I(sl, "export");
    sl->s_include = I(sl, "include");
    sl->t_import = I(sl, "import");          sl->t_defmodule = I(sl, "defmodule");
    sl->t_export = I(sl, "export");          sl->t_refer = I(sl, "refer");
    sl->t_as = I(sl, "as");                  sl->t_defstruct = I(sl, "defstruct");
    sl->t_heap = I(sl, "heap");              sl->t_is = I(sl, "is?");
    sl->t_nil_sym = I(sl, "nil");

    for (size_t i = 0; i < N_ONDEMAND; i++) {
        sl->od_from[i] = I(sl, ONDEMAND[i][0]);
        sl->od_to[i]   = I(sl, ONDEMAND[i][1]);
        sl->od_lib[i]  = -1;
        for (size_t j = 0; j < N_SCHEME_LIBS; j++)
            if (strcmp(SCHEME_LIBS[j].name, ONDEMAND[i][2]) == 0) sl->od_lib[i] = (int)j;
    }
    for (size_t i = 0; i < N_RENAMES; i++) {
        sl->rn_from[i] = I(sl, RENAMES[i][0]);
        sl->rn_to[i]   = I(sl, RENAMES[i][1]);
    }
}

/* --- Form helpers ---------------------------------------------------------- */

static Form *Sym(SL *sl, Span sp, const Symbol *s) { return form_sym(sl->a, sp, s); }
/* `:refer`, `:as`, `:heap` -- the reader spells these as keywords, so the
 * elaborator expects F_KEYWORD, not a symbol whose name starts with ':'. */
static Form *Kw(SL *sl, Span sp, const Symbol *s)  { return form_keyword(sl->a, sp, s); }
static Form *Nil(SL *sl, Span sp)                  { return form_nil(sl->a, sp); }
static Form *Int(SL *sl, Span sp, int64_t v)       { return form_int(sl->a, sp, v); }
static Form *Bool(SL *sl, Span sp, bool v)         { return form_bool(sl->a, sp, v); }

static Form *List(SL *sl, Span sp, Form **items, uint32_t n) {
    return form_list(sl->a, sp, items, n);
}
static Form *Vec(SL *sl, Span sp, Form **items, uint32_t n) {
    return form_vec(sl->a, sp, items, n);
}
/* A list from a fixed set of items. */
static Form *Ln(SL *sl, Span sp, int n, ...) {
    Form *items[16];
    va_list ap; va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) items[i] = va_arg(ap, Form *);
    va_end(ap);
    return List(sl, sp, items, (uint32_t)n);
}
/* `: any`, the way the reader spells a spaced annotation. */
static Form *AnyAnn(SL *sl, Span sp) {
    return form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_any));
}

/* The reader spells `()` as F_NIL; a binding list, formals list or do-spec
 * list written empty (or produced empty by a syntax-rules template) is an
 * empty list to the forms below. */
static Form *nil_to_list(SL *sl, Form *f) {
    if (f && f->tag == F_NIL) return List(sl, f->span, NULL, 0);
    return f;
}

static bool is_sym(const Form *f, const Symbol *s) {
    return f && f->tag == F_SYM && f->as.sym == s;
}
static bool head_is(const Form *f, const Symbol *s) {
    return f && f->tag == F_LIST && f->as.list.len > 0 && is_sym(f->as.list.items[0], s);
}
static bool is_scheme_file(const Form *f) {
    const SourceFile *sf = f ? diag_source_file(f->span.file_id) : NULL;
    return sf != NULL && sf->lang == LANG_R7RS;
}

static void fb_push(FB *b, Form *f) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->items = (Form **)realloc(b->items, b->cap * sizeof(Form *));
        if (!b->items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    b->items[b->n++] = f;
}
static Form *fb_list(SL *sl, FB *b, Span sp) {
    Form *f = List(sl, sp, b->items, b->n);
    free(b->items);
    return f;
}
static Form *fb_vec(SL *sl, FB *b, Span sp) {
    Form *f = Vec(sl, sp, b->items, b->n);
    free(b->items);
    return f;
}

/* A fresh symbol no source file has used: the same symbol-table check gensym
 * makes, so a lowering temporary can never capture a user name. */
static const Symbol *fresh(SL *sl, const char *prefix) {
    char buf[64];
    for (;;) {
        snprintf(buf, sizeof buf, "%s%u", prefix, sl->next_tmp++);
        StrSlice s = strslice(buf, (uint32_t)strlen(buf));
        if (!symtab_contains(sl->st, s)) return symtab_intern(sl->st, s);
    }
}

static void err(const Form *at, const char *fmt, ...) TUR_PRINTF_FMT(2, 3);
static void err(const Form *at, const char *fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_emit(DIAG_ERROR, at ? at->span : SPAN_UNKNOWN, "%s", msg);
}

/* --- set! targets ---------------------------------------------------------- */

static void note_mut(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_muts; i++) if (sl->muts[i] == s) return;
    if (sl->n_muts == sl->cap_muts) {
        sl->cap_muts = sl->cap_muts ? sl->cap_muts * 2 : 16;
        sl->muts = (const Symbol **)realloc((void *)sl->muts,
                                            sl->cap_muts * sizeof(const Symbol *));
        if (!sl->muts) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->muts[sl->n_muts++] = s;
}
static bool is_mut(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_muts; i++) if (sl->muts[i] == s) return true;
    /* The scan records the target as written; a global renamed for a clash
     * with a Turmeric form (`return` -> `return--user`) is asked for by its
     * renamed spelling. */
    for (uint32_t i = 0; i < sl->n_clash; i++)
        if (sl->clash_to[i] == s)
            for (uint32_t j = 0; j < sl->n_muts; j++) if (sl->muts[j] == sl->clash_from[i]) return true;
    return false;
}
/* R4: the mutability scan runs BEFORE expansion (a top-level `define` is
 * lowered as `def` or `def ^mut` before any later form is looked at), so a
 * `set!` a template performs has to be accounted for syntactically.  A
 * `(set! v ...)` in a syntax-rules template whose target is one of the rule's
 * pattern variables marks the macro as a SETTER of its arguments: every
 * symbol among a use's arguments is then a set! target (per-name, so it is
 * the same over-approximation the scan already makes); a template that sets
 * a name of its own (`(set! counter ...)`) marks that name directly.  Scoping
 * is ignored here on purpose -- an over-approximation by name is safe. */
static void note_setter_macro(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_setters; i++) if (sl->setters[i] == s) return;
    if (sl->n_setters == sl->cap_setters) {
        sl->cap_setters = sl->cap_setters ? sl->cap_setters * 2 : 8;
        sl->setters = (const Symbol **)realloc((void *)sl->setters,
                                               sl->cap_setters * sizeof(const Symbol *));
        if (!sl->setters) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->setters[sl->n_setters++] = s;
}
static bool is_setter_macro(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_setters; i++) if (sl->setters[i] == s) return true;
    return false;
}
static bool form_mentions_sym(const Form *f, const Symbol *s) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == s;
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_mentions_sym(f->as.list.items[i], s)) return true;
    return false;
}
/* Does `f` (a lowered form) use `s` as a value -- anywhere but at the head
 * of a call?  Quoted data does not count. */
static bool form_uses_as_value(const Form *f, const Symbol *s) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == s;
    if (f->tag == F_QUOTE) return false;
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++) {
            const Form *x = f->as.list.items[i];
            if (i == 0 && f->tag == F_LIST && x->tag == F_SYM) continue;   /* the callee */
            if (form_uses_as_value(x, s)) return true;
        }
    return false;
}
static void note_all_syms(SL *sl, const Form *f) {
    if (!f) return;
    if (f->tag == F_SYM) { note_mut(sl, f->as.sym); return; }
    if (f->tag == F_QUOTE) return;
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++) note_all_syms(sl, f->as.list.items[i]);
}
/* Walk a template for set! forms; `pattern` is the rule's pattern. */
static void scan_template_sets(SL *sl, const Symbol *macro, const Form *pattern, const Form *t) {
    if (!t) return;
    if (t->tag == F_QUOTE) return;
    if (t->tag != F_LIST && t->tag != F_VEC) return;
    if (t->tag == F_LIST && t->as.list.len == 3 && is_sym(t->as.list.items[0], sl->s_set) &&
        t->as.list.items[1]->tag == F_SYM) {
        const Symbol *tgt = t->as.list.items[1]->as.sym;
        if (form_mentions_sym(pattern, tgt)) note_setter_macro(sl, macro);
        else note_mut(sl, tgt);
    }
    for (uint32_t i = 0; i < t->as.list.len; i++) scan_template_sets(sl, macro, pattern, t->as.list.items[i]);
}
static void scan_syntax_rules(SL *sl, const Symbol *macro, const Form *spec) {
    if (!head_is(spec, sl->s_syntax_rules)) return;
    for (uint32_t i = 1; i < spec->as.list.len; i++) {
        const Form *rule = spec->as.list.items[i];
        if (rule->tag == F_LIST && rule->as.list.len == 2 && rule->as.list.items[0]->tag == F_LIST)
            scan_template_sets(sl, macro, rule->as.list.items[0], rule->as.list.items[1]);
    }
}
static void collect_setter_macros(SL *sl, const Form *f) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return;
    if (head_is(f, sl->s_define_syntax) && f->as.list.len == 3 && f->as.list.items[1]->tag == F_SYM)
        scan_syntax_rules(sl, f->as.list.items[1]->as.sym, f->as.list.items[2]);
    else if ((head_is(f, sl->s_let_syntax) || head_is(f, sl->s_letrec_syntax)) &&
             f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST) {
        const Form *bl = f->as.list.items[1];
        for (uint32_t i = 0; i < bl->as.list.len; i++) {
            const Form *b = bl->as.list.items[i];
            if (b->tag == F_LIST && b->as.list.len == 2 && b->as.list.items[0]->tag == F_SYM)
                scan_syntax_rules(sl, b->as.list.items[0]->as.sym, b->as.list.items[1]);
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) collect_setter_macros(sl, f->as.list.items[i]);
}

static void collect_muts(SL *sl, const Form *f) {
    if (!f) return;
    switch (f->tag) {
        case F_QUOTE: return;
        case F_LIST: case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE:
        case F_UNQUOTE_SPLICING: case F_MAP: case F_SET: case F_MAP_LITERAL:
        case F_SET_LITERAL:
            if (f->tag == F_LIST && f->as.list.len == 3 &&
                is_sym(f->as.list.items[0], sl->s_set) &&
                f->as.list.items[1]->tag == F_SYM)
                note_mut(sl, f->as.list.items[1]->as.sym);
            if (f->tag == F_LIST && f->as.list.len >= 2 && f->as.list.items[0]->tag == F_SYM &&
                is_setter_macro(sl, f->as.list.items[0]->as.sym))
                for (uint32_t i = 1; i < f->as.list.len; i++) note_all_syms(sl, f->as.list.items[i]);
            for (uint32_t i = 0; i < f->as.list.len; i++)
                collect_muts(sl, f->as.list.items[i]);
            return;
        default: return;
    }
}

/* r7rs-procedure-body-forward-reference: R7RS 5.3.1 lets a procedure body
 * name a top-level variable whose define comes later in the program, as long
 * as it is defined by the time the body runs:
 *
 *     (define (f) (* y 2))
 *     (define y 21)
 *
 * A `(define (f) ...)` is a defn, which the elaborator pre-declares, but a
 * plain variable is declared where its `def` stands, so `y` was unbound in
 * `f`'s body.  This scan finds every top-level variable define that a form
 * BEFORE it mentions (by name, anywhere but under a quote -- the same
 * over-approximation the set! scan makes) and marks it a set! target: its
 * def is then `(def ^mut y : any init)`, the one shape the elaborator's
 * Pass 1 pre-declares ahead of the bodies (elab_pre_declare_any_mut_def),
 * on the program and on a library body alike.  The initializer still runs
 * where the define stands.  A `(define f (lambda ...))` is left alone: it
 * becomes a defn (the define arm) and forward-references already. */
static struct SMacro *sr_lookup(SL *sl, const Symbol *s);
static bool form_mentions_sym_unquoted(const Form *f, const Symbol *s) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == s;
    if (f->tag == F_QUOTE) return false;
    if (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_QUASIQUOTE ||
        f->tag == F_UNQUOTE || f->tag == F_UNQUOTE_SPLICING)
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (form_mentions_sym_unquoted(f->as.list.items[i], s)) return true;
    return false;
}
/* The variable defines of one form (a `define`, or a `begin` of them). */
static void forward_scan_form(SL *sl, const Form *f, Form *const *before, uint32_t n_before) {
    if (!f || f->tag != F_LIST) return;
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) forward_scan_form(sl, f->as.list.items[i], before, n_before);
        return;
    }
    if (!head_is(f, sl->s_define) || f->as.list.len != 3 || f->as.list.items[1]->tag != F_SYM) return;
    const Form *init = f->as.list.items[2];
    if (init->tag == F_LIST && init->as.list.len >= 3 && is_sym(init->as.list.items[0], sl->s_lambda) &&
        !sr_lookup(sl, sl->s_lambda))
        return;
    /* A case-lambda define is a defn too (the define arm). */
    if (init->tag == F_LIST && init->as.list.len >= 1 && is_sym(init->as.list.items[0], sl->s_case_lambda) &&
        !sr_lookup(sl, sl->s_case_lambda))
        return;
    const Symbol *name = f->as.list.items[1]->as.sym;
    if (is_mut(sl, name)) return;
    for (uint32_t j = 0; j < n_before; j++)
        if (form_mentions_sym_unquoted(before[j], name)) { note_mut(sl, name); return; }
    /* r7rs-toplevel-define-refers-to-itself: so does a define whose own
     * initializer names it -- under a delay, a lambda or a stream-cons, as
     * SRFI 41's `(define nats (stream-cons 0 (stream-map add1 nats)))` does.
     * A plain def cannot see the global it defines ("unbound symbol"); the
     * pre-declared `^mut` one can, and the reference runs after the store.
     * A body's define does the same with a cell (lower_body_inner). */
    if (form_mentions_sym_unquoted(init, name)) note_mut(sl, name);
}
static void note_forward_defs(SL *sl, Form *const *forms, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) forward_scan_form(sl, forms[i], forms, i);
}

/* --- renaming -------------------------------------------------------------- */

/* ---- R10: lexical scope (hygiene) ------------------------------------------ */

static const Symbol *scope_lookup(const LFrame *f, const Symbol *s) {
    for (; f; f = f->parent)
        for (int32_t i = (int32_t)f->n - 1; i >= 0; i--)
            if (f->src[i] == s) return f->uq[i];
    return NULL;
}
/* Open a scope; returns the one to restore with scope_close. */
static LFrame *scope_open(SL *sl) {
    LFrame *saved = sl->scope;
    LFrame *f = (LFrame *)arena_alloc(sl->a, sizeof(LFrame));
    memset(f, 0, sizeof *f);
    f->parent = saved;
    sl->scope = f;
    return saved;
}
static void scope_close(SL *sl, LFrame *saved) { sl->scope = saved; }
static const Symbol *global_alias_orig(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_ga; i++) if (sl->ga_from[i] == s) return sl->ga_to[i];
    return NULL;
}
static const Symbol *lit_orig(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_lit; i++) if (sl->lit_from[i] == s) return sl->lit_to[i];
    return s;
}
/* The name an identifier was written as: a template's global alias is its
 * original (quoted data, literal matching), anything else itself. */
static const Symbol *ident_orig(const SL *sl, const Symbol *s) {
    const Symbol *g = global_alias_orig(sl, s);
    return g ? g : s;
}
static void sym_pair_push(SL *sl, const Symbol ***from, const Symbol ***to, uint32_t *n, uint32_t *cap,
                          const Symbol *a, const Symbol *b) {
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 16;
        const Symbol **nf = (const Symbol **)arena_alloc(sl->a, nc * sizeof(Symbol *));
        const Symbol **nt = (const Symbol **)arena_alloc(sl->a, nc * sizeof(Symbol *));
        if (*n) { memcpy((void *)nf, (void *)*from, *n * sizeof(Symbol *)); memcpy((void *)nt, (void *)*to, *n * sizeof(Symbol *)); }
        *from = nf; *to = nt; *cap = nc;
    }
    (*from)[*n] = a; (*to)[*n] = b; (*n)++;
}
static bool prelude_span(Span sp);
/* Declare a local binder in the innermost scope: its unique name.  The
 * prelude is written against its own names and is never renamed. */
/* r7rs-turmeric-syntax-leaks item 4: `^` is an R7RS <initial>, but a
 * Turmeric name starting with it is an annotation (`^mut`), so a Scheme
 * identifier `^x` is spelled `__scheme_caret_x` wherever it is bound or used
 * (quoted, it is still the symbol `^x`). */
static const Symbol *caret_spelling(SL *sl, const Symbol *s) {
    char buf[256];
    snprintf(buf, sizeof buf, SCHEME_CARET_PREFIX "%s", s->name + 1);
    return I(sl, buf);
}
static const Symbol *bind_name(SL *sl, const Symbol *s, Span sp) {
    if (!sl->scope || prelude_span(sp)) return s;
    char pre[160];
    if (s->name[0] == '^')
        snprintf(pre, sizeof pre, SCHEME_CARET_PREFIX "%s__v", s->name + 1);
    else
        snprintf(pre, sizeof pre, "%s__v", s->name);
    const Symbol *u = fresh(sl, pre);
    sym_pair_push(sl, &sl->scope->src, &sl->scope->uq, &sl->scope->n, &sl->scope->cap, s, u);
    return u;
}
static const Symbol *rn_global(SL *sl, const Symbol *s);
static const Symbol *rn(SL *sl, const Symbol *s) {
    /* A local first.  A template's global alias is never a local -- except
     * one a later expansion step bound (hyg_pending): then it is that
     * binding, so looking the alias itself up first is right for both. */
    if (sl->scope) {
        const Symbol *u = scope_lookup(sl->scope, s);
        if (u) return u;
    }
    const Symbol *g = global_alias_orig(sl, s);
    if (g) return rn_global(sl, g);
    return rn_global(sl, s);
}
/* A keyword test that respects scope: `f` is the identifier `kw` and not a
 * local variable that shadows it (`(let ((=> #f)) (cond (#t => 'ok)))`), or
 * a template's alias of it. */
static bool kw_is(SL *sl, const Form *f, const Symbol *kw) {
    if (!f || f->tag != F_SYM) return false;
    const Symbol *g = global_alias_orig(sl, f->as.sym);
    if (g) return g == kw && !(sl->scope && scope_lookup(sl->scope, f->as.sym));
    return f->as.sym == kw && !(sl->scope && scope_lookup(sl->scope, kw));
}

/* A Scheme global named like a Turmeric builtin type (`any`, `int`, `ptr`):
 * spelled `<name>--user` wherever user code names it, or the stdlib's type
 * annotations would read the program's definition.  R7RS has no such names,
 * so in user code the identifier can only mean a Scheme definition -- the
 * program's, or a library's -- and never the type.  One rule, applied alike
 * in the defining library and in every importer (by name, `only`, `rename`,
 * `prefix`), needs no knowledge of which library defines it.  NULL for any
 * other name. */
static const Symbol *type_named_global(SL *sl, const Symbol *s) {
    if (typekind_from_symbol(s->name) == TY_UNKNOWN) return NULL;
    char buf[256];
    snprintf(buf, sizeof buf, "%s--user", s->name);
    return I(sl, buf);
}

static const Symbol *rn_std(SL *sl, const Symbol *s);
static const Symbol *caret_spelling(SL *sl, const Symbol *s);
struct LibSyntax;
static const Symbol *lib_respelling(SL *sl, const struct LibSyntax *lx, const Symbol *pub);
static const Symbol *rn_global(SL *sl, const Symbol *s) {
    /* redefinitions_to_set's name for a standard binding the program
     * redefines: what the bare name means with no program definition. */
    if (s->len > 7 && memcmp(s->name, "--std--", 7) == 0) return rn_std(sl, I(sl, s->name + 7));
    for (uint32_t i = 0; i < sl->n_renames; i++)
        if (sl->renames[i].from == s) return sl->renames[i].to;
    /* r7rs-turmeric-syntax-leaks item 4: a `^` identifier, bound or not. */
    if (sl->in_user && s->name[0] == '^' && s->len > 1) return caret_spelling(sl, s);
    /* r7rs-srfi-plan S1: a name an `(import (srfi N))` bound, in user code
     * only -- an SRFI's own body is spelled onto its targets already. */
    if (sl->in_user)
        for (uint32_t i = 0; i < sl->n_srfi; i++)
            if (sl->srfi_from[i] == s) return sl->srfi_to[i];
    if (sl->in_user)
        for (uint32_t i = 0; i < sl->n_clash; i++)
            if (sl->clash_from[i] == s) return sl->clash_to[i];
    if (sl->in_user && sl->global_kind) {
        /* A REPL turn: `square` an earlier turn defined for itself is
         * `square--user`, and still means that definition here. */
        char buf[256];
        snprintf(buf, sizeof buf, "%s--user", s->name);
        if (sl->global_kind(sl->lib_resolve_ud, buf) == SCHEME_GLOBAL_EARLIER_TURN) return I(sl, buf);
    }
    if (sl->in_user) {
        const Symbol *t = type_named_global(sl, s);
        if (t) return t;
    }
    for (uint32_t i = 0; i < sl->n_prefixes; i++) {
        if (s->len > sl->prefixes[i].plen &&
            memcmp(s->name, sl->prefixes[i].prefix, sl->prefixes[i].plen) == 0) {
            if (!sl->prefixes[i].alias) {
                /* Alias-less (a global library): the prefix comes off, and the
                 * bare name resolves as written -- through a rename inside
                 * the prefix, `(prefix (rename (scheme base) (car kar)) p:)`,
                 * else as the library's own name, whatever an SRFI's import
                 * or an `except` made the bare name mean (D5). */
                const Symbol *bare = I(sl, s->name + sl->prefixes[i].plen);
                for (uint32_t r = 0; r < sl->n_renames; r++)
                    if (sl->renames[r].from == bare) return sl->renames[r].to;
                return rn_std(sl, bare);
            }
            /* The rest is the module's spelling: a form-named export was
             * renamed in the library (`gen` -> `gen--user`), so it is here. */
            const Symbol *rest = I(sl, s->name + sl->prefixes[i].plen);
            /* r7rs-library-defines-standard-or-stdlib-name: the library
             * spelled a standard or stdlib name it defines `<name>--user`. */
            const Symbol *lib_sp = lib_respelling(sl, sl->prefixes[i].lx, rest);
            if (lib_sp) rest = lib_sp;
            else if (sl->in_user)
                for (uint32_t c = 0; c < sl->n_clash; c++)
                    if (sl->clash_from[c] == rest) { rest = sl->clash_to[c]; break; }
            if (sl->in_user && type_named_global(sl, rest)) rest = type_named_global(sl, rest);
            char buf[256];
            snprintf(buf, sizeof buf, "%s/%s", sl->prefixes[i].alias->name, rest->name);
            return I(sl, buf);
        }
    }
    for (uint32_t i = 0; i < sl->n_excluded; i++)
        if (sl->excluded[i] == s) return s;   /* (except ...): the program's own */
    for (size_t i = 0; i < N_RENAMES; i++)
        if (sl->rn_from[i] == s) return sl->rn_to[i];
    /* R7: an on-demand library's name means its procedure only in a unit
     * that imported the library (the library file is not loaded otherwise,
     * and the name stays free for the program's own use). */
    for (size_t i = 0; i < N_ONDEMAND; i++)
        if (sl->od_from[i] == s && sl->od_lib[i] >= 0 && sl->lib_imported[sl->od_lib[i]])
            return sl->od_to[i];
    return s;
}

/* What a (scheme ...) library's own name means: its procedure, whatever the
 * unit's `except`s did to the bare name. */
static const Symbol *rn_std(SL *sl, const Symbol *s) {
    for (size_t i = 0; i < N_RENAMES; i++)
        if (sl->rn_from[i] == s) return sl->rn_to[i];
    for (size_t i = 0; i < N_ONDEMAND; i++)
        if (sl->od_from[i] == s && sl->od_lib[i] >= 0 && sl->lib_imported[sl->od_lib[i]])
            return sl->od_to[i];
    return rn(sl, s);
}

static bool is_char_form(SL *sl, const Form *f);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp);
static Form *lower(SL *sl, Form *f);

/* --- R4: syntax-rules ------------------------------------------------------ */
/*
 * A `syntax-rules` transformer is a pattern matcher plus a template
 * instantiator over forms, and it lives HERE, in the Form -> Form lowering,
 * rather than on the `defmacro*` / `Syntax` substrate D5 named.  The reason
 * is ordering, not taste: a `defmacro*` runs inside elaboration, AFTER this
 * pass, and its output would be Scheme forms (`let`, `cond`, a named `let`)
 * that nothing would lower.  An expansion has to happen where the lowering
 * can still see it, so the expander is part of the lowering and every
 * expansion is lowered on the spot.  D5's hygiene verdict stands: (a)
 * renaming -- every identifier a template introduces that lands in a
 * BINDING position is renamed to a fresh symbol, consistently across that
 * expansion, so it cannot capture a use-site name; a free identifier the
 * template introduces keeps its name and means what it means at the
 * definition site, which in one flat namespace is right until the use site
 * shadows it (the referential-transparency gap the plan asks for a named
 * failing test of: tests/fixtures/r7rs-syntax-rules-referential-transparency).
 *
 * Patterns: `_`, literals (matched by name), pattern variables, `...` at any
 * depth including after a subpattern with its own ellipsis, elements after
 * an ellipsis (`(_ a ... b c)`), improper tails (`(_ a . rest)`), vectors,
 * and datum literals (numbers, strings, booleans, chars).  A custom ellipsis
 * (`(syntax-rules ::: (lits) ...)`) and the `(... ...)` escape are honoured.
 * Templates: substitution, `x ...` and `x ... ...`, dotted tails that splice
 * a substituted list, vectors, and `syntax-error`.  Macros scope lexically
 * through `define-syntax` (top level or body start), `let-syntax` and
 * `letrec-syntax` (both letrec-scoped here, since expansion is lazy).
 */

typedef struct SRule { Form *pattern, *template; } SRule;
typedef struct SMacro {
    const Symbol  *name;
    const Symbol  *ellipsis;
    const Symbol **literals; uint32_t n_literals;
    SRule         *rules;    uint32_t n_rules;
    LFrame        *def_scope;   /* R10: where its template's names mean what they mean */
} SMacro;

/* A pattern-variable binding: depth 0 holds the matched form; depth d > 0
 * holds an F_LIST whose items are depth d-1 values, one per iteration of
 * the ellipsis that produced it. */
typedef struct MBind { const Symbol *var; uint32_t depth; Form *val; } MBind;
typedef struct MEnv  { MBind *items; uint32_t n, cap; } MEnv;
/* A symbol buffer (pattern variables with their static depths, binders). */
typedef struct SB { const Symbol **syms; uint32_t *depths; uint32_t n, cap; } SB;

#define SR_MAX_DEPTH 1000

static void sb_push(SB *b, const Symbol *s, uint32_t d) {
    for (uint32_t i = 0; i < b->n; i++) if (b->syms[i] == s) return;
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 8;
        b->syms = (const Symbol **)realloc((void *)b->syms, b->cap * sizeof(*b->syms));
        b->depths = (uint32_t *)realloc(b->depths, b->cap * sizeof(uint32_t));
        if (!b->syms || !b->depths) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    b->syms[b->n] = s; b->depths[b->n] = d; b->n++;
}
static void sb_free(SB *b) { free((void *)b->syms); free(b->depths); b->syms = NULL; b->depths = NULL; b->n = b->cap = 0; }

static void env_push(MEnv *e, const Symbol *var, uint32_t depth, Form *val) {
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->items = (MBind *)realloc(e->items, e->cap * sizeof(MBind));
        if (!e->items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    e->items[e->n].var = var; e->items[e->n].depth = depth; e->items[e->n].val = val; e->n++;
}
static MBind *env_lookup(MEnv *e, const Symbol *var) {
    for (int32_t i = (int32_t)e->n - 1; i >= 0; i--) if (e->items[i].var == var) return &e->items[i];
    return NULL;
}
static void env_free(MEnv *e) { free(e->items); e->items = NULL; e->n = e->cap = 0; }

static SMacro *sr_lookup(SL *sl, const Symbol *s) {
    for (int32_t i = (int32_t)sl->n_macros - 1; i >= 0; i--)
        if (sl->macros[i]->name == s) return sl->macros[i];
    return NULL;
}
static void sr_push(SL *sl, SMacro *m) {
    if (sl->n_macros == sl->cap_macros) {
        sl->cap_macros = sl->cap_macros ? sl->cap_macros * 2 : 8;
        sl->macros = (SMacro **)realloc(sl->macros, sl->cap_macros * sizeof(SMacro *));
        if (!sl->macros) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->macros[sl->n_macros++] = m;
}
static bool sr_is_literal(const SMacro *m, const Symbol *s) {
    for (uint32_t i = 0; i < m->n_literals; i++) if (m->literals[i] == s) return true;
    return false;
}
/* R10: a literal has priority over the ellipsis (R7RS 4.3.2): with
 * `(syntax-rules ... (...) ...)` the `...` matches and emits itself. */
static bool sr_is_ellipsis(const SMacro *m, const Form *f) {
    return f && f->tag == F_SYM && f->as.sym == m->ellipsis && !sr_is_literal(m, f->as.sym);
}
/* The items of a list form and its dotted tail (NULL when proper). */
static void sr_parts(SL *sl, Form *f, Form ***items, uint32_t *n, Form **tail) {
    *tail = NULL;
    if (f->tag == F_NIL) { *items = NULL; *n = 0; return; }
    *items = f->as.list.items; *n = f->as.list.len;
    if (*n >= 3 && is_sym((*items)[*n - 2], sl->s_dot)) { *tail = (*items)[*n - 1]; *n -= 2; }
}
static bool sr_is_listy(const Form *f) { return f->tag == F_LIST || f->tag == F_NIL; }

/* Pattern variables of `pat` with their static ellipsis depths. */
static void sr_pattern_vars(SL *sl, const SMacro *m, Form *pat, uint32_t depth, SB *out) {
    switch (pat->tag) {
        case F_SYM:
            if (pat->as.sym == sl->s_underscore || sr_is_ellipsis(m, pat) || sr_is_literal(m, pat->as.sym)) return;
            sb_push(out, pat->as.sym, depth);
            return;
        case F_LIST: case F_VEC: {
            if (is_char_form(sl, pat)) return;
            for (uint32_t i = 0; i < pat->as.list.len; i++) {
                Form *it = pat->as.list.items[i];
                if (is_sym(it, sl->s_dot)) continue;
                uint32_t d = depth;
                if (i + 1 < pat->as.list.len && sr_is_ellipsis(m, pat->as.list.items[i + 1])) d++;
                if (sr_is_ellipsis(m, it)) continue;
                sr_pattern_vars(sl, m, it, d, out);
            }
            return;
        }
        default: return;
    }
}

static bool sr_match(SL *sl, const SMacro *m, Form *pat, Form *form, MEnv *env);

/* Match pattern items (with an optional dotted tail pattern) against form
 * items (with an optional dotted tail).  At most one ellipsis per level. */
static bool sr_match_items(SL *sl, const SMacro *m, Form **pi, uint32_t np, Form *ptail,
                           Form **fi, uint32_t nf, Form *ftail, MEnv *env, Span sp) {
    int32_t e = -1;
    for (uint32_t i = 0; i < np; i++) {
        if (sr_is_ellipsis(m, pi[i])) {
            if (i == 0) { err(pi[i], "an ellipsis must follow a subpattern"); return false; }
            if (e >= 0) { err(pi[i], "only one ellipsis per list level in a pattern"); return false; }
            e = (int32_t)i;
        }
    }
    if (e < 0) {
        if (ptail) {
            if (nf < np) return false;
            for (uint32_t i = 0; i < np; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
            Form *rest;
            if (nf == np) rest = ftail ? ftail : Nil(sl, sp);
            else {
                FB b = {0};
                for (uint32_t i = np; i < nf; i++) fb_push(&b, fi[i]);
                if (ftail) { fb_push(&b, Sym(sl, sp, sl->s_dot)); fb_push(&b, ftail); }
                rest = fb_list(sl, &b, sp);
            }
            return sr_match(sl, m, ptail, rest, env);
        }
        if (nf != np || ftail) return false;
        for (uint32_t i = 0; i < np; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
        return true;
    }
    uint32_t npre = (uint32_t)e - 1, npost = np - (uint32_t)e - 1;
    Form *sub = pi[e - 1];
    if (nf < npre + npost) return false;
    if (!ptail && ftail) return false;
    uint32_t cnt = nf - npre - npost;
    for (uint32_t i = 0; i < npre; i++) if (!sr_match(sl, m, pi[i], fi[i], env)) return false;
    MEnv *subs = cnt ? (MEnv *)calloc(cnt, sizeof(MEnv)) : NULL;
    bool ok = true;
    for (uint32_t j = 0; j < cnt && ok; j++)
        if (!sr_match(sl, m, sub, fi[npre + j], &subs[j])) ok = false;
    if (ok) {
        SB vars = {0};
        sr_pattern_vars(sl, m, sub, 0, &vars);
        for (uint32_t v = 0; v < vars.n; v++) {
            FB seq = {0};
            for (uint32_t j = 0; j < cnt; j++) {
                MBind *b = env_lookup(&subs[j], vars.syms[v]);
                fb_push(&seq, b ? b->val : Nil(sl, sp));
            }
            env_push(env, vars.syms[v], vars.depths[v] + 1, fb_list(sl, &seq, sp));
        }
        sb_free(&vars);
    }
    for (uint32_t j = 0; j < cnt; j++) env_free(&subs[j]);
    free(subs);
    if (!ok) return false;
    for (uint32_t i = 0; i < npost; i++)
        if (!sr_match(sl, m, pi[e + 1 + i], fi[nf - npost + i], env)) return false;
    if (ptail) return sr_match(sl, m, ptail, ftail ? ftail : Nil(sl, sp), env);
    return true;
}

static bool sr_match(SL *sl, const SMacro *m, Form *pat, Form *form, MEnv *env) {
    switch (pat->tag) {
        case F_SYM:
            /* A literal first: `_` in the literals list matches only `_`
             * (R7RS 4.3.2), it is not the wildcard. */
            /* An input matches a literal when they name the same thing
             * (free-identifier=?): a literal a template inserted was renamed
             * like a binder, and is compared by its original name. */
            if (sr_is_literal(m, pat->as.sym))
                return form->tag == F_SYM && lit_orig(sl, ident_orig(sl, form->as.sym)) == lit_orig(sl, pat->as.sym);
            if (pat->as.sym == sl->s_underscore) return true;
            if (sr_is_ellipsis(m, pat)) { err(pat, "misplaced ellipsis in pattern"); return false; }
            env_push(env, pat->as.sym, 0, form);
            return true;
        case F_NIL: case F_LIST: {
            if (is_char_form(sl, pat)) return form_equal(pat, form);
            if (!sr_is_listy(form)) return false;
            Form **pi, **fi, *ptail, *ftail; uint32_t np, nf;
            sr_parts(sl, pat, &pi, &np, &ptail);
            sr_parts(sl, form, &fi, &nf, &ftail);
            return sr_match_items(sl, m, pi, np, ptail, fi, nf, ftail, env, form->span);
        }
        case F_VEC:
            if (form->tag != F_VEC) return false;
            return sr_match_items(sl, m, pat->as.list.items, pat->as.list.len, NULL,
                                  form->as.list.items, form->as.list.len, NULL, env, form->span);
        default:
            return form_equal(pat, form);
    }
}

/* Template variables bound at depth >= 1 in `env`, i.e. the ones an
 * ellipsis over `t` iterates. */
static void sr_template_vars(SL *sl, const SMacro *m, Form *t, MEnv *env, SB *out) {
    switch (t->tag) {
        case F_SYM: {
            MBind *b = env_lookup(env, t->as.sym);
            if (b && b->depth >= 1) sb_push(out, t->as.sym, b->depth);
            return;
        }
        case F_LIST: case F_VEC:
            if (t->as.list.len == 2 && sr_is_ellipsis(m, t->as.list.items[0])) return;   /* (... ...) */
            for (uint32_t i = 0; i < t->as.list.len; i++) sr_template_vars(sl, m, t->as.list.items[i], env, out);
            return;
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            sr_template_vars(sl, m, t->as.list.items[0], env, out);
            return;
        default: return;
    }
}

static Form *sr_inst(SL *sl, const SMacro *m, Form *t, MEnv *env, Span sp, FB *intro, bool esc);

/* `t ...` (k ellipses): instantiate `t` once per element of the iterated
 * variables, flattening k levels. */
static bool sr_inst_ellipsis(SL *sl, const SMacro *m, Form *t, MEnv *env, uint32_t k,
                             Span sp, FB *intro, FB *out) {
    SB vars = {0};
    sr_template_vars(sl, m, t, env, &vars);
    if (vars.n == 0) { err(t, "no pattern variable in the subtemplate before this ellipsis"); return false; }
    int64_t len = -1;
    for (uint32_t v = 0; v < vars.n; v++) {
        MBind *b = env_lookup(env, vars.syms[v]);
        int64_t l = (int64_t)b->val->as.list.len;
        if (len < 0) len = l;
        else if (l != len) { err(t, "pattern variables under this ellipsis matched different lengths"); sb_free(&vars); return false; }
    }
    bool ok = true;
    for (int64_t j = 0; j < len && ok; j++) {
        MEnv e2 = {0};
        for (uint32_t i = 0; i < env->n; i++) env_push(&e2, env->items[i].var, env->items[i].depth, env->items[i].val);
        for (uint32_t v = 0; v < vars.n; v++) {
            MBind *b = env_lookup(env, vars.syms[v]);
            env_push(&e2, vars.syms[v], b->depth - 1, b->val->as.list.items[j]);
        }
        if (k == 1) {
            Form *r = sr_inst(sl, m, t, &e2, sp, intro, false);
            if (!r) ok = false; else fb_push(out, r);
        } else {
            ok = sr_inst_ellipsis(sl, m, t, &e2, k - 1, sp, intro, out);
        }
        env_free(&e2);
    }
    sb_free(&vars);
    return ok;
}

static Form *sr_inst(SL *sl, const SMacro *m, Form *t, MEnv *env, Span sp, FB *intro, bool esc) {
    switch (t->tag) {
        case F_SYM: {
            if (!esc && sr_is_ellipsis(m, t)) { err(t, "misplaced ellipsis in template"); return NULL; }
            MBind *b = env_lookup(env, t->as.sym);
            if (b) {
                if (b->depth != 0) { err(t, "pattern variable '%s' needs %u more ellipsis(es) here", t->as.sym->name, b->depth); return NULL; }
                return b->val;
            }
            Form *s = Sym(sl, sp, t->as.sym);
            fb_push(intro, s);
            return s;
        }
        case F_NIL: return t;
        case F_LIST: case F_VEC: {
            Form **items, *tail; uint32_t n;
            if (t->tag == F_VEC) { items = t->as.list.items; n = t->as.list.len; tail = NULL; }
            else sr_parts(sl, t, &items, &n, &tail);
            if (!esc && t->tag == F_LIST && n == 2 && !tail && sr_is_ellipsis(m, items[0]))
                return sr_inst(sl, m, items[1], env, sp, intro, true);
            FB out = {0};
            for (uint32_t i = 0; i < n; i++) {
                uint32_t k = 0;
                if (!esc) while (i + 1 + k < n && sr_is_ellipsis(m, items[i + 1 + k])) k++;
                if (k == 0) {
                    Form *r = sr_inst(sl, m, items[i], env, sp, intro, esc);
                    if (!r) { free(out.items); return NULL; }
                    fb_push(&out, r);
                } else {
                    if (!sr_inst_ellipsis(sl, m, items[i], env, k, sp, intro, &out)) { free(out.items); return NULL; }
                    i += k;
                }
            }
            if (tail) {
                Form *tv = sr_inst(sl, m, tail, env, sp, intro, esc);
                if (!tv) { free(out.items); return NULL; }
                if (sr_is_listy(tv)) {
                    /* (a . (b c)) is (a b c): splice a substituted list. */
                    if (tv->tag == F_LIST)
                        for (uint32_t i = 0; i < tv->as.list.len; i++) fb_push(&out, tv->as.list.items[i]);
                } else {
                    fb_push(&out, Sym(sl, sp, sl->s_dot));
                    fb_push(&out, tv);
                }
            }
            if (t->tag == F_VEC) {
                Form *vf = fb_vec(sl, &out, sp);
                vf->fx_prov = t->fx_prov;   /* R7: a template's `#(...)` stays a datum */
                return vf;
            }
            if (out.n == 0) { free(out.items); return Nil(sl, sp); }
            return fb_list(sl, &out, sp);
        }
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING: {
            Form *inner = sr_inst(sl, m, t->as.list.items[0], env, sp, intro, esc);
            if (!inner) return NULL;
            Form *g = form_new(sl->a, t->tag, sp);
            Form **one = (Form **)arena_alloc(sl->a, sizeof(Form *));
            one[0] = inner;
            g->as.list.items = one; g->as.list.len = 1;
            return g;
        }
        default: return t;
    }
}

/* --- hygiene (D5a): rename the introduced binders ---------------------------- */

static bool hyg_introduced(const FB *intro, const Form *f) {
    for (uint32_t i = 0; i < intro->n; i++) if (intro->items[i] == f) return true;
    return false;
}
static void hyg_add(SL *sl, Form *f, const FB *intro, SB *out) {
    if (f && f->tag == F_SYM && f->as.sym != sl->s_dot && hyg_introduced(intro, f)) sb_push(out, f->as.sym, 0);
}
/* A formals spec: a symbol, or a (possibly dotted) list of symbols. */
static void hyg_formals(SL *sl, Form *formals, const FB *intro, SB *out) {
    if (!formals) return;
    if (formals->tag == F_SYM) { hyg_add(sl, formals, intro, out); return; }
    if (formals->tag == F_LIST)
        for (uint32_t i = 0; i < formals->as.list.len; i++) hyg_add(sl, formals->as.list.items[i], intro, out);
}
static void hyg_walk(SL *sl, Form *f, const FB *intro, SB *out, bool quoted);
static void hyg_binding_list(SL *sl, Form *bl, const FB *intro, SB *out, bool formals) {
    if (!bl || bl->tag != F_LIST) return;
    for (uint32_t i = 0; i < bl->as.list.len; i++) {
        Form *b = bl->as.list.items[i];
        if (b->tag != F_LIST || b->as.list.len < 1) continue;
        if (formals) hyg_formals(sl, b->as.list.items[0], intro, out);
        else hyg_add(sl, b->as.list.items[0], intro, out);
    }
}
static void hyg_walk(SL *sl, Form *f, const FB *intro, SB *out, bool quoted) {
    if (!f) return;
    switch (f->tag) {
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_walk(sl, f->as.list.items[0], intro, out, true); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING: hyg_walk(sl, f->as.list.items[0], intro, out, false); return;
        case F_VEC:
            for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
            return;
        case F_LIST: break;
        default: return;
    }
    if (quoted || f->as.list.len == 0) {
        for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
        return;
    }
    Form *h = f->as.list.items[0];
    if (h->tag == F_SYM) {
        const Symbol *s = h->as.sym;
        uint32_t n = f->as.list.len;
        if (s == sl->s_quote || s == sl->s_syntax_rules || s == sl->s_define_syntax ||
            s == sl->s_let_syntax || s == sl->s_letrec_syntax) return;
        if (s == sl->s_lambda && n >= 2) hyg_formals(sl, f->as.list.items[1], intro, out);
        else if (s == sl->s_define && n >= 2) {
            Form *t = f->as.list.items[1];
            if (t->tag == F_SYM) hyg_add(sl, t, intro, out); else hyg_formals(sl, t, intro, out);
        }
        else if (s == sl->s_define_values && n >= 2) hyg_formals(sl, f->as.list.items[1], intro, out);
        else if ((s == sl->s_let || s == sl->s_letstar || s == sl->s_letrec || s == sl->s_letrecstar) && n >= 2) {
            uint32_t bi = 1;
            if (f->as.list.items[1]->tag == F_SYM) { hyg_add(sl, f->as.list.items[1], intro, out); bi = 2; }
            if (bi < n) hyg_binding_list(sl, f->as.list.items[bi], intro, out, false);
        }
        else if (s == sl->s_do && n >= 2) hyg_binding_list(sl, f->as.list.items[1], intro, out, false);
        else if ((s == sl->s_let_values || s == sl->s_letstar_values) && n >= 2)
            hyg_binding_list(sl, f->as.list.items[1], intro, out, true);
        else if (s == sl->s_case_lambda) {
            for (uint32_t i = 1; i < n; i++) {
                Form *cl = f->as.list.items[i];
                if (cl->tag == F_LIST && cl->as.list.len >= 1) hyg_formals(sl, cl->as.list.items[0], intro, out);
            }
        }
        else if (s == sl->s_guard && n >= 2 && f->as.list.items[1]->tag == F_LIST &&
                 f->as.list.items[1]->as.list.len >= 1)
            hyg_add(sl, f->as.list.items[1]->as.list.items[0], intro, out);   /* R10 */
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_walk(sl, f->as.list.items[i], intro, out, quoted);
}
/* Rename every introduced occurrence of a collected binder, in place:
 * introduced symbol nodes are fresh to this expansion, so no other form
 * shares them. */
static void hyg_rename(SL *sl, Form *f, const FB *intro, const SB *binders, const Symbol **aliases, bool quoted) {
    if (!f) return;
    switch (f->tag) {
        case F_SYM:
            if (quoted || !hyg_introduced(intro, f)) return;
            for (uint32_t i = 0; i < binders->n; i++)
                if (binders->syms[i] == f->as.sym) { f->as.sym = aliases[i]; return; }
            return;
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_rename(sl, f->as.list.items[0], intro, binders, aliases, true); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING: hyg_rename(sl, f->as.list.items[0], intro, binders, aliases, false); return;
        case F_LIST: case F_VEC:
            if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
            for (uint32_t i = 0; i < f->as.list.len; i++) hyg_rename(sl, f->as.list.items[i], intro, binders, aliases, quoted);
            return;
        default: return;
    }
}

/* R10: a `syntax-rules` a template inserts (a macro that defines a macro):
 * its literals and each rule's pattern variables, where the TEMPLATE put
 * them, are binders of that inner transformer -- renamed apart from the
 * user's identifiers that the outer substitution put beside them, so
 * `((bar x) 'y)` with the user's `x` in for `y` quotes the user's `x`
 * rather than substituting the inner pattern variable.  Renamed everywhere
 * in the rule, quotes included: a pattern variable is substituted inside a
 * quote.  `made` collects the fresh names. */
static void hyg_sr_collect(SL *sl, const SMacro *om, Form *p, const FB *intro, const Symbol *ell,
                           const SB *lits, SB *out, bool head) {
    if (!p) return;
    if (p->tag == F_SYM) {
        if (head || !hyg_introduced(intro, p)) return;
        const Symbol *x = p->as.sym;
        if (x == ell || x == sl->s_ellipsis || x == sl->s_underscore || x == sl->s_dot) return;
        for (uint32_t i = 0; i < lits->n; i++) if (lits->syms[i] == x) return;
        sb_push(out, x, 0);
        return;
    }
    (void)om;
    if (p->tag == F_LIST || p->tag == F_VEC)
        for (uint32_t i = 0; i < p->as.list.len; i++)
            hyg_sr_collect(sl, om, p->as.list.items[i], intro, ell, lits, out, false);
}
static void hyg_sr_rename(Form *f, const FB *intro, const SB *from, const Symbol **to) {
    if (!f) return;
    if (f->tag == F_SYM) {
        if (!hyg_introduced(intro, f)) return;
        for (uint32_t i = 0; i < from->n; i++) if (from->syms[i] == f->as.sym) { f->as.sym = to[i]; return; }
        return;
    }
    if (f->tag == F_LIST || f->tag == F_VEC || f->tag == F_QUOTE || f->tag == F_QUASIQUOTE ||
        f->tag == F_UNQUOTE || f->tag == F_UNQUOTE_SPLICING)
        for (uint32_t i = 0; i < f->as.list.len; i++) hyg_sr_rename(f->as.list.items[i], intro, from, to);
}
static void hyg_inner_sr(SL *sl, const SMacro *om, Form *f, const FB *intro, SB *made) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return;
    if (f->tag == F_LIST && f->as.list.len >= 2 && f->as.list.items[0]->tag == F_SYM &&
        f->as.list.items[0]->as.sym == sl->s_syntax_rules) {
        uint32_t i = 1;
        const Symbol *ell = sl->s_ellipsis;
        if (f->as.list.items[i]->tag == F_SYM) { ell = f->as.list.items[i]->as.sym; i++; }
        if (i >= f->as.list.len) return;
        Form *lits = f->as.list.items[i++];
        SB lit = {0};
        if (lits->tag == F_LIST)
            for (uint32_t j = 0; j < lits->as.list.len; j++) {
                Form *l = lits->as.list.items[j];
                if (l->tag == F_SYM && hyg_introduced(intro, l)) sb_push(&lit, l->as.sym, 0);
            }
        /* The literals: renamed across the whole transformer, and remembered
         * by their original names for matching. */
        if (lit.n) {
            const Symbol **to = (const Symbol **)arena_alloc(sl->a, lit.n * sizeof(Symbol *));
            for (uint32_t j = 0; j < lit.n; j++) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__l", lit.syms[j]->name);
                to[j] = fresh(sl, pre);
                sym_pair_push(sl, &sl->lit_from, &sl->lit_to, &sl->n_lit, &sl->cap_lit, to[j], lit.syms[j]);
                sb_push(made, to[j], 0);
            }
            hyg_sr_rename(f, intro, &lit, to);
        }
        SB lit_now = {0};
        if (lits->tag == F_LIST)
            for (uint32_t j = 0; j < lits->as.list.len; j++)
                if (lits->as.list.items[j]->tag == F_SYM) sb_push(&lit_now, lits->as.list.items[j]->as.sym, 0);
        for (; i < f->as.list.len; i++) {
            Form *rule = f->as.list.items[i];
            if (rule->tag != F_LIST || rule->as.list.len != 2 || rule->as.list.items[0]->tag != F_LIST) continue;
            Form *pat = rule->as.list.items[0];
            SB vars = {0};
            for (uint32_t j = 0; j < pat->as.list.len; j++)
                hyg_sr_collect(sl, om, pat->as.list.items[j], intro, ell, &lit_now, &vars, j == 0);
            if (vars.n) {
                const Symbol **to = (const Symbol **)arena_alloc(sl->a, vars.n * sizeof(Symbol *));
                for (uint32_t j = 0; j < vars.n; j++) {
                    char pre[160];
                    snprintf(pre, sizeof pre, "%s__p", vars.syms[j]->name);
                    to[j] = fresh(sl, pre);
                    sb_push(made, to[j], 0);
                }
                hyg_sr_rename(rule, intro, &vars, to);
            }
            sb_free(&vars);
        }
        sb_free(&lit);
        sb_free(&lit_now);
        return;
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_inner_sr(sl, om, f->as.list.items[i], intro, made);
}

/* R10 (referential transparency): a free identifier the template inserted
 * means what it means where the MACRO was defined.  Bound there, it becomes
 * that binding's unique name; unbound there (a global, a keyword) but bound
 * locally at the use site, it becomes an alias that names the global --
 * `(let ((if even?)) (my-or ...))` still expands to the core `if`.  Anything
 * else is already right.  Quoted data is left alone. */
static bool sb_has(const SB *b, const Symbol *s) {
    for (uint32_t i = 0; i < b->n; i++) if (b->syms[i] == s) return true;
    return false;
}
/* One alias per identifier per expansion: a template that writes `x` twice
 * (cute's `((x nse) ...)` and `(position ... x)`) means one identifier, and
 * a later step may bind it (hyg_pending), so both must share the alias. */
typedef struct { const Symbol **from, **to; uint32_t n, cap; } HygAliases;
static void hyg_resolve_free(SL *sl, const SMacro *m, Form *f, const FB *intro, const SB *made, bool quoted,
                             HygAliases *ga) {
    if (!f) return;
    switch (f->tag) {
        case F_SYM: {
            if (quoted || !hyg_introduced(intro, f)) return;
            const Symbol *x = f->as.sym;
            if (sb_has(made, x) || x == m->ellipsis || x == sl->s_ellipsis ||
                x == sl->s_underscore || x == sl->s_dot)
                return;
            const Symbol *d = scope_lookup(m->def_scope, x);
            if (d) { f->as.sym = d; return; }
            if (sl->scope && scope_lookup(sl->scope, x)) {
                for (uint32_t i = 0; i < ga->n; i++)
                    if (ga->from[i] == x) { f->as.sym = ga->to[i]; return; }
                char pre[160];
                snprintf(pre, sizeof pre, "%s__g", x->name);
                const Symbol *a = fresh(sl, pre);
                sym_pair_push(sl, &sl->ga_from, &sl->ga_to, &sl->n_ga, &sl->cap_ga, a, x);
                sym_pair_push(sl, &ga->from, &ga->to, &ga->n, &ga->cap, x, a);
                f->as.sym = a;
            }
            return;
        }
        case F_QUOTE: return;
        case F_QUASIQUOTE: hyg_resolve_free(sl, m, f->as.list.items[0], intro, made, true, ga); return;
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            hyg_resolve_free(sl, m, f->as.list.items[0], intro, made, false, ga);
            return;
        case F_LIST: case F_VEC:
            if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
            for (uint32_t i = 0; i < f->as.list.len; i++)
                hyg_resolve_free(sl, m, f->as.list.items[i], intro, made, quoted, ga);
            return;
        default: return;
    }
}

/* R7RS 4.3.2 (hygiene), the binder a later step makes: an identifier the
 * template inserts as an ARGUMENT of another macro use may become a binder
 * only once that macro expands -- SRFI 26's reference `cut` inserts `x` at
 * each recursive step, and the last step makes them all lambda parameters.
 * Each step's `x` is a different identifier, so here each gets its own
 * alias, recorded as a global alias of the name: left free it still means
 * what the name means, and bound by a later step it is that binding (rn
 * looks an alias up locally first).  Only identifiers nothing else claims:
 * not bound where the macro was defined or used (hyg_resolve_free's), not a
 * keyword or macro, not one this expansion already renamed. */
static bool is_scheme_syntax_name(const char *name);
static bool hyg_pending_skip(SL *sl, const SMacro *m, const Symbol *x, const SB *made) {
    if (sb_has(made, x) || global_alias_orig(sl, x)) return true;
    if (x == m->ellipsis || x == sl->s_ellipsis || x == sl->s_underscore || x == sl->s_dot ||
        x == sl->s_else || x == sl->s_arrow)
        return true;
    if (is_scheme_syntax_name(x->name) || sr_lookup(sl, x)) return true;
    if (scope_lookup(m->def_scope, x) || (sl->scope && scope_lookup(sl->scope, x))) return true;
    return false;
}
static void hyg_pending_args(SL *sl, const SMacro *m, Form *f, const FB *intro, const SB *made, SB *out) {
    if (!f) return;
    if (f->tag == F_SYM) {
        if (hyg_introduced(intro, f) && !hyg_pending_skip(sl, m, f->as.sym, made) && !sb_has(out, f->as.sym))
            sb_push(out, f->as.sym, 0);
        return;
    }
    if (f->tag == F_QUOTE) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_pending_args(sl, m, f->as.list.items[i], intro, made, out);
}
static void hyg_pending(SL *sl, const SMacro *m, Form *f, const FB *intro, const SB *made, SB *out) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return;
    if (f->tag == F_LIST && f->as.list.len >= 1 && is_sym(f->as.list.items[0], sl->s_quote)) return;
    if (f->tag == F_LIST && f->as.list.len >= 1 && f->as.list.items[0]->tag == F_SYM) {
        const Symbol *h = f->as.list.items[0]->as.sym;
        if (h == sl->s_syntax_rules) return;   /* an inner transformer: hyg_inner_sr's */
        if (sr_lookup(sl, ident_orig(sl, h)) && !(sl->scope && scope_lookup(sl->scope, h))) {
            for (uint32_t i = 1; i < f->as.list.len; i++)
                hyg_pending_args(sl, m, f->as.list.items[i], intro, made, out);
            return;
        }
    }
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_pending(sl, m, f->as.list.items[i], intro, made, out);
}
static void hyg_alias_rename(Form *f, const FB *intro, const SB *from, const Symbol **to) {
    if (!f) return;
    if (f->tag == F_SYM) {
        if (!hyg_introduced(intro, f)) return;
        for (uint32_t i = 0; i < from->n; i++) if (from->syms[i] == f->as.sym) { f->as.sym = to[i]; return; }
        return;
    }
    if (f->tag == F_QUOTE) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    for (uint32_t i = 0; i < f->as.list.len; i++) hyg_alias_rename(f->as.list.items[i], intro, from, to);
}

/* One expansion of `use` by `m`, hygienically renamed; NULL after an error. */
static Form *sr_expand(SL *sl, SMacro *m, Form *use) {
    Form **fi, *ftail; uint32_t nf;
    sr_parts(sl, use, &fi, &nf, &ftail);
    for (uint32_t r = 0; r < m->n_rules; r++) {
        Form *pat = m->rules[r].pattern;
        Form **pi, *ptail; uint32_t np;
        sr_parts(sl, pat, &pi, &np, &ptail);
        MEnv env = {0};
        /* The keyword position of the pattern is not matched (R7RS 4.3.2). */
        bool ok = np >= 1 && nf >= 1 &&
                  sr_match_items(sl, m, pi + 1, np - 1, ptail, fi + 1, nf - 1, ftail, &env, use->span);
        if (!ok) { env_free(&env); continue; }
        FB intro = {0};
        Form *x = sr_inst(sl, m, m->rules[r].template, &env, use->span, &intro, false);
        env_free(&env);
        if (!x) { free(intro.items); return NULL; }
        SB made = {0};
        hyg_inner_sr(sl, m, x, &intro, &made);
        SB binders = {0};
        hyg_walk(sl, x, &intro, &binders, false);
        if (binders.n > 0) {
            const Symbol **aliases = (const Symbol **)arena_alloc(sl->a, binders.n * sizeof(const Symbol *));
            for (uint32_t i = 0; i < binders.n; i++) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__h", binders.syms[i]->name);
                aliases[i] = fresh(sl, pre);
                sb_push(&made, aliases[i], 0);
            }
            hyg_rename(sl, x, &intro, &binders, aliases, false);
        }
        sb_free(&binders);
        SB pend = {0};
        hyg_pending(sl, m, x, &intro, &made, &pend);
        if (pend.n > 0) {
            const Symbol **to = (const Symbol **)arena_alloc(sl->a, pend.n * sizeof(const Symbol *));
            for (uint32_t i = 0; i < pend.n; i++) {
                char pre[160];
                snprintf(pre, sizeof pre, "%s__t", pend.syms[i]->name);
                to[i] = fresh(sl, pre);
                sym_pair_push(sl, &sl->ga_from, &sl->ga_to, &sl->n_ga, &sl->cap_ga, to[i], pend.syms[i]);
                sb_push(&made, to[i], 0);
            }
            hyg_alias_rename(x, &intro, &pend, to);
        }
        sb_free(&pend);
        HygAliases ga = {0};
        hyg_resolve_free(sl, m, x, &intro, &made, false, &ga);
        sb_free(&made);
        free(intro.items);
        return x;
    }
    err(use, "no syntax-rules pattern of '%s' matches this form", m->name->name);
    return NULL;
}

/* (define-syntax name (syntax-rules [ellipsis] (literal ...) (pattern template) ...)) */
static void sr_define(SL *sl, Form *f) {
    if (f->as.list.len != 3 || f->as.list.items[1]->tag != F_SYM) {
        err(f, "define-syntax expects (define-syntax name (syntax-rules ...))");
        return;
    }
    const Symbol *name = f->as.list.items[1]->as.sym;
    Form *spec = f->as.list.items[2];
    if (head_is(spec, sl->s_er_macro_transformer)) {
        err(spec, "er-macro-transformer is not supported yet (r7rs-lang-plan R4 defers the low-level "
                  "escape); write the transformer with syntax-rules");
        return;
    }
    if (!head_is(spec, sl->s_syntax_rules)) {
        err(spec, "the transformer of '%s' must be a (syntax-rules ...) form", name->name);
        return;
    }
    SMacro *m = (SMacro *)arena_alloc(sl->a, sizeof(SMacro));
    memset(m, 0, sizeof *m);
    m->name = name;
    m->ellipsis = sl->s_ellipsis;
    m->def_scope = sl->scope;
    uint32_t i = 1;
    if (i < spec->as.list.len && spec->as.list.items[i]->tag == F_SYM) { m->ellipsis = spec->as.list.items[i]->as.sym; i++; }
    if (i >= spec->as.list.len || !sr_is_listy(spec->as.list.items[i])) {
        err(spec, "syntax-rules expects a literals list: (syntax-rules (literal ...) (pattern template) ...)");
        return;
    }
    Form *lits = spec->as.list.items[i++];
    if (lits->tag == F_LIST) {
        m->literals = (const Symbol **)arena_alloc(sl->a, lits->as.list.len * sizeof(const Symbol *));
        for (uint32_t j = 0; j < lits->as.list.len; j++) {
            if (lits->as.list.items[j]->tag != F_SYM) { err(lits->as.list.items[j], "syntax-rules literals must be identifiers"); return; }
            m->literals[m->n_literals++] = lits->as.list.items[j]->as.sym;
        }
    }
    uint32_t nrules = spec->as.list.len - i;
    m->rules = (SRule *)arena_alloc(sl->a, (nrules ? nrules : 1) * sizeof(SRule));
    for (; i < spec->as.list.len; i++) {
        Form *rule = spec->as.list.items[i];
        if (rule->tag != F_LIST || rule->as.list.len != 2 || !sr_is_listy(rule->as.list.items[0]) ||
            rule->as.list.items[0]->tag == F_NIL) {
            err(rule, "a syntax-rules rule is ((_ pattern ...) template)");
            return;
        }
        m->rules[m->n_rules].pattern = rule->as.list.items[0];
        m->rules[m->n_rules].template = rule->as.list.items[1];
        m->n_rules++;
    }
    sr_push(sl, m);
}

/* Expand the head of `f` while it is a macro use.  Returns NULL after an
 * error; otherwise the first form whose head is not a macro. */
static Form *sr_expand_head(SL *sl, Form *f) {
    uint32_t steps = 0;
    for (;;) {
        if (!f || f->tag != F_LIST || f->as.list.len == 0 || f->as.list.items[0]->tag != F_SYM) return f;
        /* R10: a local variable of that name shadows the macro; a template's
         * alias of the macro's name is the macro. */
        const Symbol *hs = f->as.list.items[0]->as.sym;
        const Symbol *g = global_alias_orig(sl, hs);
        if (sl->scope && scope_lookup(sl->scope, hs)) return f;
        SMacro *m = sr_lookup(sl, g ? g : hs);
        if (!m) return f;
        if (++steps > SR_MAX_DEPTH) {
            err(f, "macro expansion of '%s' did not terminate after %d steps", m->name->name, SR_MAX_DEPTH);
            return NULL;
        }
        f = sr_expand(sl, m, f);
        if (!f) return NULL;
    }
}

/* (let-syntax ((name (syntax-rules ...)) ...) body...) -- the body is a
 * body (internal defines allowed) with the macros in scope. */
static Form *lower_let_syntax(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3 || !sr_is_listy(f->as.list.items[1])) {
        err(f, "%s expects (%s ((name (syntax-rules ...)) ...) body...)",
            f->as.list.items[0]->as.sym->name, f->as.list.items[0]->as.sym->name);
        return Nil(sl, sp);
    }
    uint32_t mark = sl->n_macros;
    Form *bl = f->as.list.items[1];
    if (bl->tag == F_LIST) {
        for (uint32_t i = 0; i < bl->as.list.len; i++) {
            Form *b = bl->as.list.items[i];
            if (b->tag != F_LIST || b->as.list.len != 2) { err(b, "a let-syntax binding is (name (syntax-rules ...))"); continue; }
            Form *def = Ln(sl, b->span, 3, Sym(sl, b->span, sl->s_define_syntax), b->as.list.items[0], b->as.list.items[1]);
            sr_define(sl, def);
        }
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    sl->n_macros = mark;
    return body;
}

/* --- R5: the numeric operators ------------------------------------------- */
/*
 * R7RS arithmetic differs from Turmeric's operators in four ways the lowering
 * has to bridge: `(+)`, `(*)`, `(- x)` and `(/ x)` are legal; `=`/`<`/... take
 * any number of arguments and chain; a mixed exact/inexact pair promotes even
 * when both are literals (Turmeric's static `(+ 1 7.1)` is TUR-E0042); and
 * exact overflow must signal (D8).  So a call `(op a b c)` folds onto the
 * prelude's binary helper (`(r7rs-add2__ (r7rs-add2__ a b) c)`), which takes
 * the checked path for two exact integers and the promoting dynamic operator
 * otherwise, and a comparison chain binds its arguments once and tests each
 * adjacent pair.  A bare operator in value position names the variadic
 * prelude procedure, so `(apply + xs)` works.
 *
 * The prelude is exempt: it is where the helpers are written in terms of the
 * raw operators, so its `(+ a b)` must stay Turmeric's.
 */
static int op_index(SL *sl, const Symbol *s) {
    for (int i = 0; i < 9; i++) if (sl->ops[i] == s) return i;
    return -1;
}
static bool prelude_span(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    if (!f || !f->path) return false;
    /* R7: a synthetic source (`<eval>`, the interpreter's stub preamble) is
     * Turmeric whatever the session's language, never Scheme to rewrite --
     * R9: up to the end of the pinned preload.  The REPL compiles that
     * preload and the prompt's input as one `<eval>` text, and the input
     * after it is the user's Scheme (g_synthetic_user_from_line). */
    if (f->path[0] == '<')
        return !(g_synthetic_user_from_line && sp.line >= g_synthetic_user_from_line);
    size_t n = strlen(f->path);
    static const char SUFFIX[] = "r7rs/prelude.tur";
    size_t m = sizeof SUFFIX - 1;
    if (n >= m && memcmp(f->path + n - m, SUFFIX, m) == 0) return true;
    /* R7: the on-demand library files are written in the prelude's own
     * Turmeric shapes and against the typed stdlib's real names. */
    return strstr(f->path, "stdlib/r7rs/") != NULL;
}

bool scheme_span_is_user_source(Span sp) {
    return lang_span_is_scheme(sp) && !prelude_span(sp);
}
static Form *lower_operator(SL *sl, Form *f, int op) {
    Span sp = f->span;
    const Symbol *h = sl->ops[op], *bin = sl->ops_bin[op];
    uint32_t n = f->as.list.len - 1;
    Form **args = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) args[i] = lower(sl, f->as.list.items[1 + i]);
    if (op < 4) {
        if (n == 0) {
            if (op == 0) return Int(sl, sp, 0);
            if (op == 2) return Int(sl, sp, 1);
            err(f, "%s needs at least one argument", h->name);
            return Nil(sl, sp);
        }
        if (n == 1) {
            if (op == 1) return Ln(sl, sp, 3, Sym(sl, sp, bin), Int(sl, sp, 0), args[0]);
            if (op == 3) return Ln(sl, sp, 3, Sym(sl, sp, bin), Int(sl, sp, 1), args[0]);
            return args[0];
        }
        Form *acc = args[0];
        for (uint32_t i = 1; i < n; i++) acc = Ln(sl, sp, 3, Sym(sl, sp, bin), acc, args[i]);
        return acc;
    }
    if (n == 0) { err(f, "%s needs at least one argument", h->name); return Nil(sl, sp); }
    if (n == 1) return Bool(sl, sp, true);
    if (n == 2) return Ln(sl, sp, 3, Sym(sl, sp, bin), args[0], args[1]);
    /* (let [t0 a t1 b t2 c] (if (op t0 t1) (op t1 t2) #f)) */
    const Symbol **t = (const Symbol **)arena_alloc(sl->a, n * sizeof(*t));
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        t[i] = fresh(sl, "__r7rs_cmp");
        fb_push(&b, Sym(sl, sp, t[i]));
        fb_push(&b, args[i]);
    }
    Form *acc = Ln(sl, sp, 3, Sym(sl, sp, bin), Sym(sl, sp, t[n - 2]), Sym(sl, sp, t[n - 1]));
    for (int32_t i = (int32_t)n - 3; i >= 0; i--)
        acc = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if),
                 Ln(sl, sp, 3, Sym(sl, sp, bin), Sym(sl, sp, t[i]), Sym(sl, sp, t[i + 1])),
                 acc, Bool(sl, sp, false));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), acc);
}

/* --- the lowering ---------------------------------------------------------- */

static Form *lower(SL *sl, Form *f);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp);
static void lower_toplevel(SL *sl, Form *f, FB *out);
static bool is_include_head(const SL *sl, const Form *f, bool *fold);
static bool is_scheme_syntax_name(const char *name);
static bool include_files(SL *sl, Form *f, bool fold, FB *out);
static void lower_import_set(SL *sl, Form *set);
static const Symbol *library_module(SL *sl, Form *set, bool *ok);
static Form *cond_expand_clause(SL *sl, Form *f, uint32_t *out_n, Form ***out_items);
static void lower_record_type(SL *sl, Form *f, FB *out, bool local);
static bool is_export_rename(SL *sl, const Form *nm);
static void user_define_names(SL *sl, const Form *f, FB *out);
static void library_defined_names(SL *sl, Form *f, FB *out);
static bool lib_needs_respelling(SL *sl, const Symbol *s);
static const Symbol *lib_user_spelling(SL *sl, const Symbol *s);
/* r7rs-define-library-cannot-export-syntax: a library's macros, as the
 * library itself and every importer see them. */
typedef struct LibScan {
    FB syntax;     /* every define-syntax form of the library body, in order */
    FB exp_pub;    /* an exported macro's public name ... */
    FB exp_in;     /* ... and its name in the library */
    FB helpers;    /* the library's own definitions its macro forms name */
} LibScan;
static void lib_scan(SL *sl, Form *deflib, LibScan *out);
static void lib_scan_free(LibScan *ls);
static const Symbol *lib_hidden(SL *sl, const Symbol *mod, const Symbol *name);
static Form *lib_rewrite_exports(SL *sl, Form *deflib, const Symbol *mod, const LibScan *ls);
static void set_clash(SL *sl, const Symbol *from, const Symbol *to);
static const Symbol *clash_spelling(const SL *sl, const Symbol *s);
static const Symbol *std_before_redefinition(SL *sl, const Symbol *s, const Symbol *r);
static Form *rebind_rest(SL *sl, Span sp, const Symbol *rest, Form *body);

/* Is `name` (already renamed) the target of a `set!` anywhere in `f`?
 * Lexical, not file-wide: a `(set! n ...)` in one lambda must not turn every
 * other `n` in the program into an `any` cell.  Works on lowered and
 * unlowered forms alike -- `set!` keeps its head and its target is renamed
 * the same way -- and never looks under `quote`. */
static bool form_sets(SL *sl, const Symbol *name, const Form *f) {
    if (!f) return false;
    switch (f->tag) {
        case F_QUOTE: return false;
        case F_LIST:
            if (f->as.list.len == 3 && is_sym(f->as.list.items[0], sl->s_set) &&
                f->as.list.items[1]->tag == F_SYM &&
                rn(sl, f->as.list.items[1]->as.sym) == name)
                return true;
            /* fall through */
        case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_TYPE_ANN:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                if (form_sets(sl, name, f->as.list.items[i])) return true;
            return false;
        default: return false;
    }
}

/* Push one binding into a `let` vector: `[^mut] name [: any] init`.  A cell
 * that is ever `set!` is typed `any` so the store can hold any later value. */
static void push_binding(SL *sl, FB *b, Span sp, const Symbol *name, Form *init,
                         bool mut) {
    name = rn(sl, name);
    if (mut) {
        fb_push(b, Sym(sl, sp, sl->t_mut));
        fb_push(b, Sym(sl, sp, name));
        fb_push(b, AnyAnn(sl, sp));
    } else {
        fb_push(b, Sym(sl, sp, name));
    }
    fb_push(b, init);
}
/* Push one parameter.  Never `^mut`: a `fn` parameter vector does not take
 * the annotation (it reads as an extra parameter and the call then
 * partially applies), so a parameter that is `set!` is rebound as a mutable
 * local inside the body by rebind_muts instead. */
static void push_param(SL *sl, FB *b, Span sp, const Symbol *name) {
    /* R10: a parameter is a binder -- declared in the innermost scope. */
    fb_push(b, Sym(sl, sp, bind_name(sl, name, sp)));
}

/* (let [^mut p : any p] body) for every parameter in `params` that `body`
 * sets -- the mutable-local rebinding push_param promises.  Not the rest
 * parameter: rebind_rest has already bound it, mutable when it is set. */
static Form *rebind_muts(SL *sl, Span sp, const Form *params, Form *body) {
    FB b = {0};
    for (uint32_t i = 0; i < params->as.list.len; i++) {
        const Form *p = params->as.list.items[i];
        if (p->tag == F_SYM && p->as.sym == sl->t_amp) break;
        if (p->tag != F_SYM) continue;
        if (form_sets(sl, p->as.sym, body))
            push_binding(sl, &b, sp, p->as.sym, Sym(sl, sp, p->as.sym), true);
    }
    if (b.n == 0) { free(b.items); return body; }
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

/* A sequence of expressions (a cond clause body, a `begin`): one form, or a
 * `do`.  Not a body: no internal defines. */
static Form *lower_seq(SL *sl, Form **items, uint32_t n, Span sp) {
    if (n == 0) return Nil(sl, sp);
    if (n == 1) return lower(sl, items[0]);
    FB b = {0};
    fb_push(&b, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 0; i < n; i++) fb_push(&b, lower(sl, items[i]));
    return fb_list(sl, &b, sp);
}

/* Formals -> a Turmeric parameter vector.  `(a b)`, `(a b . rest)` and a
 * bare `rest` symbol; the rest parameter is `& rest : any`, and its (renamed)
 * symbol comes back through `out_rest` so the body can rebind it -- inside
 * the body a rest parameter is the carrier `int` list, and the elements
 * were packed as `(Cons any)` cells at the call site, so `(:: rest (Cons
 * any))` is the ascription that gives the list its type back. */
static Form *lower_formals(SL *sl, Form *formals, bool *ok, const Symbol **out_rest) {
    FB b = {0};
    *ok = true;
    if (out_rest) *out_rest = NULL;
    if (formals->tag == F_SYM) {
        const Symbol *r = bind_name(sl, formals->as.sym, formals->span);
        fb_push(&b, Sym(sl, formals->span, sl->t_amp));
        fb_push(&b, Sym(sl, formals->span, r));
        fb_push(&b, AnyAnn(sl, formals->span));
        if (out_rest) *out_rest = r;
        return fb_vec(sl, &b, formals->span);
    }
    formals = nil_to_list(sl, formals);
    if (formals->tag != F_LIST) {
        err(formals, "lambda formals must be a list of identifiers, a single "
                     "identifier, or `(a b . rest)`");
        *ok = false;
        free(b.items);
        return Vec(sl, formals->span, NULL, 0);
    }
    uint32_t n = formals->as.list.len;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) {
                err(p, "malformed rest formal: expected `(a b . rest)`");
                *ok = false;
                break;
            }
            const Symbol *r = bind_name(sl, formals->as.list.items[n - 1]->as.sym, p->span);
            fb_push(&b, Sym(sl, p->span, sl->t_amp));
            fb_push(&b, Sym(sl, p->span, r));
            fb_push(&b, AnyAnn(sl, p->span));
            if (out_rest) *out_rest = r;
            break;
        }
        if (p->tag != F_SYM) {
            err(p, "lambda formal must be an identifier");
            *ok = false;
            break;
        }
        push_param(sl, &b, p->span, p->as.sym);
    }
    return fb_vec(sl, &b, formals->span);
}

/* (let [rest (r7rs-chain->list__ (:: rest (Cons any)))] body) -- see
 * lower_formals.  A variadic `& r : any` arrives as the `(Cons any)` chain
 * Saffron's widen builds; R3 gives Scheme lists their own representation
 * (R7rsPair / the null singleton, so `set-cdr!` and dotted tails work), and
 * the chain is converted at the one place it enters Scheme code, so a rest
 * parameter is a Scheme list like every other list the program sees. */
static Form *rebind_rest(SL *sl, Span sp, const Symbol *rest, Form *body) {
    if (!rest) return body;
    Form *cons_any = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "Cons")), Sym(sl, sp, sl->t_any));
    Form *asc = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), Sym(sl, sp, rest), cons_any);
    Form *conv = Ln(sl, sp, 2, Sym(sl, sp, sl->p_chain_to_list), asc);
    FB b = {0};
    push_binding(sl, &b, sp, rest, conv, form_sets(sl, rest, body));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

/* (fn [params] body') */
static Form *lower_lambda_parts(SL *sl, Span sp, Form *formals,
                                Form **body, uint32_t nbody) {
    bool ok;
    const Symbol *rest = NULL;
    LFrame *saved = scope_open(sl);
    Form *params = lower_formals(sl, formals, &ok, &rest);
    if (!ok) { scope_close(sl, saved); return Nil(sl, sp); }
    if (nbody == 0) {
        err(formals, "lambda needs a body");
        scope_close(sl, saved);
        return Nil(sl, sp);
    }
    /* A top-level statement's thunk runs at once: its body is as eager as
     * the statement (r7rs-saved-standard-procedure-follows-redefinition). */
    uint32_t defer = sl->eager_thunk ? 0 : 1;
    sl->eager_thunk = false;
    sl->deferred += defer;
    Form *lowered = lower_body(sl, body, nbody, sp);
    sl->deferred -= defer;
    lowered = rebind_rest(sl, sp, rest, lowered);
    lowered = rebind_muts(sl, sp, params, lowered);
    scope_close(sl, saved);
    /* R6 (docs/archive/r7rs-compiled-dynamic-shapes.md section 2): every
     * Scheme procedure returns `any`, spelled out.  An unannotated lambda
     * whose body ends in a nil-returning call (`display`) was typed
     * `(fn [any] nil)`, and the compiled dynamic call -- which checks the
     * box against the all-`any` signature -- refused it. */
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), params, AnyAnn(sl, sp), lowered);
}

static Form *lower_lambda(SL *sl, Form *f) {
    if (f->as.list.len < 3) {
        err(f, "lambda expects (lambda formals body...)");
        return Nil(sl, f->span);
    }
    return lower_lambda_parts(sl, f->span, f->as.list.items[1],
                              f->as.list.items + 2, f->as.list.len - 2);
}

/* Parse `((name init) ...)` into parallel arrays.  Returns false on a shape
 * error (already reported). */
static bool parse_bindings(SL *sl, Form *blist, const Symbol ***out_names,
                           Form ***out_inits, uint32_t *out_n) {
    blist = nil_to_list(sl, blist);
    if (blist->tag != F_LIST) {
        err(blist, "expected a list of (name init) bindings");
        return false;
    }
    uint32_t n = blist->as.list.len;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(*inits));
    for (uint32_t i = 0; i < n; i++) {
        Form *b = blist->as.list.items[i];
        if (b->tag == F_SYM) {              /* `(let (x) ...)`: unspecified init */
            names[i] = b->as.sym;
            inits[i] = Nil(sl, b->span);
            continue;
        }
        if (b->tag != F_LIST || b->as.list.len < 1 || b->as.list.len > 2 ||
            b->as.list.items[0]->tag != F_SYM) {
            err(b, "binding must be (name init)");
            return false;
        }
        names[i] = b->as.list.items[0]->as.sym;
        inits[i] = (b->as.list.len == 2) ? b->as.list.items[1] : Nil(sl, b->span);
    }
    *out_names = names; *out_inits = inits; *out_n = n;
    return true;
}

/* (let [b1 i1 ...] body) -- one binding vector. */
static Form *make_let(SL *sl, Span sp, const Symbol **names, Form **inits,
                      uint32_t n, Form *body) {
    FB b = {0};
    for (uint32_t i = 0; i < n; i++)
        push_binding(sl, &b, sp, names[i], inits[i],
                     form_sets(sl, rn(sl, names[i]), body));
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
}

static Form *lower_let(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t len = f->as.list.len;
    if (len < 3) { err(f, "let expects (let bindings body...)"); return Nil(sl, sp); }
    Form *second = f->as.list.items[1];

    /* Turmeric `(let [x 1] ...)` inside a Scheme file: pass through. */
    if (second->tag == F_VEC) return NULL;

    if (second->tag == F_SYM) {
        /* Named let: (letrec [name (fn [vars] body')] (name inits'...)) */
        if (len < 4) { err(f, "named let expects (let name bindings body...)"); return Nil(sl, sp); }
        const Symbol **names; Form **inits; uint32_t n;
        if (!parse_bindings(sl, f->as.list.items[2], &names, &inits, &n)) return Nil(sl, sp);
        /* R10: the inits are in the OUTER scope; the loop name and the
         * variables are the body's. */
        Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
        for (uint32_t i = 0; i < n; i++) linits[i] = lower(sl, inits[i]);
        LFrame *saved = scope_open(sl);
        const Symbol *loop = bind_name(sl, second->as.sym, second->span);
        LFrame *vars_saved = scope_open(sl);
        FB params = {0};
        for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
        Form *pvec = fb_vec(sl, &params, sp);
        Form *lbody = rebind_muts(sl, sp, pvec,
                                  lower_body(sl, f->as.list.items + 3, len - 3, sp));
        scope_close(sl, vars_saved);
        Form *fn = Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), pvec, lbody);
        FB call = {0};
        fb_push(&call, Sym(sl, sp, loop));
        for (uint32_t i = 0; i < n; i++) fb_push(&call, linits[i]);
        scope_close(sl, saved);
        Form *bvec[2] = { Sym(sl, sp, loop), fn };
        return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), Vec(sl, sp, bvec, 2),
                  fb_list(sl, &call, sp));
    }

    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, second, &names, &inits, &n)) return Nil(sl, sp);
    /* R10: every init in the OUTER scope, then the binders -- each a unique
     * name, so Turmeric's sequential `let` keeps Scheme's parallel meaning
     * with no temporaries (an init that names a binder means the outer one,
     * which has another name). */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) linits[i] = lower(sl, inits[i]);
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_name(sl, names[i], sp);
    Form *body = lower_body(sl, f->as.list.items + 2, len - 2, sp);
    Form *r = n == 0 ? body : make_let(sl, sp, names, linits, n, body);
    scope_close(sl, saved);
    return r;
}

/* let*: nested single-binding lets, innermost last. */
static Form *lower_letstar(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "let* expects (let* bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric let* */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    /* Each init sees the bindings before it: one scope per binding (R10). */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    LFrame **saved = (LFrame **)arena_alloc(sl->a, (n + 1) * sizeof(LFrame *));
    for (uint32_t i = 0; i < n; i++) {
        linits[i] = lower(sl, inits[i]);
        saved[i] = scope_open(sl);
        bind_name(sl, names[i], sp);
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
        body = make_let(sl, sp, names + i, linits + i, 1, body);
        scope_close(sl, saved[i]);
    }
    return body;
}

/* R10: does the lowered `f` mention one of `names` other than as a call's
 * head?  A letrec lambda that CALLS a sibling compiles to a direct call; one
 * that takes a sibling as a VALUE (`(eqv? f g)`, `(map g xs)`) captures it,
 * and the compiled letrec filled that capture before the sibling existed
 * (cc: 'f_N' undeclared -- chibi's `(letrec ((f (lambda () (eqv? f g)))
 * ...))`).  Over-approximates (a shadowing parameter counts): the fallback
 * below is correct for every group, only slower. */
static bool mentions_as_value(const Form *f, const Symbol **names, uint32_t n) {
    if (!f) return false;
    if (f->tag == F_SYM) {
        for (uint32_t i = 0; i < n; i++) if (f->as.sym == names[i]) return true;
        return false;
    }
    if (f->tag == F_QUOTE) return false;
    if (f->tag != F_LIST && f->tag != F_VEC) return false;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Form *it = f->as.list.items[i];
        if (i == 0 && f->tag == F_LIST && it->tag == F_SYM) continue;
        if (mentions_as_value(it, names, n)) return true;
    }
    return false;
}
/* A letrec over `pairs` (renamed name, lowered init, alternating; consumed).
 * When some init takes a group member as a value, the group is instead a set
 * of `any` cells assigned in order -- letrec* (R7RS 4.2.2) -- which the
 * assignment conversion then turns into shared boxes. */
static Form *make_letrec(SL *sl, Span sp, FB *pairs, Form *body) {
    uint32_t n = pairs->n / 2;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    for (uint32_t i = 0; i < n; i++) names[i] = pairs->items[2 * i]->as.sym;
    bool as_value = false;
    for (uint32_t i = 0; i < n && !as_value; i++)
        as_value = mentions_as_value(pairs->items[2 * i + 1], names, n);
    if (!as_value) return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), fb_vec(sl, pairs, sp), body);
    FB cells = {0}, seq = {0};
    fb_push(&seq, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&cells, Sym(sl, sp, sl->t_mut));
        fb_push(&cells, Sym(sl, sp, names[i]));
        fb_push(&cells, AnyAnn(sl, sp));
        fb_push(&cells, Bool(sl, sp, false));
        fb_push(&seq, Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, names[i]), pairs->items[2 * i + 1]));
    }
    free(pairs->items);
    fb_push(&seq, body);
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &cells, sp), fb_list(sl, &seq, sp));
}

/* letrec / letrec*: Turmeric's letrec (a lambda may name any sibling). */
static Form *lower_letrec(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "letrec expects (letrec bindings body...)"); return Nil(sl, sp); }
    if (f->as.list.items[1]->tag == F_VEC) return NULL;   /* Turmeric letrec */
    const Symbol **names; Form **inits; uint32_t n;
    if (!parse_bindings(sl, f->as.list.items[1], &names, &inits, &n)) return Nil(sl, sp);
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_name(sl, names[i], sp);
    FB b = {0};
    for (uint32_t i = 0; i < n; i++) {
        fb_push(&b, Sym(sl, sp, rn(sl, names[i])));
        fb_push(&b, lower(sl, inits[i]));
    }
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    Form *r = n == 0 ? body : make_letrec(sl, sp, &b, body);
    if (n == 0) free(b.items);
    scope_close(sl, saved);
    return r;
}

/* A Scheme `do` loop has the shape (do ((var init step)...) (test res...)
 * cmd...); anything else with that head is Turmeric's `do` sequence. */
static bool looks_like_scheme_do(const Form *f) {
    if (f->as.list.len < 3) return false;
    const Form *specs = f->as.list.items[1];
    const Form *test  = f->as.list.items[2];
    if (specs->tag == F_NIL) return test->tag == F_LIST;
    if (specs->tag != F_LIST || test->tag != F_LIST) return false;
    for (uint32_t i = 0; i < specs->as.list.len; i++)
        if (specs->as.list.items[i]->tag != F_LIST) return false;
    return true;
}

static Form *lower_do(SL *sl, Form *f) {
    Span sp = f->span;
    Form *specs = nil_to_list(sl, f->as.list.items[1]);
    Form *test  = f->as.list.items[2];
    uint32_t n = specs->as.list.len;
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, (n + 1) * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    Form **steps = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        if (s->as.list.len < 2 || s->as.list.len > 3 || s->as.list.items[0]->tag != F_SYM) {
            err(s, "do binding must be (var init [step])");
            return Nil(sl, sp);
        }
        names[i] = s->as.list.items[0]->as.sym;
        inits[i] = lower(sl, s->as.list.items[1]);     /* the OUTER scope (R10) */
    }
    if (test->as.list.len < 1) { err(test, "do needs (test result...)"); return Nil(sl, sp); }
    const Symbol *loop = fresh(sl, "__r7rs_do");
    LFrame *saved = scope_open(sl);
    FB params = {0};
    for (uint32_t i = 0; i < n; i++) push_param(sl, &params, sp, names[i]);
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        steps[i] = (s->as.list.len == 3) ? lower(sl, s->as.list.items[2])
                                         : Sym(sl, sp, rn(sl, names[i]));
    }
    Form *result = lower_seq(sl, test->as.list.items + 1, test->as.list.len - 1, sp);
    /* (do cmds'... (loop steps'...)) */
    FB again = {0};
    fb_push(&again, Sym(sl, sp, sl->t_do));
    for (uint32_t i = 3; i < f->as.list.len; i++) fb_push(&again, lower(sl, f->as.list.items[i]));
    FB recur = {0};
    fb_push(&recur, Sym(sl, sp, loop));
    for (uint32_t i = 0; i < n; i++) fb_push(&recur, steps[i]);
    fb_push(&again, fb_list(sl, &recur, sp));
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), lower(sl, test->as.list.items[0]),
                    result, fb_list(sl, &again, sp));
    Form *pvec = fb_vec(sl, &params, sp);
    body = rebind_muts(sl, sp, pvec, body);
    scope_close(sl, saved);
    Form *fn = Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), pvec, body);
    FB call = {0};
    fb_push(&call, Sym(sl, sp, loop));
    for (uint32_t i = 0; i < n; i++) fb_push(&call, inits[i]);
    Form *bvec[2] = { Sym(sl, sp, loop), fn };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_letrec), Vec(sl, sp, bvec, 2),
              fb_list(sl, &call, sp));
}

static Form *lower_if(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t len = f->as.list.len;
    if (len != 3 && len != 4) { err(f, "if expects (if test then [else])"); return Nil(sl, sp); }
    Form *c = lower(sl, f->as.list.items[1]);
    Form *t = lower(sl, f->as.list.items[2]);
    Form *e = (len == 4) ? lower(sl, f->as.list.items[3]) : Nil(sl, sp);
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, t, e);
}

/* Scheme `cond` clauses are lists; Turmeric's are a flat pair-up. */
static bool looks_like_scheme_cond(const Form *f) {
    if (f->as.list.len < 2) return false;
    for (uint32_t i = 1; i < f->as.list.len; i++)
        if (f->as.list.items[i]->tag != F_LIST || f->as.list.items[i]->as.list.len == 0)
            return false;
    return true;
}

/* A template-style alias of a global or keyword (hyg_resolve_free's kind):
 * whatever the use site binds the name to, the alias means the global. */
static const Symbol *global_alias_of(SL *sl, const char *name) {
    const Symbol *x = I(sl, name);
    char pre[160];
    snprintf(pre, sizeof pre, "%s__g", name);
    const Symbol *a = fresh(sl, pre);
    sym_pair_push(sl, &sl->ga_from, &sl->ga_to, &sl->n_ga, &sl->cap_ga, a, x);
    return a;
}
/* SRFI 61: `(generator guard => receiver)` -- the generator's values go to
 * guard, and when it answers true, to receiver, whose values are the cond's;
 * otherwise the next clause.  As the SRFI's reference implementation:
 *   (call-with-values (lambda () generator)
 *     (lambda vals (if (apply guard vals) (apply receiver vals) <rest>)))
 * built from global aliases, so a local `apply` or `lambda` at the use site
 * does not capture it, and `vals` fresh, so it captures nothing of the
 * user's. */
static Form *srfi61_clause(SL *sl, Form *cl, Form **rest, uint32_t nrest, Span sp) {
    Span s = cl->span;
    Form **it = cl->as.list.items;
    const Symbol *cwv = global_alias_of(sl, "call-with-values");
    const Symbol *lam = global_alias_of(sl, "lambda");
    const Symbol *iff = global_alias_of(sl, "if");
    const Symbol *app = global_alias_of(sl, "apply");
    const Symbol *vals = fresh(sl, "srfi61_vals__");
    Form *after;
    if (nrest > 0) {
        FB c = {0};
        fb_push(&c, Sym(sl, sp, global_alias_of(sl, "cond")));
        for (uint32_t i = 0; i < nrest; i++) fb_push(&c, rest[i]);
        after = fb_list(sl, &c, sp);
    } else {
        after = Ln(sl, s, 3, Sym(sl, s, iff), Bool(sl, s, false), Bool(sl, s, false));   /* unspecified */
    }
    Form *test = Ln(sl, s, 3, Sym(sl, s, app), it[1], Sym(sl, s, vals));
    Form *recv = Ln(sl, s, 3, Sym(sl, s, app), it[3], Sym(sl, s, vals));
    Form *body = Ln(sl, s, 4, Sym(sl, s, iff), test, recv, after);
    Form *consumer = Ln(sl, s, 3, Sym(sl, s, lam), Sym(sl, s, vals), body);
    Form *producer = Ln(sl, s, 3, Sym(sl, s, lam), Ln(sl, s, 0), it[0]);
    return lower(sl, Ln(sl, s, 3, Sym(sl, s, cwv), producer, consumer));
}

/* SRFI 17: a `set!` whose target is a call form -- `(set! (f arg ...) v)`.
 * Turmeric's own targets are not this shape: `(.field x)` and `(@ p)` have
 * heads that are not Scheme identifiers, and the prelude, which writes them,
 * is excluded at the use site. */
static bool is_srfi17_place(const Form *f) {
    if (f->as.list.len != 3) return false;
    const Form *p = f->as.list.items[1];
    if (p->tag != F_LIST || p->as.list.len == 0) return false;
    const Form *h = p->as.list.items[0];
    if (h->tag == F_SYM && (h->as.sym->name[0] == '.' || h->as.sym->name[0] == '@')) return false;
    return true;
}
/* `(set! (f arg ...) v)` is `((setter f) arg ... v)`, as SRFI 17 defines it:
 * `f` and the arguments are ordinary expressions, evaluated once each.
 * `setter` is the SRFI's own -- its spliced definition, reached through a
 * global alias, so a local `setter` at the use site does not capture it and
 * the program need not have imported the name at all. */
static const Symbol *srfi_spelling(SL *sl, int64_t num, const Symbol *name);
static Form *srfi17_place(SL *sl, Form *f) {
    Span sp = f->span;
    Form *place = f->as.list.items[1];
    const Symbol *setter = global_alias_of(sl, srfi_spelling(sl, 17, I(sl, "setter"))->name);
    FB call = {0};
    fb_push(&call, Ln(sl, sp, 2, Sym(sl, sp, setter), place->as.list.items[0]));
    for (uint32_t i = 1; i < place->as.list.len; i++) fb_push(&call, place->as.list.items[i]);
    fb_push(&call, f->as.list.items[2]);
    return lower(sl, fb_list(sl, &call, sp));
}

static Form *cond_chain(SL *sl, Form **clauses, uint32_t n, Span sp) {
    if (n == 0) return Nil(sl, sp);
    Form *cl = clauses[0];
    Form **it = cl->as.list.items;
    uint32_t len = cl->as.list.len;
    if (kw_is(sl, it[0], sl->s_else))
        return lower_seq(sl, it + 1, len - 1, cl->span);
    Form *rest = cond_chain(sl, clauses + 1, n - 1, sp);
    Form *test = lower(sl, it[0]);
    if (len == 1) {
        /* (test): the test's value is the result. */
        const Symbol *t = fresh(sl, "__r7rs_cond");
        Form *body = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if),
                        Sym(sl, cl->span, t), Sym(sl, cl->span, t), rest);
        Form *bv[2] = { Sym(sl, cl->span, t), test };
        return Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->t_let), Vec(sl, cl->span, bv, 2), body);
    }
    if (len == 4 && kw_is(sl, it[2], sl->s_arrow)) {
        if (sl->srfi61_cond) return srfi61_clause(sl, cl, clauses + 1, n - 1, sp);
        err(cl, "a (generator guard => receiver) cond clause is SRFI 61's; import (srfi 61) to use it");
        return Nil(sl, sp);
    }
    if (kw_is(sl, it[1], sl->s_arrow)) {
        if (len != 3) { err(cl, "cond clause with => expects (test => receiver)"); return Nil(sl, sp); }
        const Symbol *t = fresh(sl, "__r7rs_cond");
        Form *call = Ln(sl, cl->span, 2, lower(sl, it[2]), Sym(sl, cl->span, t));
        Form *body = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if),
                        Sym(sl, cl->span, t), call, rest);
        Form *bv[2] = { Sym(sl, cl->span, t), test };
        return Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->t_let), Vec(sl, cl->span, bv, 2), body);
    }
    return Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
              lower_seq(sl, it + 1, len - 1, cl->span), rest);
}

/* A datum in a `case` clause: symbols are quoted, everything else is itself. */
static Form *case_datum(SL *sl, Form *d) {
    if (d->tag == F_SYM) return form_quote(sl->a, d->span, d);
    return d;
}

static bool looks_like_scheme_case(const SL *sl, const Form *f) {
    if (f->as.list.len < 3) return false;
    for (uint32_t i = 2; i < f->as.list.len; i++) {
        const Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len < 2) return false;
        const Form *d = cl->as.list.items[0];
        if (d->tag != F_LIST && !is_sym(d, sl->s_else)) return false;
    }
    return true;
}

static Form *lower_case(SL *sl, Form *f) {
    Span sp = f->span;
    const Symbol *k = fresh(sl, "__r7rs_case");
    Form *chain = Nil(sl, sp);
    for (int32_t i = (int32_t)f->as.list.len - 1; i >= 2; i--) {
        Form *cl = f->as.list.items[i];
        Form **it = cl->as.list.items;
        uint32_t len = cl->as.list.len;
        Form *body;
        if (len >= 3 && kw_is(sl, it[1], sl->s_arrow)) {
            body = Ln(sl, cl->span, 2, lower(sl, it[2]), Sym(sl, cl->span, k));
        } else {
            body = lower_seq(sl, it + 1, len - 1, cl->span);
        }
        if (kw_is(sl, it[0], sl->s_else)) { chain = body; continue; }
        if (it[0]->tag != F_LIST) { err(cl, "case clause expects ((datum...) body...) or (else body...)"); return Nil(sl, sp); }
        /* (if (eqv? k d1) true (if (eqv? k d2) true false)) -- static bools. */
        Form *test = Bool(sl, cl->span, false);
        for (int32_t j = (int32_t)it[0]->as.list.len - 1; j >= 0; j--) {
            Form *eq = Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_eqv),
                          Sym(sl, cl->span, k), case_datum(sl, it[0]->as.list.items[j]));
            test = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), eq,
                      Bool(sl, cl->span, true), test);
        }
        chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test, body, chain);
    }
    Form *bv[2] = { Sym(sl, sp, k), lower(sl, f->as.list.items[1]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), chain);
}

/* (and) -> #t; (and a) -> a; (and a b...) -> (let [t a] (if t (and b...) t)) */
static Form *and_chain(SL *sl, Form **args, uint32_t n, Span sp) {
    if (n == 0) return Bool(sl, sp, true);
    if (n == 1) return lower(sl, args[0]);
    const Symbol *t = fresh(sl, "__r7rs_and");
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), Sym(sl, sp, t),
                    and_chain(sl, args + 1, n - 1, sp), Sym(sl, sp, t));
    Form *bv[2] = { Sym(sl, sp, t), lower(sl, args[0]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
}
/* (or) -> #f; (or a) -> a; (or a b...) -> (let [t a] (if t t (or b...))) */
static Form *or_chain(SL *sl, Form **args, uint32_t n, Span sp) {
    if (n == 0) return Bool(sl, sp, false);
    if (n == 1) return lower(sl, args[0]);
    const Symbol *t = fresh(sl, "__r7rs_or");
    Form *body = Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), Sym(sl, sp, t),
                    Sym(sl, sp, t), or_chain(sl, args + 1, n - 1, sp));
    Form *bv[2] = { Sym(sl, sp, t), lower(sl, args[0]) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
}

/* R6: a zero-parameter `(fn [] : any body)`. */
static Form *thunk_of(SL *sl, Span sp, Form *body) {
    return Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), Vec(sl, sp, NULL, 0), AnyAnn(sl, sp), body);
}

/* R6: (guard (var clause...) body...) -- R7RS 4.2.7, over the escape
 * continuation and the handler stack (prelude r7rs-call/cc,
 * r7rs-with-exception-handler):
 *
 *   ((r7rs-call/cc (fn [k] : any
 *      (r7rs-with-exception-handler
 *        (fn [var] : any (k (fn [] : any (cond clause... (else (raise-continuable var))))))
 *        (fn [] : any (let [v body'] (fn [] : any v)))))))
 *
 * Both arms hand the continuation a THUNK, so the clauses are evaluated after
 * the escape, in the guard's own dynamic environment, and a clause-less
 * exception is re-raised from there with `raise-continuable` -- R7RS asks for
 * the raise's dynamic environment, which needs a re-entrant continuation
 * (D7); with escapes only, this is the reachable reading. */
static Form *lower_guard(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_LIST ||
        f->as.list.items[1]->as.list.len < 1 ||
        f->as.list.items[1]->as.list.items[0]->tag != F_SYM) {
        err(f, "guard expects (guard (var clause...) body...)");
        return Nil(sl, sp);
    }
    Form *spec = f->as.list.items[1];
    const Symbol *var = spec->as.list.items[0]->as.sym;
    uint32_t ncl = spec->as.list.len - 1;
    Form **given = spec->as.list.items + 1;
    for (uint32_t i = 0; i < ncl; i++)
        if (given[i]->tag != F_LIST || given[i]->as.list.len == 0) {
            err(given[i], "guard clause expects (test body...) or (else body...)");
            return Nil(sl, sp);
        }
    bool has_else = ncl > 0 && kw_is(sl, given[ncl - 1]->as.list.items[0], sl->s_else);
    Form **cls = (Form **)arena_alloc(sl->a, (ncl + 1) * sizeof(Form *));
    if (ncl) memcpy(cls, given, ncl * sizeof(Form *));
    if (!has_else) {
        Form *reraise = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-raise-continuable")), Sym(sl, sp, var));
        cls[ncl++] = Ln(sl, sp, 2, Sym(sl, sp, sl->s_else), reraise);
    }
    /* R10: the variable is the clauses' binder, not the body's. */
    LFrame *saved = scope_open(sl);
    FB pb = {0};
    push_param(sl, &pb, spec->as.list.items[0]->span, var);
    Form *hparams = fb_vec(sl, &pb, sp);
    Form *chain = cond_chain(sl, cls, ncl, sp);
    const Symbol *k = fresh(sl, "__r7rs_guard_k");
    const Symbol *v = fresh(sl, "__r7rs_guard_v");
    Form *hbody = rebind_muts(sl, sp, hparams, Ln(sl, sp, 2, Sym(sl, sp, k), thunk_of(sl, sp, chain)));
    scope_close(sl, saved);
    Form *handler = Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), hparams, AnyAnn(sl, sp), hbody);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    Form *bv[2] = { Sym(sl, sp, v), body };
    Form *bthunk = thunk_of(sl, sp, Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2),
                                       thunk_of(sl, sp, Sym(sl, sp, v))));
    Form *weh = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-with-exception-handler")), handler, bthunk);
    Form *kp = Sym(sl, sp, k);
    Form *recv = Ln(sl, sp, 4, Sym(sl, sp, sl->t_fn), Vec(sl, sp, &kp, 1), AnyAnn(sl, sp), weh);
    Form *cc = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-call/ec__")), recv);
    return Ln(sl, sp, 1, cc);
}

/* R6: (parameterize ((p v) ...) body...) ->
 *   (r7rs-parameterize__ (r7rs-list (r7rs-cons p v) ...) (fn [] : any body')) */
static Form *lower_parameterize(SL *sl, Form *f) {
    Span sp = f->span;
    if (f->as.list.len < 3) {
        err(f, "parameterize expects (parameterize ((param value) ...) body...)");
        return Nil(sl, sp);
    }
    Form *bl = nil_to_list(sl, f->as.list.items[1]);
    if (bl->tag != F_LIST) {
        err(bl, "parameterize expects a list of (param value) bindings");
        return Nil(sl, sp);
    }
    FB lb = {0};
    fb_push(&lb, Sym(sl, sp, sl->p_list));
    for (uint32_t i = 0; i < bl->as.list.len; i++) {
        Form *b = bl->as.list.items[i];
        if (b->tag != F_LIST || b->as.list.len != 2) {
            err(b, "parameterize binding expects (param value)");
            free(lb.items);
            return Nil(sl, sp);
        }
        fb_push(&lb, Ln(sl, b->span, 3, Sym(sl, b->span, sl->p_cons),
                        lower(sl, b->as.list.items[0]), lower(sl, b->as.list.items[1])));
    }
    Form *bindings = fb_list(sl, &lb, sp);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    return Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-parameterize__")), bindings, thunk_of(sl, sp, body));
}

/* R6: (delay expr) -> (r7rs-delay-force__ (fn [] : any (r7rs-make-promise expr')))
 *     (delay-force expr) -> (r7rs-delay-force__ (fn [] : any expr'))
 * -- the R7RS 7.3 definitions, `delay` being `delay-force` of a forced
 * promise. */
static Form *lower_delay(SL *sl, Form *f, bool is_force) {
    Span sp = f->span;
    if (f->as.list.len != 2) {
        err(f, "%s expects one expression", is_force ? "delay-force" : "delay");
        return Nil(sl, sp);
    }
    sl->deferred++;
    Form *x = lower(sl, f->as.list.items[1]);
    sl->deferred--;
    if (!is_force) x = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-make-promise")), x);
    return Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-delay-force__")), thunk_of(sl, sp, x));
}

static Form *lower_when_unless(SL *sl, Form *f, bool negate) {
    Span sp = f->span;
    if (f->as.list.len < 3) { err(f, "%s expects (test body...)", negate ? "unless" : "when"); return Nil(sl, sp); }
    Form *c = lower(sl, f->as.list.items[1]);
    Form *body = lower_seq(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    return negate ? Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, Nil(sl, sp), body)
                  : Ln(sl, sp, 4, Sym(sl, sp, sl->t_if), c, body, Nil(sl, sp));
}

/* (case-lambda (formals body...)...) -> a variadic fn that dispatches on the
 * argument count and applies the matching clause as a lambda. */
static Form *lower_case_lambda(SL *sl, Form *f) {
    Span sp = f->span;
    const Symbol *args = fresh(sl, "__r7rs_args");
    const Symbol *nsym = fresh(sl, "__r7rs_nargs");
    /* R6: the no-match arm is the prelude's `any`-typed failure, not a bare
     * `panic`: a never-typed else arm nested in the clause chain lost the
     * outer arms' result assignments on the compiled path, so a two-clause
     * case-lambda answered its second clause with an untagged word. */
    Form *chain = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-fail-any__")),
                     form_str(sl->a, sp, "case-lambda: no clause matches the argument count", 50));
    for (int32_t i = (int32_t)f->as.list.len - 1; i >= 1; i--) {
        Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len < 2) { err(cl, "case-lambda clause expects (formals body...)"); return Nil(sl, sp); }
        Form *formals = cl->as.list.items[0];
        uint32_t fixed = 0; bool has_rest = false;
        if (formals->tag == F_SYM) { has_rest = true; }
        else if (formals->tag == F_LIST) {
            for (uint32_t j = 0; j < formals->as.list.len; j++) {
                if (is_sym(formals->as.list.items[j], sl->s_dot)) { has_rest = true; break; }
                fixed++;
            }
        } else { err(formals, "case-lambda formals must be a list or an identifier"); return Nil(sl, sp); }
        /* A rest clause takes its rest as ONE list parameter: the clause lambda
         * is built from formals with the dot removed (`(a b . more)` ->
         * `(a b more)`, a bare `more` -> `(more)`) and called with the fixed
         * arguments and `(list-tail args fixed)`.  Same body semantics, and no
         * dynamic call has to spread a list into a variadic closure, which
         * neither back end's apply helpers do. */
        Form *clause_formals = formals;
        if (has_rest) {
            FB nf = {0};
            if (formals->tag == F_SYM) {
                fb_push(&nf, formals);
            } else {
                for (uint32_t j = 0; j < formals->as.list.len; j++) {
                    Form *p = formals->as.list.items[j];
                    if (is_sym(p, sl->s_dot)) continue;
                    fb_push(&nf, p);
                }
            }
            clause_formals = fb_list(sl, &nf, formals->span);
        }
        Form *lam = lower_lambda_parts(sl, cl->span, clause_formals, cl->as.list.items + 1, cl->as.list.len - 1);
        /* (lam (list-ref args 0) ... [(list-tail args fixed)]) */
        FB call = {0};
        fb_push(&call, lam);
        for (uint32_t j = 0; j < fixed; j++)
            fb_push(&call, Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_list_ref),
                              Sym(sl, cl->span, args), Int(sl, cl->span, (int64_t)j)));
        if (has_rest) {
            fb_push(&call, Ln(sl, cl->span, 3, Sym(sl, cl->span, sl->p_list_tail),
                              Sym(sl, cl->span, args), Int(sl, cl->span, (int64_t)fixed)));
            Form *test = Ln(sl, cl->span, 3, Sym(sl, cl->span, I(sl, ">=")),
                            Sym(sl, cl->span, nsym), Int(sl, cl->span, (int64_t)fixed));
            chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
                       fb_list(sl, &call, cl->span), chain);
            continue;
        }
        Form *test = Ln(sl, cl->span, 3, Sym(sl, cl->span, I(sl, "=")),
                        Sym(sl, cl->span, nsym), Int(sl, cl->span, (int64_t)fixed));
        chain = Ln(sl, cl->span, 4, Sym(sl, cl->span, sl->t_if), test,
                   fb_list(sl, &call, cl->span), chain);
    }
    Form *nbind[2] = { Sym(sl, sp, nsym),
                       Ln(sl, sp, 2, Sym(sl, sp, sl->p_length), Sym(sl, sp, args)) };
    Form *body = rebind_rest(sl, sp, args,
                             Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, nbind, 2), chain));
    Form *params[3] = { Sym(sl, sp, sl->t_amp), Sym(sl, sp, args), AnyAnn(sl, sp) };
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_fn), Vec(sl, sp, params, 3), body);
}

/* (r7rs-values-ref tmp i) / (r7rs-values-rest tmp i) */
static Form *values_ref(SL *sl, Span sp, const Symbol *tmp, uint32_t i, bool rest) {
    return Ln(sl, sp, 3, Sym(sl, sp, rest ? sl->p_values_rest : sl->p_values_ref),
              Sym(sl, sp, tmp), Int(sl, sp, (int64_t)i));
}

/* Push `formals` bound from the Values carrier in `tmp` onto a binding
 * vector; `body` (lowered) decides which cells are mutable. */
static bool push_values_bindings(SL *sl, FB *b, Form *formals, const Symbol *tmp,
                                 const Form *body) {
    Span sp = formals->span;
    if (formals->tag == F_SYM) {
        push_binding(sl, b, sp, formals->as.sym, values_ref(sl, sp, tmp, 0, true),
                     form_sets(sl, rn(sl, formals->as.sym), body));
        return true;
    }
    formals = nil_to_list(sl, formals);
    if (formals->tag != F_LIST) { err(formals, "formals must be a list or an identifier"); return false; }
    uint32_t n = formals->as.list.len;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) { err(p, "malformed rest formal"); return false; }
            push_binding(sl, b, sp, formals->as.list.items[n - 1]->as.sym, values_ref(sl, sp, tmp, i, true),
                         form_sets(sl, rn(sl, formals->as.list.items[n - 1]->as.sym), body));
            return true;
        }
        if (p->tag != F_SYM) { err(p, "formal must be an identifier"); return false; }
        push_binding(sl, b, sp, p->as.sym, values_ref(sl, sp, tmp, i, false),
                     form_sets(sl, rn(sl, p->as.sym), body));
    }
    return true;
}

/* R10: declare the identifiers of a formals spec in the innermost scope. */
static void bind_formals(SL *sl, Form *formals) {
    if (formals->tag == F_SYM) { bind_name(sl, formals->as.sym, formals->span); return; }
    formals = nil_to_list(sl, formals);
    if (formals->tag != F_LIST) return;
    for (uint32_t i = 0; i < formals->as.list.len; i++) {
        Form *p = formals->as.list.items[i];
        if (p->tag == F_SYM && p->as.sym != sl->s_dot) bind_name(sl, p->as.sym, p->span);
    }
}

/* let-values / let*-values: (let [t1 e1 ...] (let [a (ref t1 0) ...] body)) --
 * the star form nests one binding at a time. */
static Form *lower_let_values(SL *sl, Form *f, bool star) {
    Span sp = f->span;
    if (f->as.list.len >= 2) f->as.list.items[1] = nil_to_list(sl, f->as.list.items[1]);
    if (f->as.list.len < 3 || f->as.list.items[1]->tag != F_LIST) {
        err(f, "%s expects (((formals) init)...) body...", star ? "let*-values" : "let-values");
        return Nil(sl, sp);
    }
    Form *specs = f->as.list.items[1];
    uint32_t n = specs->as.list.len;
    if (star) {
        /* One scope per binding, each init in the scope of those before it. */
        Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
        LFrame **saved = (LFrame **)arena_alloc(sl->a, (n + 1) * sizeof(LFrame *));
        LFrame *outer = sl->scope;
        for (uint32_t i = 0; i < n; i++) {
            Form *s = specs->as.list.items[i];
            if (s->tag != F_LIST || s->as.list.len != 2) {
                err(s, "binding must be (formals init)");
                scope_close(sl, outer);
                return Nil(sl, sp);
            }
            linits[i] = lower(sl, s->as.list.items[1]);
            saved[i] = scope_open(sl);
            bind_formals(sl, s->as.list.items[0]);
        }
        Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
        for (int32_t i = (int32_t)n - 1; i >= 0; i--) {
            Form *s = specs->as.list.items[i];
            const Symbol *tmp = fresh(sl, "__r7rs_vals");
            FB inner = {0};
            if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) {
                free(inner.items);
                scope_close(sl, outer);
                return Nil(sl, sp);
            }
            scope_close(sl, saved[i]);
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &inner, sp), body);
            Form *bv[2] = { Sym(sl, sp, tmp), linits[i] };
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), Vec(sl, sp, bv, 2), body);
        }
        return body;
    }
    FB outer = {0}, inner = {0};
    /* Inits first (source order for temporaries), then the body, then the
     * inner bindings, which need the body to decide mutability. */
    Form **linits = (Form **)arena_alloc(sl->a, (n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        if (s->tag != F_LIST || s->as.list.len != 2) { err(s, "binding must be (formals init)"); return Nil(sl, sp); }
        linits[i] = lower(sl, s->as.list.items[1]);
    }
    LFrame *saved = scope_open(sl);
    for (uint32_t i = 0; i < n; i++) bind_formals(sl, specs->as.list.items[i]->as.list.items[0]);
    Form *body = lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp);
    for (uint32_t i = 0; i < n; i++) {
        Form *s = specs->as.list.items[i];
        const Symbol *tmp = fresh(sl, "__r7rs_vals");
        fb_push(&outer, Sym(sl, sp, tmp));
        fb_push(&outer, linits[i]);
        if (!push_values_bindings(sl, &inner, s->as.list.items[0], tmp, body)) {
            free(outer.items); free(inner.items);
            scope_close(sl, saved);
            return Nil(sl, sp);
        }
    }
    scope_close(sl, saved);
    Form *in = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &inner, sp), body);
    return Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &outer, sp), in);
}

/* `(define-values formals expr)` -> a run of `define`s over a temporary.
 * Returned as SCHEME forms so the caller lowers them in its own context
 * (top level or body). */
static uint32_t expand_define_values(SL *sl, Form *f, FB *out) {
    Span sp = f->span;
    if (f->as.list.len != 3) { err(f, "define-values expects (define-values formals expr)"); return 0; }
    Form *formals = f->as.list.items[1];
    const Symbol *tmp = fresh(sl, "__r7rs_dvals");
    fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), Sym(sl, sp, tmp), f->as.list.items[2]));
    if (formals->tag == F_SYM) {
        fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), formals, values_ref(sl, sp, tmp, 0, true)));
        return 2;
    }
    formals = nil_to_list(sl, formals);
    if (formals->tag != F_LIST) { err(formals, "define-values formals must be a list or an identifier"); return 0; }
    uint32_t n = formals->as.list.len, made = 1;
    for (uint32_t i = 0; i < n; i++) {
        Form *p = formals->as.list.items[i];
        if (is_sym(p, sl->s_dot)) {
            if (i != n - 2 || formals->as.list.items[n - 1]->tag != F_SYM) { err(p, "malformed rest formal"); return 0; }
            fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), formals->as.list.items[n - 1], values_ref(sl, sp, tmp, i, true)));
            made++;
            break;
        }
        if (p->tag != F_SYM) { err(p, "formal must be an identifier"); return 0; }
        fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), p, values_ref(sl, sp, tmp, i, false)));
        made++;
    }
    return made;
}

/* A body: internal defines at the start (R7RS 5.3.2 -- letrec* order), then
 * the expressions.  Lambdas group into a `letrec` so they may be mutually
 * recursive; a value define is a `let` (mutable if ever set!). */
/* R4: a body form whose head is a macro is expanded before the body is
 * classified, so a macro can expand to a define (R7RS 5.3.2); a `begin`
 * splices, and a `define-syntax` registers a macro scoped to this body. */
static void sr_expand_body_item(SL *sl, Form *it, FB *out) {
    it = sr_expand_head(sl, it);
    if (!it) return;
    if (head_is(it, sl->s_define_syntax)) { sr_define(sl, it); return; }
    if (head_is(it, sl->s_begin)) {
        for (uint32_t j = 1; j < it->as.list.len; j++) sr_expand_body_item(sl, it->as.list.items[j], out);
        return;
    }
    fb_push(out, it);
}

static Form *lower_body_inner(SL *sl, Form **items, uint32_t n, Span sp);
static Form *lower_body(SL *sl, Form **items, uint32_t n, Span sp) {
    uint32_t mark = sl->n_macros;
    /* R10: a body is a scope -- its internal defines, and the frame the
     * macros defined at its start resolve their templates in. */
    LFrame *saved = scope_open(sl);
    Form *r = lower_body_inner(sl, items, n, sp);
    scope_close(sl, saved);
    sl->n_macros = mark;
    return r;
}

static Form *lower_body_inner(SL *sl, Form **items, uint32_t n, Span sp) {
    /* Macro uses at the head first (R4), then define-values into plain
     * defines, so one loop sees them. */
    FB pre = {0};
    for (uint32_t i = 0; i < n; i++) sr_expand_body_item(sl, items[i], &pre);
    FB seq = {0};
    for (uint32_t i = 0; i < pre.n; i++) {
        if (head_is(pre.items[i], sl->s_define_values)) expand_define_values(sl, pre.items[i], &seq);
        else fb_push(&seq, pre.items[i]);
    }
    free(pre.items);
    items = seq.items; n = seq.n;
    /* R7RS 5.5: `define-record-type` is a definition, so a body's leading
     * definitions may include one.  Its struct and procedures are top-level
     * declarations, which a body has nowhere to put: they are lifted to the
     * top level under fresh names, and this body's scope maps the names it
     * wrote to them.  Nothing outside the body can reach the type, so one
     * lifted type per source occurrence serves every call of the enclosing
     * procedure (r7rs-define-record-type-not-an-internal-definition). */
    {
        uint32_t w = 0, i = 0;
        for (; i < n; i++) {
            if (head_is(items[i], sl->s_define_record_type)) {
                lower_record_type(sl, items[i], sl->lift_out, true);
                continue;
            }
            if (!head_is(items[i], sl->s_define)) break;
            items[w++] = items[i];
        }
        for (; i < n; i++) items[w++] = items[i];
        n = w;
    }
    if (n == 0) { free(seq.items); return Nil(sl, sp); }

    uint32_t ndef = 0;
    while (ndef < n && head_is(items[ndef], sl->s_define)) ndef++;
    for (uint32_t i = ndef; i < n; i++) {
        if (head_is(items[i], sl->s_define)) {
            err(items[i], "define is only allowed at the beginning of a body (R7RS 5.3.2)");
            free(seq.items);
            return Nil(sl, sp);
        }
    }
    /* R10: the defines are letrec* -- every name is bound before any init
     * or expression of the body is lowered. */
    for (uint32_t i = 0; i < ndef; i++) {
        Form *d = items[i];
        if (d->as.list.len < 2) continue;
        Form *target = d->as.list.items[1];
        if (target->tag == F_LIST && target->as.list.len >= 1 && target->as.list.items[0]->tag == F_SYM)
            bind_name(sl, target->as.list.items[0]->as.sym, target->span);
        else if (target->tag == F_SYM)
            bind_name(sl, target->as.sym, target->span);
    }
    Form *rest = lower_seq(sl, items + ndef, n - ndef, sp);
    if (ndef == 0) { free(seq.items); return rest; }

    /* name / lowered init / is-lambda, in source order. */
    const Symbol **names = (const Symbol **)arena_alloc(sl->a, ndef * sizeof(*names));
    Form **inits = (Form **)arena_alloc(sl->a, ndef * sizeof(Form *));
    bool *is_lam = (bool *)arena_alloc(sl->a, ndef * sizeof(bool));
    for (uint32_t i = 0; i < ndef; i++) {
        Form *d = items[i];
        if (d->as.list.len < 2) { err(d, "define expects (define name expr) or (define (name . formals) body...)"); free(seq.items); return Nil(sl, sp); }
        Form *target = d->as.list.items[1];
        if (target->tag == F_LIST && target->as.list.len >= 1 && target->as.list.items[0]->tag == F_SYM) {
            names[i] = target->as.list.items[0]->as.sym;
            Form *formals = List(sl, target->span, target->as.list.items + 1, target->as.list.len - 1);
            inits[i] = lower_lambda_parts(sl, d->span, formals, d->as.list.items + 2, d->as.list.len - 2);
            is_lam[i] = true;
        } else if (target->tag == F_SYM) {
            if (d->as.list.len != 3) { err(d, "define expects (define name expr)"); free(seq.items); return Nil(sl, sp); }
            names[i] = target->as.sym;
            inits[i] = lower(sl, d->as.list.items[2]);
            is_lam[i] = head_is(d->as.list.items[2], sl->s_lambda);
        } else {
            err(d, "define target must be an identifier or (name . formals)");
            free(seq.items);
            return Nil(sl, sp);
        }
    }
    /* A lambda define that is later `set!` cannot live in a letrec (no
     * mutable letrec cell); it becomes a `let` and loses self-reference by
     * name, which R7RS programs rarely need of a reassigned procedure. */
    for (uint32_t i = 0; i < ndef; i++) {
        if (!is_lam[i]) continue;
        for (uint32_t k = 0; k < n; k++)
            if (form_sets(sl, rn(sl, names[i]), items[k])) { is_lam[i] = false; break; }
    }
    /* r7rs-internal-procedure-value-not-eq: an internal procedure the body
     * hands around as a value -- SICP 3.3.5's `me`, which a connector keeps
     * and later compares with `eq?` -- is one procedure, so one object.  As
     * a letrec function each reference made a fresh closure, and `(eq? me
     * me)` was #f; bound once in a variable (the hoisting below), every
     * reference reads the same one.  A procedure only ever called stays a
     * letrec function. */
    for (uint32_t i = 0; i < ndef; i++) {
        if (!is_lam[i]) continue;
        const Symbol *self = rn(sl, names[i]);
        bool as_value = form_uses_as_value(rest, self);
        for (uint32_t k = 0; k < ndef && !as_value; k++) as_value = form_uses_as_value(inits[k], self);
        if (!as_value) continue;
        is_lam[i] = false;
        /* As an `any`: a variable of function type is boxed again at each
         * use that wants a value. */
        inits[i] = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), inits[i], Sym(sl, sp, sl->t_any));
    }
    /* letrec* (R7RS 5.3.2): every name a body defines is in scope throughout
     * it.  The nesting below binds a value define INSIDE the definitions
     * before it, so an earlier define whose init mentions a later value
     * define -- `(define (a) (set! b 1)) (define b 0)` -- would find `b`
     * unbound.  Such a value define is hoisted: a mutable cell bound around
     * the whole body, assigned in place
     * (r7rs-internal-define-forward-set). */
    bool *hoist = (bool *)arena_alloc(sl->a, ndef * sizeof(bool));
    bool any_hoist = false;
    for (uint32_t j = 0; j < ndef; j++) {
        hoist[j] = false;
        if (is_lam[j]) continue;
        const Symbol *nj = rn(sl, names[j]);
        for (uint32_t i = 0; i < j && !hoist[j]; i++)
            if (form_mentions_sym(inits[i], nj)) hoist[j] = true;
        if (hoist[j]) any_hoist = true;
    }
    free(seq.items);
    Form *body = rest;
    int32_t i = (int32_t)ndef - 1;
    while (i >= 0) {
        if (!is_lam[i] && hoist[i]) {
            const Symbol *self = rn(sl, names[i]);
            Form *store = Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, self), inits[i]);
            body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_do), store, body);
            i--;
            continue;
        }
        if (is_lam[i]) {
            int32_t j = i;
            while (j > 0 && is_lam[j - 1]) j--;
            FB b = {0};
            for (int32_t k = j; k <= i; k++) {
                fb_push(&b, Sym(sl, sp, rn(sl, names[k])));
                fb_push(&b, inits[k]);
            }
            body = make_letrec(sl, sp, &b, body);
            i = j - 1;
        } else {
            const Symbol *self = rn(sl, names[i]);
            if (form_mentions_sym(inits[i], self)) {
                /* R10: letrec* (R7RS 5.3.2) -- a value define whose own init
                 * refers to it, as `(define p (delay ... (force p)))` does.
                 * A plain `let` left that `p` unbound; bind a cell first and
                 * `set!` it, so the init's closure sees the finished value. */
                FB b = {0};
                push_binding(sl, &b, sp, names[i], Bool(sl, sp, false), true);
                Form *store = Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, self), inits[i]);
                body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp),
                          Ln(sl, sp, 3, Sym(sl, sp, sl->t_do), store, body));
            } else {
                body = make_let(sl, sp, names + i, inits + i, 1, body);
            }
            i--;
        }
    }
    if (any_hoist) {
        FB b = {0};
        for (uint32_t j = 0; j < ndef; j++)
            if (hoist[j]) push_binding(sl, &b, sp, names[j], Bool(sl, sp, false), true);
        body = Ln(sl, sp, 3, Sym(sl, sp, sl->t_let), fb_vec(sl, &b, sp), body);
    }
    return body;
}

/* R3 / D4: `quote` constructs runtime data.  A symbol stays the `(quote sym)`
 * the substrate already understands (an interned Sym), an atom is itself, a
 * character literal is the `(r7rs-char__ n)` call the reader produced, a
 * list is `(r7rs-list d...)` (a dotted one a `r7rs-cons` chain ending in the
 * tail datum), a vector `(r7rs-vector d...)`.  Built at runtime each time the
 * expression runs -- the static `.rodata` table D4 wants is deferred with the
 * literal-mutation question (Section 8, Q2). */
/* T1/T2: a literal outside int64 is the reader's `(r7rs-big__ "<digits>")`
 * and an exact non-integer its `(r7rs-ratio__ "<n>/<d>")` (T6: a non-real
 * complex number its `(r7rs-complex__ "<re> <im>")`), data in the same
 * way, so "char form" here means any of the reader's literal calls. */
static bool is_char_form(SL *sl, const Form *f) {
    if (f->tag != F_LIST || f->as.list.len != 2) return false;
    Form *h = f->as.list.items[0], *v = f->as.list.items[1];
    return (is_sym(h, sl->p_char) && v->tag == F_INT) ||
           ((is_sym(h, sl->p_big) || is_sym(h, sl->p_ratio) || is_sym(h, sl->p_complex)) &&
            v->tag == F_STR);
}
static Form *lower_datum(SL *sl, Form *d);
static Form *datum_list(SL *sl, Form *f) {
    Span sp = f->span;
    uint32_t n = f->as.list.len;
    /* dotted: (a b . t) */
    if (n >= 3 && is_sym(f->as.list.items[n - 2], sl->s_dot)) {
        Form *tail = lower_datum(sl, f->as.list.items[n - 1]);
        for (int32_t i = (int32_t)n - 3; i >= 0; i--)
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_cons), lower_datum(sl, f->as.list.items[i]), tail);
        return tail;
    }
    FB b = {0};
    fb_push(&b, Sym(sl, sp, sl->p_list));
    for (uint32_t i = 0; i < n; i++) fb_push(&b, lower_datum(sl, f->as.list.items[i]));
    return fb_list(sl, &b, sp);
}
static Form *lower_datum(SL *sl, Form *d) {
    Span sp = d->span;
    switch (d->tag) {
        case F_SYM:
            /* r7rs-turmeric-syntax-leaks item 5: in user source `nil`,
             * `true` and `false` are identifiers, so quoted they are symbols
             * -- built by name, since `(quote nil)` would elaborate as
             * Turmeric's nil (the prelude's `'nil` is still the empty list). */
            if (!prelude_span(sp) &&
                (d->as.sym == sl->t_nil_sym || !strcmp(d->as.sym->name, "true") ||
                 !strcmp(d->as.sym->name, "false")))
                return Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-string->symbol")),
                          form_str(sl->a, sp, d->as.sym->name, d->as.sym->len));
            if (d->as.sym == sl->t_nil_sym) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
            /* A template's alias quoted (hyg_pending): the symbol as written. */
            if (global_alias_orig(sl, d->as.sym)) return form_quote(sl->a, sp, Sym(sl, sp, ident_orig(sl, d->as.sym)));
            return form_quote(sl->a, sp, d);
        case F_LIST:
            if (is_char_form(sl, d)) return d;
            if (d->as.list.len == 0) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
            return datum_list(sl, d);
        case F_VEC: {
            FB b = {0};
            fb_push(&b, Sym(sl, sp, sl->p_vector));
            for (uint32_t i = 0; i < d->as.list.len; i++) fb_push(&b, lower_datum(sl, d->as.list.items[i]));
            return fb_list(sl, &b, sp);
        }
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING: {
            /* ''x is (quote x) as data: a two-element list. */
            const char *head = d->tag == F_QUOTE ? "quote" : d->tag == F_QUASIQUOTE ? "quasiquote"
                             : d->tag == F_UNQUOTE ? "unquote" : "unquote-splicing";
            Form *h = form_quote(sl->a, sp, Sym(sl, sp, I(sl, head)));
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list), h, lower_datum(sl, d->as.list.items[0]));
        }
        case F_NIL: case F_BOOL:
            /* R10: the WORD `nil` / `true` / `false` quoted is a symbol (the
             * reader stamps them); `()` and `#t`/`#f` are not stamped.  Built
             * by name: a `(quote nil)` would elaborate as Turmeric's nil. */
            if (d->fx_prov == PROV_SCHEME_WORD && !prelude_span(sp)) {
                const char *w = d->tag == F_NIL ? "nil" : d->as.b ? "true" : "false";
                return Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-string->symbol")),
                          form_str(sl->a, sp, w, (uint32_t)strlen(w)));
            }
            if (d->tag == F_NIL) return Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
            return d;
        default: return d;   /* int, float, string, keyword */
    }
}

/* Quasiquote: the datum walker with holes.  At depth 1 an `unquote` is an
 * expression and an `unquote-splicing` element splices via `r7rs-append2__`;
 * a nested quasiquote raises the depth and its unquotes lower it, staying
 * data (R7RS 4.2.8). */
static Form *lower_qq(SL *sl, Form *f, int depth);
/* R10: `,x` and `(unquote x)` are the same datum (R7RS 4.2.8), as are the
 * other three abbreviations and their long forms.  The kind of `f` -- one of
 * F_QUOTE / F_QUASIQUOTE / F_UNQUOTE / F_UNQUOTE_SPLICING -- or F_LIST when
 * it is neither; `*arg` is the one operand. */
static FormTag qq_kind(SL *sl, Form *f, Form **arg) {
    switch (f->tag) {
        case F_QUOTE: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            *arg = f->as.list.items[0];
            return f->tag;
        case F_LIST:
            if (f->as.list.len == 2 && f->as.list.items[0]->tag == F_SYM) {
                const Symbol *h = f->as.list.items[0]->as.sym;
                *arg = f->as.list.items[1];
                if (h == sl->s_quasiquote) return F_QUASIQUOTE;
                if (h == sl->s_unquote) return F_UNQUOTE;
                if (h == sl->s_unquote_splicing) return F_UNQUOTE_SPLICING;
                if (h == I(sl, "quote")) return F_QUOTE;
            }
            return F_LIST;
        default:
            return f->tag;
    }
}
static Form *qq_list(SL *sl, Form *f, int depth) {
    Span sp = f->span;
    uint32_t n = f->as.list.len;
    Form *tail;
    int32_t last;
    if (n >= 3 && is_sym(f->as.list.items[n - 2], sl->s_dot)) {
        tail = lower_qq(sl, f->as.list.items[n - 1], depth);
        last = (int32_t)n - 3;
    } else {
        tail = Ln(sl, sp, 1, Sym(sl, sp, sl->p_list));
        last = (int32_t)n - 1;
    }
    for (int32_t i = last; i >= 0; i--) {
        Form *it = f->as.list.items[i], *arg = NULL;
        if (qq_kind(sl, it, &arg) == F_UNQUOTE_SPLICING && depth == 1)
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_append), lower(sl, arg), tail);
        else
            tail = Ln(sl, sp, 3, Sym(sl, sp, sl->p_cons), lower_qq(sl, it, depth), tail);
    }
    return tail;
}
static Form *lower_qq(SL *sl, Form *f, int depth) {
    Span sp = f->span;
    Form *arg = NULL;
    switch (qq_kind(sl, f, &arg)) {
        case F_UNQUOTE:
            if (depth == 1) return lower(sl, arg);
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote)),
                      lower_qq(sl, arg, depth - 1));
        case F_UNQUOTE_SPLICING:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_unquote_splicing)),
                      lower_qq(sl, arg, depth - 1));
        case F_QUASIQUOTE:
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, sl->s_quasiquote)),
                      lower_qq(sl, arg, depth + 1));
        case F_QUOTE:
            /* R10: `',x` is `(quote (unquote x))` -- a quote does not change
             * the quasiquote level, so the unquote inside it still fires. */
            return Ln(sl, sp, 3, Sym(sl, sp, sl->p_list),
                      form_quote(sl->a, sp, Sym(sl, sp, I(sl, "quote"))),
                      lower_qq(sl, arg, depth));
        case F_LIST:
            if (is_char_form(sl, f)) return f;
            return qq_list(sl, f, depth);
        case F_VEC: {
            Form *as_list = form_new(sl->a, F_LIST, sp);
            as_list->as.list = f->as.list;
            return Ln(sl, sp, 2, Sym(sl, sp, sl->p_list_to_vector), qq_list(sl, as_list, depth));
        }
        default: return lower_datum(sl, f);
    }
}

/* Lower every subform of a list-shaped form, keeping its tag. */
static Form *lower_children(SL *sl, Form *f) {
    Form **it = (Form **)arena_alloc(sl->a, (f->as.list.len + 1) * sizeof(Form *));
    bool changed = false;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        it[i] = lower(sl, f->as.list.items[i]);
        if (it[i] != f->as.list.items[i]) changed = true;
    }
    if (!changed) return f;
    Form *g = form_new(sl->a, f->tag, f->span);
    *g = *f;
    g->as.list.items = it;
    return g;
}

static bool scheme_std_visible(SL *sl, const Symbol *s);
/* Does an import at `sp` grant the unit stdlib names?  The user's own. */
static bool import_grants(const SL *sl, Span sp) {
    (void)sp;
    return sl->in_user;
}
/* A REPL session lowers each prompt turn on its own, so what an earlier
 * turn's `(import (turmeric stdlib/...))` granted is kept here, for the
 * process, and every later turn of a synthetic (`<eval>`) source starts from
 * it.  Kept as names, re-interned into each turn's table. */
typedef struct { char **items; uint32_t n, cap; } NameList;
static struct { NameList granted, denied, files; } g_repl_grants;
static void names_add(NameList *l, const char *s) {
    for (uint32_t i = 0; i < l->n; i++) if (strcmp(l->items[i], s) == 0) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->items = (char **)realloc(l->items, l->cap * sizeof(char *));
        if (!l->items) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    l->items[l->n] = strdup(s);
    if (!l->items[l->n]) { fprintf(stderr, "tur: oom\n"); abort(); }
    l->n++;
}
static bool span_is_synthetic(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f && f->path && f->path[0] == '<';
}
static bool callcc_escape_only(SL *sl, const Form *call);
static Form *lower(SL *sl, Form *f) {
    if (!f) return f;
    switch (f->tag) {
        case F_SYM: {
            /* R5: the prelude is exempt from the rename table as well as the
             * operator rewrite -- it is written against the typed stdlib by
             * its real names (`floor`, `sqrt`, `exp`, ...), which are exactly
             * the Scheme names the table maps onto the prelude's own
             * procedures; renamed, `(floor x)` inside r7rs-floor called
             * itself forever. */
            if (prelude_span(f->span)) return f;
            /* R10: a local variable (or a template's global alias) first --
             * `(let ((+ -)) (+ 3 1))` means the local `+`. */
            if (global_alias_orig(sl, f->as.sym) ||
                (sl->scope && scope_lookup(sl->scope, f->as.sym))) {
                const Symbol *r = rn(sl, f->as.sym);
                return (r == f->as.sym) ? f : Sym(sl, f->span, r);
            }
            int op = op_index(sl, f->as.sym);
            /* R10: an operator as a VALUE is the variadic prelude procedure,
             * as an `any` -- `((if #f + *) 3 4)` merged two fn types into a
             * C function pointer spelled from carrier kinds, which cc rejects
             * as incompatible pointer types (an error from GCC 14). */
            if (op >= 0)
                return Ln(sl, f->span, 3, Sym(sl, f->span, I(sl, "::")),
                          Sym(sl, f->span, sl->ops_val[op]), Sym(sl, f->span, sl->t_any));
            const Symbol *r = rn(sl, f->as.sym);
            if (sl->in_user && sl->deferred == 0 && r != f->as.sym) {
                const Symbol *early = std_before_redefinition(sl, f->as.sym, r);
                if (early) r = early;
            }
            /* r7rs-turmeric-syntax-leaks item 8: a Turmeric stdlib name the
             * unit did not import is not bound in Scheme. */
            if (r == f->as.sym && sl->in_user && !scheme_std_visible(sl, r)) {
                const Symbol *file = std_file_of(sl, r);
                if (file && sym_list_has(sl->granted_files, sl->n_granted_files, file))
                    err(f, "'%s' is not bound: this unit's import of (turmeric stdlib/%s) leaves it out "
                           "(an `except`, or a `rename` gave it another name)", r->name, file->name);
                else if (file)
                    err(f, "'%s' is not bound: it is Turmeric's (stdlib/%s.tur), which a Scheme program "
                           "reaches only through an import -- add (import (turmeric stdlib/%s))",
                        r->name, file->name, file->name);
                else
                    err(f, "'%s' is not bound: it is a Turmeric built-in, which Scheme cannot name; "
                           "use the Scheme procedure (display, write, ...), or call it from a Turmeric "
                           "module imported with (turmeric <module>)", r->name);
                return Ln(sl, f->span, 3, Sym(sl, f->span, I(sl, "::")), Nil(sl, f->span),
                          Sym(sl, f->span, sl->t_any));
            }
            return (r == f->as.sym) ? f : Sym(sl, f->span, r);
        }
        case F_QUOTE:      return lower_datum(sl, f->as.list.items[0]);
        case F_QUASIQUOTE: return lower_qq(sl, f->as.list.items[0], 1);
        case F_LIST: {
            /* R10: `(quasiquote x)` written out is the same as `` `x ``. */
            Form *arg = NULL;
            if (!prelude_span(f->span) && qq_kind(sl, f, &arg) == F_QUASIQUOTE &&
                !sr_lookup(sl, sl->s_quasiquote))
                return lower_qq(sl, arg, 1);
            break;
        }
        case F_VEC:
            /* R7: a vector is self-evaluating (R7RS 4.1.2): `#(a b c)` is
             * the constant `'#(a b c)`, its elements DATA, not expressions to
             * evaluate (R10: chibi's suite writes `(test #(a b c) ...)`).  So
             * it is built exactly as a quoted one -- through `vector`, always
             * a `(Vec any)`.  Only a vector the reader read from `#(`
             * (PROV_SCHEME_VECTOR): a Turmeric-shaped `(defn f [x] ...)` in a
             * Scheme file keeps its binding vector. */
            if (!prelude_span(f->span) && f->fx_prov == PROV_SCHEME_VECTOR)
                return lower_datum(sl, f);
            return lower_children(sl, f);
        case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL:
        case F_UNQUOTE: case F_UNQUOTE_SPLICING:
            return lower_children(sl, f);
        default: return f;
    }
    if (f->as.list.len == 0) return Ln(sl, f->span, 1, Sym(sl, f->span, sl->p_list));
    Form *head = f->as.list.items[0];
    if (head->tag == F_SYM && !prelude_span(f->span)) {
        /* R10 (hygiene): a head that is a LOCAL variable is a call, whatever
         * its name -- `(let ((if even?)) (if 7))` calls even?.  A template's
         * alias of a keyword or global is that keyword or global, however
         * the use site binds the name. */
        if (sl->scope && scope_lookup(sl->scope, head->as.sym))
            return lower_children(sl, f);
    }
    /* The name the head DISPATCHES on: an alias's original.  The form keeps
     * the alias, so a global procedure it names is still called globally. */
    const Symbol *hsym = head->tag == F_SYM ? head->as.sym : NULL;
    if (hsym && global_alias_orig(sl, hsym)) hsym = global_alias_orig(sl, hsym);
    if (hsym && sr_lookup(sl, hsym)) {
        /* R4: a macro use.  Expand until the head is not a macro, then lower
         * the expansion; the depth guard catches an expansion that grows a
         * macro use inside itself forever. */
        Form *x = sr_expand_head(sl, f);
        if (!x) return Nil(sl, f->span);
        if (sl->expand_depth >= SR_MAX_DEPTH) {
            err(f, "macro expansion nested more than %d deep -- does '%s' expand to itself?",
                SR_MAX_DEPTH, head->as.sym->name);
            return Nil(sl, f->span);
        }
        sl->expand_depth++;
        Form *r = lower(sl, x);
        sl->expand_depth--;
        return r;
    }
    /* `((lambda (x ...) body...) a ...)` is `(let ((x a) ...) body...)`, which
     * is how R7RS defines `let` -- and lowered as a call of a closure value it
     * was never a C tail call, so a loop written that way grew the stack.
     * Only a fixed formals list whose length matches; `lambda` must be the
     * keyword, not a local. */
    if (head->tag == F_LIST && head->as.list.len >= 3 && !prelude_span(f->span) &&
        head->as.list.items[0]->tag == F_SYM &&
        !(sl->scope && scope_lookup(sl->scope, head->as.list.items[0]->as.sym)) &&
        (global_alias_orig(sl, head->as.list.items[0]->as.sym)
             ? global_alias_orig(sl, head->as.list.items[0]->as.sym)
             : head->as.list.items[0]->as.sym) == sl->s_lambda) {
        const Form *formals = head->as.list.items[1];
        uint32_t nf = formals->tag == F_LIST ? formals->as.list.len : 0;
        bool ok = (formals->tag == F_NIL || formals->tag == F_LIST) && nf == f->as.list.len - 1;
        for (uint32_t i = 0; ok && i < nf; i++)
            ok = formals->as.list.items[i]->tag == F_SYM &&
                 !is_sym(formals->as.list.items[i], sl->s_dot);   /* (x . rest) */
        if (ok) {
            FB binds = {0};
            for (uint32_t i = 0; i < nf; i++)
                fb_push(&binds, Ln(sl, f->span, 2, formals->as.list.items[i], f->as.list.items[i + 1]));
            FB let = {0};
            fb_push(&let, Sym(sl, f->span, sl->s_let));
            fb_push(&let, nf ? fb_list(sl, &binds, f->span) : Nil(sl, f->span));
            for (uint32_t i = 2; i < head->as.list.len; i++) fb_push(&let, head->as.list.items[i]);
            Form *lf = fb_list(sl, &let, f->span);
            Form *r = lower_let(sl, lf);
            if (r) return r;
        }
    }
    if (head->tag == F_SYM) {
        const Symbol *h = hsym;
        Form *r = NULL;
        int op = op_index(sl, h);
        if (op >= 0 && !prelude_span(f->span)) return lower_operator(sl, f, op);
        if (h == sl->s_quote && f->as.list.len == 2) return lower_datum(sl, f->as.list.items[1]);
        if (h == sl->s_quasiquote && f->as.list.len == 2) return lower_qq(sl, f->as.list.items[1], 1);
        if (h == sl->s_syntax_error) {
            const char *msg = (f->as.list.len >= 2 && f->as.list.items[1]->tag == F_STR)
                ? f->as.list.items[1]->as.s.p : "syntax-error";
            err(f, "%s%s", msg, f->as.list.len > 2 ? " (see the forms that follow the message)" : "");
            return Nil(sl, f->span);
        }
        if (h == sl->s_let_syntax || h == sl->s_letrec_syntax) return lower_let_syntax(sl, f);
        if (h == sl->s_lambda)      return lower_lambda(sl, f);
        if (h == sl->s_let)         { r = lower_let(sl, f);      if (r) return r; }
        if (h == sl->s_letstar)     { r = lower_letstar(sl, f);  if (r) return r; }
        if (h == sl->s_letrec || h == sl->s_letrecstar) { r = lower_letrec(sl, f); if (r) return r; }
        if (h == sl->s_do && looks_like_scheme_do(f)) return lower_do(sl, f);
        if (h == sl->s_begin)       return lower_seq(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_if)          return lower_if(sl, f);
        if (h == sl->s_cond && looks_like_scheme_cond(f))
            return cond_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_set && !prelude_span(f->span) && is_srfi17_place(f)) {
            if (sl->srfi17_set) return srfi17_place(sl, f);
            err(f->as.list.items[1], "assigning to (procedure arg ...) is SRFI 17's generalized "
                                     "set!; import (srfi 17) to use it");
            return Nil(sl, f->span);
        }
        if (h == sl->s_case && looks_like_scheme_case(sl, f)) return lower_case(sl, f);
        if (h == sl->s_and)         return and_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_or)          return or_chain(sl, f->as.list.items + 1, f->as.list.len - 1, f->span);
        if (h == sl->s_when)        return lower_when_unless(sl, f, false);
        if (h == sl->s_unless)      return lower_when_unless(sl, f, true);
        if (h == sl->s_case_lambda) return lower_case_lambda(sl, f);
        if (h == sl->s_let_values)  return lower_let_values(sl, f, false);
        if (h == sl->s_letstar_values) return lower_let_values(sl, f, true);
        if (!prelude_span(f->span)) {
            /* R7: named refusals rather than an unbound-name error. */
            const char *hn = h->name;
            (void)hn;
            bool fold;
            if (is_include_head(sl, f, &fold)) {
                /* In expression position the included forms are a `begin`. */
                FB got = {0};
                Form *r = Nil(sl, f->span);
                if (include_files(sl, f, fold, &got) && got.n > 0) r = lower_seq(sl, got.items, got.n, f->span);
                free(got.items);
                return r;
            }
        }
        if (h == sl->s_guard)        return lower_guard(sl, f);
        if (h == sl->s_parameterize) return lower_parameterize(sl, f);
        if (h == sl->s_delay)        return lower_delay(sl, f, false);
        if (h == sl->s_delay_force)  return lower_delay(sl, f, true);
        if (h == sl->s_define) {
            err(f, "define is not allowed in expression position; a body's "
                   "defines come first (R7RS 5.3.2)");
            return Nil(sl, f->span);
        }
        if (h == sl->s_define_syntax) {
            err(f, "define-syntax is only allowed at the top level or at the beginning of a body");
            return Nil(sl, f->span);
        }
        if (h == sl->s_define_record_type) {
            err(f, "define-record-type is only allowed at the top level or at the beginning of a body (R7RS 5.3.2)");
            return Nil(sl, f->span);
        }
        if (h == sl->s_cond_expand) {
            uint32_t n; Form **items;
            if (!cond_expand_clause(sl, f, &n, &items)) return Nil(sl, f->span);
            return lower_seq(sl, items, n, f->span);
        }
        /* r7rs-turmeric-syntax-leaks item 7: a Turmeric special form (`defn`,
         * `fn`, `match`, `::`, ...) is not Scheme.  In a user Scheme source
         * its name is an ordinary identifier; one the program never binds
         * (a binder of that name is in the clash table, so rn moves it) heads
         * a form written as Turmeric.  Refuse it and say where Turmeric code
         * goes: a Turmeric module imported with (turmeric ...), or, at the
         * REPL, the prompt switched with `#lang turmeric`.  The
         * Turmeric-shaped sources (the prelude, stdlib/r7rs/) are exempt. */
        if (!prelude_span(f->span) && rn(sl, h) == h && !global_alias_orig(sl, head->as.sym) &&
            tur_name_is_reserved_special_form(h->name) && !is_scheme_syntax_name(h->name)) {
            const SourceFile *sf = diag_source_file(f->span.file_id);
            if (sf && sf->path && sf->path[0] == '<')
                err(head, "'%s' is Turmeric syntax, not Scheme; to write Turmeric at this prompt, "
                          "switch it with #lang turmeric", h->name);
            else
                err(head, "'%s' is Turmeric syntax, not Scheme; write Turmeric code in a Turmeric "
                          "module and import it with (turmeric <module>)", h->name);
            /* An `any`, so a refused form in call-head position does not
             * cascade into "nil is not callable". */
            return Ln(sl, f->span, 3, Sym(sl, f->span, I(sl, "::")), Nil(sl, f->span), Sym(sl, f->span, sl->t_any));
        }
        /* r7rs-callcc-memory-never-freed: a `call/cc` whose continuation
         * provably never outlives the call is the one-shot escape, which
         * copies no stack and pins nothing (callcc_escape_only says when). */
        if (!prelude_span(f->span) && callcc_escape_only(sl, f))
            return Ln(sl, f->span, 2, Sym(sl, f->span, I(sl, "r7rs-call/ec-proc__")),
                      lower(sl, f->as.list.items[1]));
    }
    return lower_children(sl, f);
}

/* One top-level Scheme form -> zero or more Turmeric top-level forms. */
/* R7RS 4.1.7 `include` / `include-ci`, and 5.6.1's `(include ...)` library
 * declaration: each named file is read as Scheme -- the Scheme reader, with
 * or without a `#lang` line of its own (one is stripped) -- relative to the
 * including file's directory, and its forms are spliced where the include
 * stood.  The file is registered with the diagnostic registry, so an error
 * inside it names it.  `include-ci` reads with `#!fold-case` in force. */
static bool is_include_head(const SL *sl, const Form *f, bool *fold) {
    if (f->tag != F_LIST || f->as.list.len < 1 || f->as.list.items[0]->tag != F_SYM) return false;
    const Symbol *h = f->as.list.items[0]->as.sym;
    if (h == sl->s_include) { *fold = false; return true; }
    if (strcmp(h->name, "include-ci") == 0) { *fold = true; return true; }
    return false;
}
static bool include_files(SL *sl, Form *f, bool fold, FB *out) {
    const char *what = fold ? "include-ci" : "include";
    if (f->as.list.len < 2) { err(f, "%s expects one or more file names: (%s \"file\" ...)", what, what); return false; }
    const SourceFile *from = diag_source_file(f->span.file_id);
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        Form *pf = f->as.list.items[i];
        if (pf->tag != F_STR) { err(pf, "%s takes string file names", what); return false; }
        char path[4096];
        size_t dlen = 0;
        const char *dir = NULL;
        if (from && from->base_dir) { dir = from->base_dir; dlen = strlen(dir); }
        else if (from && from->path) {
            const char *slash = strrchr(from->path, '/');
            if (slash) { dir = from->path; dlen = (size_t)(slash - from->path); }
        }
        bool absolute = pf->as.s.len > 0 && pf->as.s.p[0] == '/';
        int pn;
        if (dir && dlen && !absolute)
            pn = snprintf(path, sizeof path, "%.*s/%.*s", (int)dlen, dir, (int)pf->as.s.len, pf->as.s.p);
        else
            pn = snprintf(path, sizeof path, "%.*s", (int)pf->as.s.len, pf->as.s.p);
        if (pn <= 0 || (size_t)pn >= sizeof path) { err(pf, "%s: file name too long", what); return false; }
        FILE *fp = fopen(path, "rb");
        if (!fp) { err(pf, "%s: cannot open '%s'", what, path); return false; }
        size_t cap = 4096, len = 0;
        char *raw = (char *)malloc(cap);
        if (!raw) { fclose(fp); fprintf(stderr, "tur: oom\n"); abort(); }
        size_t got;
        while ((got = fread(raw + len, 1, cap - len - 1, fp)) > 0) {
            len += got;
            if (cap - len < 2) {
                cap *= 2;
                raw = (char *)realloc(raw, cap);
                if (!raw) { fclose(fp); fprintf(stderr, "tur: oom\n"); abort(); }
            }
        }
        fclose(fp);
        raw[len] = '\0';
        /* An included file is Scheme by definition; a `#lang` line of its own
         * is stripped, and the dialect it names is not consulted. */
        const char *body = raw; size_t blen = len;
        const char *bad = NULL; size_t bad_len = 0;
        LangDialect dialect = LANG_TURMERIC;
        (void)detect_lang_dialect(raw, len, &body, &blen, &bad, &bad_len, &dialect);
        if (bad) { err(pf, "%s: bad #lang line in '%s' ('%.*s')", what, path, (int)bad_len, bad); free(raw); return false; }
        const char *prefix = fold ? "#!fold-case " : "";
        size_t plen = strlen(prefix);
        char *src = (char *)arena_alloc(sl->a, plen + blen + 1);
        memcpy(src, prefix, plen);
        memcpy(src + plen, body, blen);
        src[plen + blen] = '\0';
        free(raw);
        char *path_copy = (char *)arena_alloc(sl->a, (size_t)pn + 1);
        memcpy(path_copy, path, (size_t)pn + 1);
        SourceFile *sf = (SourceFile *)arena_alloc(sl->a, sizeof(SourceFile));
        *sf = (SourceFile){0};
        sf->path        = path_copy;
        sf->src         = src;
        sf->len         = plen + blen;
        sf->file_id     = diag_alloc_file_id();
        sf->reader_type = READER_R7RS;
        sf->lang        = LANG_R7RS;
        diag_register_file(sf);
        uint32_t nf = 0;
        Form **fs = read_all_with_registry(sl->a, sl->st, sf, NULL, &nf);
        if (!fs) return false;
        for (uint32_t k = 0; k < nf; k++) {
            bool sub_fold;
            if (is_include_head(sl, fs[k], &sub_fold)) { if (!include_files(sl, fs[k], sub_fold, out)) return false; }
            else fb_push(out, fs[k]);
        }
    }
    return true;
}
/* The pre-pass: splice top-level includes, and a library's `(include ...)`
 * declarations as `(begin ...)`, BEFORE the scans that read the whole
 * program (the `set!` targets, the clash table), so an included definition
 * is seen by them like one written in place. */
static Form *expand_library_includes(SL *sl, Form *f) {
    bool any = false, fold;
    for (uint32_t i = 2; i < f->as.list.len && !any; i++) any = is_include_head(sl, f->as.list.items[i], &fold);
    if (!any) return f;
    FB decls = {0};
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        Form *d = f->as.list.items[i];
        if (i >= 2 && is_include_head(sl, d, &fold)) {
            FB body = {0};
            fb_push(&body, Sym(sl, d->span, sl->s_begin));
            if (include_files(sl, d, fold, &body)) fb_push(&decls, fb_list(sl, &body, d->span));
            else free(body.items);
        } else fb_push(&decls, d);
    }
    return fb_list(sl, &decls, f->span);
}
static void expand_includes(SL *sl, Form *const *forms, uint32_t n, FB *out) {
    for (uint32_t i = 0; i < n; i++) {
        Form *f = forms[i];
        bool fold;
        if (is_scheme_file(f) && !prelude_span(f->span)) {
            if (is_include_head(sl, f, &fold)) {
                FB got = {0};
                if (include_files(sl, f, fold, &got)) expand_includes(sl, got.items, got.n, out);
                free(got.items);
                continue;
            }
            if (head_is(f, sl->s_define_library)) f = expand_library_includes(sl, f);
        }
        fb_push(out, f);
    }
}

/* r7rs-toplevel-reentry-reruns-forms: one top-level STATEMENT of the program,
 * lowered under its own prompt -- `(r7rs-toplevel__ (lambda () <stmt>))`,
 * lowered as the Scheme form it is.  A continuation captured inside the
 * statement then copies the stack only up to the runner's frame, so
 * re-entering it from a later form finishes this statement and carries on
 * after the invoking form, the way chibi and Racket delimit each top-level
 * form.  The prompt is per program statement: a library body's forms and a
 * REPL line (whose value the prompt echoes) are lowered bare. */
static Form *lower_toplevel_stmt(SL *sl, Form *f) {
    Span sp = f->span;
    Form *thunk = Ln(sl, sp, 3, Sym(sl, sp, sl->s_lambda), Ln(sl, sp, 0), f);
    Form *call = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-toplevel__")), thunk);
    sl->eager_thunk = sl->deferred == 0;
    Form *r = lower(sl, call);
    sl->eager_thunk = false;
    return r;
}

static void lower_toplevel_1(SL *sl, Form *f, FB *out);
static void lower_toplevel(SL *sl, Form *f, FB *out) {
    FB *saved = sl->lift_out;
    sl->lift_out = out;
    lower_toplevel_1(sl, f, out);
    sl->lift_out = saved;
}
static void lower_toplevel_1(SL *sl, Form *f, FB *out) {
    Span sp = f->span;
    {
        bool fold;
        if (is_include_head(sl, f, &fold) && !prelude_span(sp)) {
            FB got = {0};
            if (include_files(sl, f, fold, &got))
                for (uint32_t i = 0; i < got.n; i++) lower_toplevel(sl, got.items[i], out);
            free(got.items);
            return;
        }
    }
    if (head_is(f, sl->s_define_syntax)) { sr_define(sl, f); return; }
    if (f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
        sr_lookup(sl, f->as.list.items[0]->as.sym)) {
        Form *x = sr_expand_head(sl, f);
        if (!x) return;
        lower_toplevel(sl, x, out);
        return;
    }
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) lower_toplevel(sl, f->as.list.items[i], out);
        return;
    }
    if (head_is(f, sl->s_define_values)) {
        FB defs = {0};
        expand_define_values(sl, f, &defs);
        for (uint32_t i = 0; i < defs.n; i++) lower_toplevel(sl, defs.items[i], out);
        free(defs.items);
        return;
    }
    if (head_is(f, sl->s_define)) {
        if (f->as.list.len < 2) { err(f, "define expects (define name expr) or (define (name . formals) body...)"); return; }
        Form *target = f->as.list.items[1];
        if (target->tag == F_LIST) {
            if (target->as.list.len < 1 || target->as.list.items[0]->tag != F_SYM) {
                err(target, "define target must be an identifier or (name . formals); "
                            "a curried define is not R7RS");
                return;
            }
            const Symbol *name = rn(sl, target->as.list.items[0]->as.sym);
            Form *formals = List(sl, target->span, target->as.list.items + 1, target->as.list.len - 1);
            bool ok;
            const Symbol *rest = NULL;
            LFrame *saved = scope_open(sl);     /* R10: the parameters' scope */
            Form *params = lower_formals(sl, formals, &ok, &rest);
            if (!ok || f->as.list.len < 3) {
                if (ok) err(f, "define needs a body");
                scope_close(sl, saved);
                return;
            }
            sl->deferred++;
            Form *body = rebind_rest(sl, sp, rest,
                                     lower_body(sl, f->as.list.items + 2, f->as.list.len - 2, sp));
            sl->deferred--;
            /* A parameter the body `set!`s is a mutable local, as in a
             * lambda (r7rs-toplevel-define-sets-its-parameter). */
            body = rebind_muts(sl, sp, params, body);
            scope_close(sl, saved);
            if (name == sl->s_main && params->as.list.len == 0) {
                /* `(define (main) ...)` is the program's entry: Turmeric's main
                 * returns int, so run the body for effect and answer 0. */
                sl->user_main = true;
                Form *ann = form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_int));
                fb_push(out, Ln(sl, sp, 6, Sym(sl, sp, sl->t_defn), Sym(sl, sp, name),
                                params, ann, body, Int(sl, sp, 0)));
                return;
            }
            fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, name), params, body));
            return;
        }
        if (target->tag != F_SYM || f->as.list.len != 3) {
            err(f, "define expects (define name expr)");
            return;
        }
        const Symbol *name = rn(sl, target->as.sym);
        /* R6: `(define f (lambda formals body...))` of a name that is never
         * `set!` is `(define (f . formals) body...)` -- a defn, so it is a
         * known global with its variadic signature (a `def` of a lambda value
         * drops the rest marker from the binding's type, and `(apply f xs)`
         * then cannot pack for it) and a forward reference works. */
        if (!is_mut(sl, name) && f->as.list.items[2]->tag == F_LIST &&
            f->as.list.items[2]->as.list.len >= 3 &&
            is_sym(f->as.list.items[2]->as.list.items[0], sl->s_lambda) &&
            !sr_lookup(sl, sl->s_lambda)) {
            Form *lam = f->as.list.items[2];
            Form *formals = nil_to_list(sl, lam->as.list.items[1]);
            FB tb = {0};
            fb_push(&tb, target);
            if (formals->tag == F_SYM) {
                fb_push(&tb, Sym(sl, formals->span, sl->s_dot));
                fb_push(&tb, formals);
            } else if (formals->tag == F_LIST) {
                for (uint32_t i = 0; i < formals->as.list.len; i++) fb_push(&tb, formals->as.list.items[i]);
            } else {
                err(formals, "lambda formals must be a list of identifiers, a single "
                             "identifier, or `(a b . rest)`");
                free(tb.items);
                return;
            }
            FB db = {0};
            fb_push(&db, f->as.list.items[0]);
            fb_push(&db, fb_list(sl, &tb, target->span));
            for (uint32_t i = 2; i < lam->as.list.len; i++) fb_push(&db, lam->as.list.items[i]);
            lower_toplevel(sl, fb_list(sl, &db, sp), out);
            return;
        }
        /* r7rs-case-lambda-define-cannot-recur: `(define f (case-lambda ...))`
         * of a name never `set!` is a defn of the dispatching fn, as the
         * lambda case above is -- a def's initializer cannot see the global it
         * defines, so a clause calling f (how a case-lambda defaults an
         * argument: `((x) (f x 1))`) was "unknown function f". */
        if (!is_mut(sl, name) && f->as.list.items[2]->tag == F_LIST &&
            f->as.list.items[2]->as.list.len >= 1 &&
            is_sym(f->as.list.items[2]->as.list.items[0], sl->s_case_lambda) &&
            !sr_lookup(sl, sl->s_case_lambda)) {
            Form *fnf = lower_case_lambda(sl, f->as.list.items[2]);
            if (head_is(fnf, sl->t_fn) && fnf->as.list.len == 3) {
                fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, name),
                                fnf->as.list.items[1], fnf->as.list.items[2]));
                return;
            }
        }
        Form *init = lower(sl, f->as.list.items[2]);
        /* R10: `(define first car)` -- a global that IS another procedure.
         * Left bare, it became a thin C function pointer whose declared
         * parameters were the carrier `int64_t` while the aliased procedure
         * takes a typed struct (cc: incompatible-pointer / int-conversion, an
         * error from GCC 14).  As an `any` it is called through the dynamic
         * path, like every other Scheme procedure value. */
        if (init->tag == F_SYM && !is_mut(sl, name))
            init = Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), init, Sym(sl, sp, sl->t_any));
        /* toplevel-def-initializers-run-before-toplevel-expressions: which
         * initializers a define after the first expression defers.  A lambda,
         * a case-lambda, a quotation or a literal does no work and needs no
         * ordering -- and a deferred procedure value would be a `^mut` fn
         * global, a fat cell, which a variadic case-lambda's thunk type does
         * not survive (cc: int-conversion on the rest chain).  A symbol IS
         * deferred: `(define x y)` after a deferred `y` must read it after
         * its assignment. */
        bool defer_init = false;
        /* Only the program's own defines: an SRFI's (or the prelude's),
         * lowered in place beside it, are a library's, initialized in the
         * library's order before the program runs -- and a deferred one is a
         * `set!` in the program's main, which keeps it and all it names alive
         * past r7rs-srfi-plan S3's pruning. */
        if (sl->toplevel_expr_seen && sl->in_user && out != &sl->lib_body) {
            const Form *raw = f->as.list.items[2];
            if (raw->tag == F_SYM) defer_init = true;
            else if (raw->tag == F_LIST && raw->as.list.len > 0) {
                const Form *h = raw->as.list.items[0];
                defer_init = !(h->tag == F_SYM
                               && (h->as.sym == sl->s_lambda || is_sym(h, sl->s_case_lambda)
                                   || is_sym(h, sl->s_quote) || is_sym(h, sl->s_quasiquote)));
            }
        }
        if (defer_init) {
            /* toplevel-def-initializers-run-before-toplevel-expressions: a
             * define AFTER the program's first top-level expression.  Its
             * initializer must run in source order (R7RS 5.1), but a
             * module-level `def`'s initializers all run before the program
             * body (`__tur_module_def_init`).  So the def is `^deferred-init`:
             * it declares the global with the initializer's own static type
             * (a seam value keeps its `(Vec A)`, which an `any` round trip
             * cannot ground back -- saffron-open-generic-result-not-grounded),
             * and the initializer runs where the define stood, as the
             * statement `(set! name (__tur-deferred-init__ name))` -- in
             * order with its neighbours, under its own prompt like every
             * other statement, on both back ends.  A define before any
             * expression keeps the plain `def`: those already run in source
             * order among themselves. */
            FB d = {0};
            fb_push(&d, Sym(sl, sp, sl->t_def));
            fb_push(&d, Sym(sl, sp, sl->t_mut));
            fb_push(&d, Sym(sl, sp, I(sl, "^deferred-init")));
            fb_push(&d, Sym(sl, sp, name));
            if (is_mut(sl, name) || (sl->repl_turn && sl->in_user)) fb_push(&d, AnyAnn(sl, sp));
            fb_push(&d, init);
            fb_push(out, fb_list(sl, &d, sp));
            fb_push(out, lower_toplevel_stmt(sl,
                Ln(sl, sp, 3, Sym(sl, sp, sl->s_set), Sym(sl, sp, name),
                   Ln(sl, sp, 2, Sym(sl, sp, I(sl, "__tur-deferred-init__")), Sym(sl, sp, name)))));
            return;
        }
        /* r7rs-repl-forgets-macros-and-set: at the REPL (and in `eval`) a
         * later turn may `set!` any variable -- `(define n 0)`, then `(set! n
         * (+ n 1))` -- so a REPL turn's top-level variable is always the
         * mutable `any` cell a `set!` target is.  A procedure definition
         * stays a defn: a later turn redefines it with `define`. */
        if (is_mut(sl, name) || (sl->repl_turn && sl->in_user)) {
            fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_def), Sym(sl, sp, sl->t_mut),
                            Sym(sl, sp, name), AnyAnn(sl, sp), init));
        } else {
            fb_push(out, Ln(sl, sp, 3, Sym(sl, sp, sl->t_def), Sym(sl, sp, name), init));
        }
        return;
    }
    if (head_is(f, sl->s_import) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST) {
        /* R3 / D9: each import set maps onto a Turmeric import (or a no-op
         * when the names are already global); the program is then wrapped
         * in a defmodule, where Turmeric's import is legal. */
        for (uint32_t i = 1; i < f->as.list.len; i++) lower_import_set(sl, f->as.list.items[i]);
        return;
    }
    if (head_is(f, sl->s_define_library)) {
        /* (define-library (my utils) (export ...) (import ...) (begin ...))
         * -> (defmodule my/utils (export ...) imports... body...) */
        if (sl->has_library) { err(f, "only one define-library per file (Turmeric: one defmodule per file)"); return; }
        if (f->as.list.len < 2) { err(f, "define-library needs a library name"); return; }
        bool ok;
        const Symbol *name = library_module(sl, f->as.list.items[1], &ok);
        if (!ok) return;
        if (!name) { err(f->as.list.items[1], "a (scheme ...), (srfi ...) or auto-loaded stdlib name cannot be defined here"); return; }
        sl->has_library = true;
        sl->lib_name = name;
        /* r7rs-define-library-cannot-export-syntax: an exported macro is not a
         * module export -- the module has no definition of it; an importer
         * reads it from this file's source (lib_syntax_of).  What the macros'
         * forms name of the library's own definitions is exported under a
         * hidden spelling, so an importer's expansion can reach it. */
        {
            LibScan ls = {0};
            lib_scan(sl, f, &ls);
            if (ls.exp_pub.n) f = lib_rewrite_exports(sl, f, name, &ls);
            lib_scan_free(&ls);
        }
        bool lib_fold = false;
        /* R7RS 5.6.1 `(export (rename internal public))`: importers see the
         * definition as `public`.  A module's `:exports` are bare names, so
         * the public spelling has to be a real definition of the module.
         * When the library defines `internal` and exports it only this way,
         * the definition itself is spelled `public` -- through the clash
         * table, so every use in the library and every `set!` scan follows
         * -- and keeps its static signature.  Otherwise (an imported name,
         * or one also exported under another name) `public` is an `any`
         * alias of it, defined at the end of the body.  A library global
         * that is itself named `public` is respelled out of the way
         * (r7rs-library-file-shape-and-export-rename). */
        typedef struct { const Symbol *in, *pub, *out; bool alias; } ExportRename;
        ExportRename *ren = NULL;
        uint32_t n_ren = 0;
        {
            uint32_t cap = 0;
            for (uint32_t i = 2; i < f->as.list.len; i++)
                if (head_is(f->as.list.items[i], sl->s_export))
                    for (uint32_t j = 1; j < f->as.list.items[i]->as.list.len; j++)
                        if (is_export_rename(sl, f->as.list.items[i]->as.list.items[j])) cap++;
            ren = (ExportRename *)arena_alloc(sl->a, (cap ? cap : 1) * sizeof *ren);
            FB plain = {0}, defined = {0};
            for (uint32_t i = 2; i < f->as.list.len; i++) {
                Form *decl = f->as.list.items[i];
                if (!head_is(decl, sl->s_export)) continue;
                for (uint32_t j = 1; j < decl->as.list.len; j++) {
                    Form *nm = decl->as.list.items[j];
                    const Symbol *pub = NULL;
                    if (is_export_rename(sl, nm)) {
                        if (nm->as.list.items[1]->tag != F_SYM || nm->as.list.items[2]->tag != F_SYM) {
                            err(nm, "(export (rename internal public)) takes two identifiers");
                            continue;
                        }
                        pub = nm->as.list.items[2]->as.sym;
                        ren[n_ren].in = nm->as.list.items[1]->as.sym;
                        ren[n_ren].pub = pub;
                        ren[n_ren].out = clash_spelling(sl, pub);
                        /* r7rs-library-defines-standard-or-stdlib-name: a
                         * public name that is a standard or stdlib one is
                         * `<name>--user` in the module, as importers expect. */
                        if (ren[n_ren].out == pub && lib_needs_respelling(sl, pub))
                            ren[n_ren].out = lib_user_spelling(sl, pub);
                        ren[n_ren].alias = true;
                        n_ren++;
                    } else if (nm->tag == F_SYM) {
                        pub = nm->as.sym;
                        fb_push(&plain, nm);
                    } else continue;
                    /* R7RS 5.6.1: an identifier may be exported once. */
                    bool twice = false;
                    for (uint32_t k = 0; k < plain.n && !twice; k++)
                        twice = plain.items[k]->as.sym == pub && plain.items[k] != nm;
                    for (uint32_t k = 0; k + (is_export_rename(sl, nm) ? 1 : 0) < n_ren && !twice; k++)
                        twice = ren[k].pub == pub;
                    if (twice) err(nm, "'%s' is exported twice", pub->name);
                }
            }
            library_defined_names(sl, f, &defined);
            for (uint32_t k = 0; k < n_ren; k++) {
                const Symbol *in = ren[k].in;
                bool defd = false;
                for (uint32_t d = 0; d < defined.n && !defd; d++) defd = defined.items[d]->as.sym == in;
                uint32_t uses = 0;
                for (uint32_t p = 0; p < plain.n; p++) uses += plain.items[p]->as.sym == in;
                for (uint32_t r = 0; r < n_ren; r++) uses += ren[r].in == in;
                if (defd && uses == 1) { ren[k].alias = false; set_clash(sl, in, ren[k].out); }
            }
            for (uint32_t k = 0; k < n_ren; k++) {
                const Symbol *pub = ren[k].pub;
                bool defd = false, respelled = false;
                for (uint32_t d = 0; d < defined.n && !defd; d++) defd = defined.items[d]->as.sym == pub;
                for (uint32_t r = 0; r < n_ren && !respelled; r++) respelled = !ren[r].alias && ren[r].in == pub;
                if (defd && !respelled) {
                    char pre[160];
                    snprintf(pre, sizeof pre, "%s--lib", pub->name);
                    set_clash(sl, pub, fresh(sl, pre));
                }
            }
            free(plain.items); free(defined.items);
        }
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            Form *decl = f->as.list.items[i];
            if (head_is(decl, sl->s_export)) {
                for (uint32_t j = 1; j < decl->as.list.len; j++) {
                    Form *nm = decl->as.list.items[j];
                    if (is_export_rename(sl, nm)) {
                        for (uint32_t k = 0; k < n_ren; k++)
                            if (ren[k].in == nm->as.list.items[1]->as.sym && ren[k].pub == nm->as.list.items[2]->as.sym) {
                                fb_push(&sl->lib_exports, Sym(sl, nm->span, ren[k].out));
                                break;
                            }
                        continue;
                    }
                    if (nm->tag != F_SYM) { err(nm, "export names must be identifiers"); continue; }
                    fb_push(&sl->lib_exports, Sym(sl, nm->span, rn(sl, nm->as.sym)));
                }
            } else if (head_is(decl, sl->s_import)) {
                for (uint32_t j = 1; j < decl->as.list.len; j++) lower_import_set(sl, decl->as.list.items[j]);
            } else if (head_is(decl, sl->s_begin)) {
                note_forward_defs(sl, decl->as.list.items + 1, decl->as.list.len - 1);
                for (uint32_t j = 1; j < decl->as.list.len; j++) lower_toplevel(sl, decl->as.list.items[j], &sl->lib_body);
            } else if (head_is(decl, sl->s_cond_expand)) {
                uint32_t n; Form **items;
                if (cond_expand_clause(sl, decl, &n, &items))
                    for (uint32_t j = 0; j < n; j++) {
                        /* a chosen clause holds declarations, not forms */
                        Form *d = items[j];
                        if (head_is(d, sl->s_begin))
                            for (uint32_t k = 1; k < d->as.list.len; k++) lower_toplevel(sl, d->as.list.items[k], &sl->lib_body);
                        else if (head_is(d, sl->s_import))
                            for (uint32_t k = 1; k < d->as.list.len; k++) lower_import_set(sl, d->as.list.items[k]);
                        else err(d, "cond-expand inside define-library takes (import ...) and (begin ...) declarations");
                    }
            } else if (is_include_head(sl, decl, &lib_fold)) {
                FB got = {0};
                if (include_files(sl, decl, lib_fold, &got))
                    for (uint32_t j = 0; j < got.n; j++) lower_toplevel(sl, got.items[j], &sl->lib_body);
                free(got.items);
            } else {
                err(decl, "define-library declarations are (export ...), (import ...), (begin ...) and (cond-expand ...)");
            }
        }
        /* The aliases, after every definition they may name.  Of a
         * fixed-arity procedure the library defines, a forwarding defn, so a
         * Turmeric importer can call it by name as it calls the original;
         * of anything else, an `any`. */
        for (uint32_t k = 0; k < n_ren; k++) {
            if (!ren[k].alias) continue;
            const Symbol *target = rn(sl, ren[k].in);
            const Form *params = NULL;
            for (uint32_t b = 0; b < sl->lib_body.n && !params; b++) {
                const Form *d = sl->lib_body.items[b];
                if (head_is(d, sl->t_defn) && d->as.list.len >= 4 && is_sym(d->as.list.items[1], target) &&
                    d->as.list.items[2]->tag == F_VEC)
                    params = d->as.list.items[2];
            }
            bool fixed = params != NULL;
            for (uint32_t p = 0; fixed && p < params->as.list.len; p++) {
                const Form *x = params->as.list.items[p];
                fixed = x->tag == F_SYM && x->as.sym->name[0] != '^' && x->as.sym != sl->t_amp;
            }
            if (fixed) {
                FB fp = {0}, call = {0};
                fb_push(&call, Sym(sl, sp, target));
                for (uint32_t p = 0; p < params->as.list.len; p++) {
                    const Symbol *v = fresh(sl, "fwd__");
                    fb_push(&fp, Sym(sl, sp, v));
                    fb_push(&call, Sym(sl, sp, v));
                }
                fb_push(&sl->lib_body, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, ren[k].out),
                                          fb_vec(sl, &fp, sp), fb_list(sl, &call, sp)));
                continue;
            }
            Form *val = lower(sl, Sym(sl, sp, ren[k].in));
            fb_push(&sl->lib_body, Ln(sl, sp, 3, Sym(sl, sp, sl->t_def), Sym(sl, sp, ren[k].out),
                                      Ln(sl, sp, 3, Sym(sl, sp, I(sl, "::")), val, Sym(sl, sp, sl->t_any))));
        }
        return;
    }
    if (head_is(f, sl->s_cond_expand)) {
        uint32_t n; Form **items;
        if (cond_expand_clause(sl, f, &n, &items))
            for (uint32_t i = 0; i < n; i++) lower_toplevel(sl, items[i], out);
        return;
    }
    if (head_is(f, sl->s_define_record_type)) {
        lower_record_type(sl, f, out, false);
        return;
    }
    if (head_is(f, sl->s_define) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_LIST &&
        f->as.list.items[1]->as.list.len >= 1 && is_sym(f->as.list.items[1]->as.list.items[0], sl->s_main) &&
        f->as.list.items[1]->as.list.len == 1)
        sl->user_main = true;
    /* A top-level expression of the program (not of a library body): every
     * define after it runs its initializer in order -- see the define arm. */
    if (out != &sl->lib_body && sl->in_user) sl->toplevel_expr_seen = true;
    /* R9 / r7rs-repl-toplevel-expression-value-not-widened: at the REPL
     * prompt (a synthetic `<eval>` source, the user's lines after the pinned
     * preload) a top-level expression's value is what the prompt echoes, and
     * it must be a Scheme value: pass it through an `any` parameter, as a
     * procedure's result is.  A program's top-level expression value is
     * discarded, so a program is left alone; so is a Turmeric form typed at
     * the prompt, whose value is Turmeric's. */
    {
        const SourceFile *sf = diag_source_file(sp.file_id);
        bool at_prompt = sf && sf->path && sf->path[0] == '<' &&
                         g_synthetic_user_from_line && sp.line >= g_synthetic_user_from_line;
        bool turmeric_form = f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
                             tur_name_is_reserved_special_form(f->as.list.items[0]->as.sym->name) &&
                             !is_scheme_syntax_name(f->as.list.items[0]->as.sym->name);
        Form *lowered;
        if (at_prompt) {
            lowered = lower(sl, f);
            if (!turmeric_form)
                lowered = Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-repl-value__")), lowered);
        } else if (out != &sl->lib_body && !turmeric_form) {
            lowered = lower_toplevel_stmt(sl, f);
        } else {
            lowered = lower(sl, f);
        }
        fb_push(out, lowered);
    }
}

/* ---------------------------------------------------------------------------
 * R3 / D9: the library system and the `(turmeric ...)` seam.
 * ------------------------------------------------------------------------- */

/* Is `<name>.tur` an auto-loaded stdlib file?  Its names are global already,
 * so `(import (turmeric stdlib/<name>))` is a no-op rather than an import
 * the module loader would fail to resolve. */
static bool stdlib_autoloaded(const char *name) {
    const char *const *files = tur_stdlib_autoload_files();
    size_t n = strlen(name);
    for (size_t i = 0; files && files[i]; i++) {
        const char *f = files[i];
        size_t fl = strlen(f);
        if (fl == n + 4 && memcmp(f, name, n) == 0 && strcmp(f + n, ".tur") == 0) return true;
    }
    return false;
}

/* The Turmeric module a library NAME denotes, or NULL when the import is a
 * no-op (`(scheme ...)` is the prelude; an auto-loaded stdlib file is already
 * global).  `(turmeric a/b/c)` is the module `a/b/c` with a leading `stdlib/`
 * dropped (the module loader's stdlib fallback finds it); any other name
 * `(my utils)` joins with `/`. */
static const Symbol *library_module(SL *sl, Form *set, bool *ok) {
    *ok = true;
    if (set->tag != F_LIST || set->as.list.len == 0 || set->as.list.items[0]->tag != F_SYM) {
        err(set, "a library name is a list of identifiers, e.g. (scheme base) or (turmeric stdlib/vec)");
        *ok = false;
        return NULL;
    }
    const char *head = set->as.list.items[0]->as.sym->name;
    if (strcmp(head, "scheme") == 0) {
        /* R7: every R7RS-small library is known by name.  A resident one is
         * the prelude; an on-demand one is spliced in by the load expander;
         * a deferred one says why it is not here yet. */
        int li = scheme_lib_index(set);
        if (li < 0) {
            err(set, "no such library in R7RS-small: the (scheme ...) libraries are base, case-lambda, "
                     "char, complex, cxr, eval, file, inexact, lazy, load, process-context, r5rs, "
                     "read, repl, time and write");
            *ok = false;
            return NULL;
        }
        if (SCHEME_LIBS[li].kind == LIB_DEFERRED) {
            err(set, "(scheme %s) is not supported yet: it %s", SCHEME_LIBS[li].name, SCHEME_LIBS[li].what);
            *ok = false;
            return NULL;
        }
        sl->lib_imported[li] = true;
        return NULL;
    }
    if (strcmp(head, "srfi") == 0 || strcmp(head, "sicp") == 0) {
        /* r7rs-srfi-plan D1: an SRFI is a built-in library, like (scheme
         * ...): srfi_import binds it; it is never a module.  So is (sicp
         * extras). */
        if (srfi_libname_num(set) < 0) {
            err(set, "%s", bad_libname_msg(set));
            *ok = false;
        }
        return NULL;
    }
    if (strcmp(head, "turmeric") == 0) {
        if (set->as.list.len != 2 || set->as.list.items[1]->tag != F_SYM) {
            err(set, "(turmeric <module>) takes one module path, e.g. (turmeric stdlib/vec) or (turmeric json/encode)");
            *ok = false;
            return NULL;
        }
        const char *m = set->as.list.items[1]->as.sym->name;
        if (strncmp(m, "stdlib/", 7) == 0) m += 7;
        if (stdlib_autoloaded(m)) return NULL;
        return I(sl, m);
    }
    char buf[256]; size_t at = 0;
    for (uint32_t i = 0; i < set->as.list.len; i++) {
        const Form *p = set->as.list.items[i];
        /* R7RS 7.1.7: a part is an identifier or an exact non-negative
         * integer, `(mylib 2)` -> `mylib/2`, as Racket's R7RS spells it. */
        if (p->tag == F_INT && p->as.i >= 0) {
            int w = snprintf(buf + at, sizeof buf - at, "%s%lld", i ? "/" : "", (long long)p->as.i);
            if (w < 0 || (size_t)w >= sizeof buf - at) { err(set, "library name too long"); *ok = false; return NULL; }
            at += (size_t)w;
            continue;
        }
        if (p->tag != F_SYM) { err(p, "a library name part must be an identifier or an exact non-negative integer"); *ok = false; return NULL; }
        int w = snprintf(buf + at, sizeof buf - at, "%s%s", i ? "/" : "", p->as.sym->name);
        if (w < 0 || (size_t)w >= sizeof buf - at) { err(set, "library name too long"); *ok = false; return NULL; }
        at += (size_t)w;
    }
    return I(sl, buf);
}

/* R7RS 5.2: an import set is a library name under any nesting of `only`,
 * `except`, `prefix` and `rename`.  The nest is folded inside out into one
 * spec -- the library, the kept names (or all), the excluded names, one
 * prefix, the renames -- each name unwound to the LIBRARY's spelling
 * through the modifiers inside it, and the spec is emitted once.  Where a
 * (scheme ...) library's names are global, `only` and `except` change what
 * a name MEANS, not what is visible: an excluded name is no longer the
 * library's (`(except (scheme base) assoc)` lets the program define its own
 * `assoc`), and a kept name stays the library's.  A user library or a
 * Turmeric module gets a `:refer` list from `only`, `:as` from `prefix`, and
 * a full import from `except` (Turmeric's import has no "all but"). */
typedef struct {
    Form *lib;               /* the library name */
    bool  has_only;
    FB    only;              /* library-spelling names kept (has_only) */
    FB    except;            /* library-spelling names dropped */
    const Symbol *prefix;    /* one prefix, or NULL */
    FB    renames;           /* pairs: new name, library-spelling name */
} SchemeImportSpec;
/* The library's spelling of a name written at the spec's current level:
 * take the spec's renames and prefix off, innermost first. */
static const Symbol *import_unwind(SL *sl, const SchemeImportSpec *spec, const Symbol *s) {
    for (int guard = 0; guard < 64; guard++) {
        bool moved = false;
        for (uint32_t i = 0; i + 1 < spec->renames.n; i += 2)
            if (spec->renames.items[i]->as.sym == s) { s = spec->renames.items[i + 1]->as.sym; moved = true; break; }
        if (!moved && spec->prefix && s->len > spec->prefix->len &&
            memcmp(s->name, spec->prefix->name, spec->prefix->len) == 0) {
            s = I(sl, s->name + spec->prefix->len);
            moved = true;
        }
        if (!moved) return s;
    }
    return s;
}
static bool resolve_import_set(SL *sl, Form *set, SchemeImportSpec *spec) {
    if (set->tag != F_LIST || set->as.list.len == 0) { err(set, "malformed import set"); return false; }
    Form *head = set->as.list.items[0];
    const char *h = head->tag == F_SYM ? head->as.sym->name : "";
    bool mod = strcmp(h, "only") == 0 || strcmp(h, "except") == 0 || strcmp(h, "prefix") == 0 ||
               strcmp(h, "rename") == 0;
    if (!mod) { spec->lib = set; return true; }
    if (set->as.list.len < 2) { err(set, "(%s <import set> ...) needs an import set", h); return false; }
    if (!resolve_import_set(sl, set->as.list.items[1], spec)) return false;
    if (strcmp(h, "only") == 0) {
        FB kept = {0};
        for (uint32_t i = 2; i < set->as.list.len; i++) {
            Form *nm = set->as.list.items[i];
            if (nm->tag != F_SYM) { err(nm, "(only ...) names must be identifiers"); free(kept.items); return false; }
            fb_push(&kept, Sym(sl, nm->span, import_unwind(sl, spec, nm->as.sym)));
        }
        /* `only` inside `only`: the intersection is the outer list. */
        free(spec->only.items);
        spec->only = kept;
        spec->has_only = true;
        return true;
    }
    if (strcmp(h, "except") == 0) {
        for (uint32_t i = 2; i < set->as.list.len; i++) {
            Form *nm = set->as.list.items[i];
            if (nm->tag != F_SYM) { err(nm, "(except ...) names must be identifiers"); return false; }
            fb_push(&spec->except, Sym(sl, nm->span, import_unwind(sl, spec, nm->as.sym)));
        }
        return true;
    }
    if (strcmp(h, "rename") == 0) {
        for (uint32_t i = 2; i < set->as.list.len; i++) {
            Form *pr = set->as.list.items[i];
            if (pr->tag != F_LIST || pr->as.list.len != 2 || pr->as.list.items[0]->tag != F_SYM ||
                pr->as.list.items[1]->tag != F_SYM) {
                err(pr, "(rename <set> (from to) ...) takes identifier pairs"); return false;
            }
            const Symbol *orig = import_unwind(sl, spec, pr->as.list.items[0]->as.sym);
            fb_push(&spec->renames, pr->as.list.items[1]);
            fb_push(&spec->renames, Sym(sl, pr->span, orig));
        }
        return true;
    }
    /* prefix */
    if (set->as.list.len != 3 || set->as.list.items[2]->tag != F_SYM) {
        err(set, "(prefix <set> <identifier>) takes one prefix identifier"); return false;
    }
    if (spec->prefix) { err(set, "an import set takes one (prefix ...)"); return false; }
    spec->prefix = set->as.list.items[2]->as.sym;
    return true;
}
static bool spec_excludes(const SchemeImportSpec *spec, const Symbol *s) {
    for (uint32_t i = 0; i < spec->except.n; i++) if (spec->except.items[i]->as.sym == s) return true;
    return false;
}
typedef struct LibSyntax LibSyntax;
static LibSyntax *lib_syntax_of(SL *sl, const Form *libname, const Symbol *mod);
static bool lib_syntax_exports(const LibSyntax *lx, const Symbol *pub);
static void lib_syntax_import(SL *sl, LibSyntax *lx, const SchemeImportSpec *spec, Span sp);
static void lib_bind_respelled(SL *sl, const LibSyntax *lx, const SchemeImportSpec *spec);
/* r7rs-turmeric-syntax-leaks item 8: the auto-loaded stdlib file a
 * `(turmeric stdlib/<file>)` (or `(turmeric <file>)`) library name denotes,
 * or NULL for any other library. */
static const Symbol *turmeric_stdlib_file(SL *sl, const Form *lib) {
    if (!lib || lib->tag != F_LIST || lib->as.list.len != 2 ||
        lib->as.list.items[0]->tag != F_SYM || lib->as.list.items[1]->tag != F_SYM ||
        strcmp(lib->as.list.items[0]->as.sym->name, "turmeric") != 0)
        return NULL;
    const char *m = lib->as.list.items[1]->as.sym->name;
    if (strncmp(m, "stdlib/", 7) == 0) m += 7;
    return stdlib_autoloaded(m) ? I(sl, m) : NULL;
}
static bool spec_excludes(const SchemeImportSpec *spec, const Symbol *s);
/* What such an import makes visible: an `only` list name by name, anything
 * else the whole file less what an `except` or a `rename` took away.  A
 * `prefix` keeps the bare names hidden -- `v:vec-new` reaches its name
 * through the prefix rule. */
static void grant_stdlib_import(SL *sl, const SchemeImportSpec *spec, const Symbol *file) {
    if (spec->has_only) {
        for (uint32_t i = 0; i < spec->only.n; i++) {
            const Symbol *o = spec->only.items[i]->as.sym;
            bool renamed = false;
            for (uint32_t r = 0; r + 1 < spec->renames.n && !renamed; r += 2)
                renamed = spec->renames.items[r + 1]->as.sym == o;
            if (!spec_excludes(spec, o) && !renamed && !spec->prefix)
                sym_list_push(&sl->granted, &sl->n_granted, &sl->cap_granted, o);
        }
        return;
    }
    if (spec->prefix) return;
    if (!sym_list_has(sl->granted_files, sl->n_granted_files, file) && sl->n_granted_files < 32)
        sl->granted_files[sl->n_granted_files++] = file;
    for (uint32_t i = 0; i < spec->except.n; i++)
        sym_list_push(&sl->denied, &sl->n_denied, &sl->cap_denied, spec->except.items[i]->as.sym);
    for (uint32_t r = 0; r + 1 < spec->renames.n; r += 2)
        sym_list_push(&sl->denied, &sl->n_denied, &sl->cap_denied, spec->renames.items[r + 1]->as.sym);
}
/* Keep a REPL turn's grants for the turns after it. */
static void repl_grants_save(const SL *sl) {
    for (uint32_t i = 0; i < sl->n_granted; i++) names_add(&g_repl_grants.granted, sl->granted[i]->name);
    for (uint32_t i = 0; i < sl->n_denied; i++) names_add(&g_repl_grants.denied, sl->denied[i]->name);
    for (uint32_t i = 0; i < sl->n_granted_files; i++) names_add(&g_repl_grants.files, sl->granted_files[i]->name);
}
static void repl_grants_load(SL *sl) {
    for (uint32_t i = 0; i < g_repl_grants.granted.n; i++)
        sym_list_push(&sl->granted, &sl->n_granted, &sl->cap_granted, I(sl, g_repl_grants.granted.items[i]));
    for (uint32_t i = 0; i < g_repl_grants.denied.n; i++)
        sym_list_push(&sl->denied, &sl->n_denied, &sl->cap_denied, I(sl, g_repl_grants.denied.items[i]));
    for (uint32_t i = 0; i < g_repl_grants.files.n && sl->n_granted_files < 32; i++) {
        const Symbol *f = I(sl, g_repl_grants.files.items[i]);
        if (!sym_list_has(sl->granted_files, sl->n_granted_files, f)) sl->granted_files[sl->n_granted_files++] = f;
    }
}
/* May user Scheme source name `s`, which resolved to itself?  Anything that
 * is not an auto-loaded stdlib global may (the elaborator reports what is
 * unbound); a stdlib global only through an import. */
static bool scheme_std_visible(SL *sl, const Symbol *s) {
    if (sym_list_has(sl->granted, sl->n_granted, s)) return true;
    const Symbol *file = std_file_of(sl, s);
    /* A built-in (`println`, `str`, `mod`) is no file's, so nothing grants
     * it; a Scheme spelling of one was renamed onto the prelude before
     * this is asked. */
    if (!file) return builtin_first_with_name(s) == NULL;
    return sym_list_has(sl->granted_files, sl->n_granted_files, file) &&
           !sym_list_has(sl->denied, sl->n_denied, s);
}

static void emit_import_spec(SL *sl, Span sp, SchemeImportSpec *spec) {
    bool ok;
    {
        const Symbol *tfile = import_grants(sl, sp) ? turmeric_stdlib_file(sl, spec->lib) : NULL;
        if (tfile) grant_stdlib_import(sl, spec, tfile);
    }
    const Symbol *mod = library_module(sl, spec->lib, &ok);
    if (!ok) return;
    /* r7rs-define-library-cannot-export-syntax: a Scheme library's exported
     * macros -- registered here, and kept out of the module's `:refer`. */
    LibSyntax *lx = lib_syntax_of(sl, spec->lib, mod);
    if (lx) lib_syntax_import(sl, lx, spec, sp);
    lib_bind_respelled(sl, lx, spec);
    /* An excluded name is no longer the library's: it resolves as the
     * program's own (rn_global skips the standard map for it). */
    for (uint32_t i = 0; i < spec->except.n; i++) {
        const Symbol *x = spec->except.items[i]->as.sym;
        if (sl->n_excluded < 64) sl->excluded[sl->n_excluded++] = x;
        else { err(spec->except.items[i], "too many (except ...) names"); return; }
    }
    /* Renames: the new name means the library's (rn'd) name -- for a
     * (scheme ...) library, its standard meaning, whatever an `except` of the
     * same name in another import set made the bare name mean. */
    FB refer = {0};
    for (uint32_t i = 0; i + 1 < spec->renames.n; i += 2) {
        const Symbol *orig = spec->renames.items[i + 1]->as.sym;
        if (lx && lib_syntax_exports(lx, orig)) continue;   /* a macro: registered above */
        const Symbol *to = mod ? lib_respelling(sl, lx, orig) : NULL;
        if (!to) to = mod ? rn(sl, orig) : rn_std(sl, orig);
        if (sl->n_renames < 64) {
            sl->renames[sl->n_renames].from = spec->renames.items[i]->as.sym;
            sl->renames[sl->n_renames].to   = to;
            sl->n_renames++;
        } else { err(spec->renames.items[i], "too many (rename ...) names"); free(refer.items); return; }
        if (!spec->has_only) fb_push(&refer, Sym(sl, spec->renames.items[i + 1]->span, to));
    }
    if (spec->has_only)
        for (uint32_t i = 0; i < spec->only.n; i++) {
            const Symbol *o = spec->only.items[i]->as.sym;
            if (lx && lib_syntax_exports(lx, o)) continue;   /* a macro: registered above */
            const Symbol *osp = lib_respelling(sl, lx, o);
            if (!spec_excludes(spec, o)) fb_push(&refer, Sym(sl, spec->only.items[i]->span, osp ? osp : rn(sl, o)));
        }
    if (spec->prefix) {
        const Symbol *pfx = spec->prefix;
        if (sl->n_prefixes >= 16) { err(spec->lib, "too many (prefix ...) imports"); free(refer.items); return; }
        if (!mod) {
            /* Auto-loaded: the names are global and bare, so the prefix just
             * comes off.  Recorded as an alias-less rule. */
            sl->prefixes[sl->n_prefixes].prefix = pfx->name;
            sl->prefixes[sl->n_prefixes].plen   = pfx->len;
            sl->prefixes[sl->n_prefixes].alias  = NULL;
            sl->prefixes[sl->n_prefixes].lx     = NULL;
            sl->n_prefixes++;
            free(refer.items);
            return;
        }
        /* The alias is the prefix without a trailing ':' or '-'; `p:name` then
         * reads as `p/name`, which is Turmeric's `:as p` spelling. */
        char alias[128];
        snprintf(alias, sizeof alias, "%s", pfx->name);
        size_t al = strlen(alias);
        while (al > 0 && (alias[al - 1] == ':' || alias[al - 1] == '-')) alias[--al] = '\0';
        if (al == 0) { err(spec->lib, "(prefix ...) needs a non-empty prefix"); free(refer.items); return; }
        sl->prefixes[sl->n_prefixes].prefix = pfx->name;
        sl->prefixes[sl->n_prefixes].plen   = pfx->len;
        sl->prefixes[sl->n_prefixes].alias  = I(sl, alias);
        sl->prefixes[sl->n_prefixes].lx     = lx;
        sl->n_prefixes++;
        fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                 Kw(sl, sp, sl->t_as), Sym(sl, sp, I(sl, alias))));
        sl->needs_module = true;
        /* A renamed name of a prefixed module still needs the bare export. */
        if (refer.n > 0) {
            fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                     Kw(sl, sp, sl->t_refer), fb_vec(sl, &refer, sp)));
        } else free(refer.items);
        return;
    }
    if (!mod) { free(refer.items); return; }   /* a (scheme ...) library: already global */
    if (spec->has_only || refer.n > 0) {
        fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod),
                                 Kw(sl, sp, sl->t_refer), fb_vec(sl, &refer, sp)));
    } else {
        free(refer.items);
        fb_push(&sl->imports, Ln(sl, sp, 2, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod)));
    }
    sl->needs_module = true;
}

/* One import set -> zero or one Turmeric `(import ...)` form in sl->imports,
 * plus the rename/prefix rules `rename`/`prefix` need. */
static void srfi_import(SL *sl, Form *libname, const SchemeImportSpec *spec, Span sp);
static void lower_import_set(SL *sl, Form *set) {
    Span sp = set->span;
    if (set->tag != F_LIST || set->as.list.len == 0) { err(set, "malformed import set"); return; }
    Form *head = set->as.list.items[0];
    const char *h = head->tag == F_SYM ? head->as.sym->name : "";
    bool ok;
    if (strcmp(h, "only") == 0 || strcmp(h, "rename") == 0 || strcmp(h, "prefix") == 0 ||
        strcmp(h, "except") == 0) {
        SchemeImportSpec spec = {0};
        if (!resolve_import_set(sl, set, &spec)) { free(spec.only.items); free(spec.except.items); free(spec.renames.items); return; }
        if (is_srfi_libname(spec.lib)) srfi_import(sl, spec.lib, &spec, sp);
        else emit_import_spec(sl, sp, &spec);
        free(spec.only.items); free(spec.except.items); free(spec.renames.items);
        return;
    }
    if (is_srfi_libname(set)) { srfi_import(sl, set, NULL, sp); return; }
    if (import_grants(sl, sp)) {
        /* r7rs-turmeric-syntax-leaks item 8: the whole file becomes visible. */
        const Symbol *tfile = turmeric_stdlib_file(sl, set);
        if (tfile) {
            SchemeImportSpec whole = {0};
            whole.lib = set;
            grant_stdlib_import(sl, &whole, tfile);
        }
    }
    const Symbol *mod = library_module(sl, set, &ok);
    if (!ok || !mod) return;
    fb_push(&sl->imports, Ln(sl, sp, 2, Sym(sl, sp, sl->t_import), Sym(sl, sp, mod)));
    sl->needs_module = true;
    LibSyntax *lx = lib_syntax_of(sl, set, mod);
    if (lx) lib_syntax_import(sl, lx, NULL, sp);
    lib_bind_respelled(sl, lx, NULL);
}

/* R7: the feature identifiers cond-expand holds -- with `srfi-N` for every
 * SRFI_LIBS row that is here (feature_holds) -- the same list `(features)`
 * returns (r7rs-features in the prelude, written out there because the
 * prelude is Turmeric-shaped and is loaded unlowered when a Turmeric program
 * imports a Scheme library).  They are kept equal twice over:
 * tests/fixtures/r7rs-features-agree asks cond-expand, through `eval`, about
 * every identifier `(features)` returns, and tests/check-r7rs-srfi-sync.sh
 * compares the prelude's list with this one and the table
 * (docs/archive/r7rs-cond-expand-ratios-feature-drift.md). */
static const char *const R7RS_FEATURES[] = { "r7rs", "exact-closed", "ratios", "turmeric" };
/* cond-expand feature requirements: the R7RS_FEATURES, `else` and any
 * `(library (scheme ...))` hold; `and`/`or`/`not` compose. */
static bool feature_holds(SL *sl, Form *req) {
    if (req->tag == F_SYM) {
        const char *n = req->as.sym->name;
        if (strcmp(n, "else") == 0) return true;
        for (size_t i = 0; i < sizeof R7RS_FEATURES / sizeof R7RS_FEATURES[0]; i++)
            if (strcmp(n, R7RS_FEATURES[i]) == 0) return true;
        /* r7rs-srfi-plan D4: `srfi-N` for every SRFI that is here. */
        if (strncmp(n, "srfi-", 5) == 0 && n[5] >= '0' && n[5] <= '9') {
            char *end = NULL;
            long long num = strtoll(n + 5, &end, 10);
            return end && *end == '\0' && num < SICP_EXTRAS_NUM && srfi_supported(srfi_row(num));
        }
        return false;
    }
    if (req->tag != F_LIST || req->as.list.len == 0 || req->as.list.items[0]->tag != F_SYM) return false;
    const char *h = req->as.list.items[0]->as.sym->name;
    if (strcmp(h, "and") == 0) {
        for (uint32_t i = 1; i < req->as.list.len; i++) if (!feature_holds(sl, req->as.list.items[i])) return false;
        return true;
    }
    if (strcmp(h, "or") == 0) {
        for (uint32_t i = 1; i < req->as.list.len; i++) if (feature_holds(sl, req->as.list.items[i])) return true;
        return false;
    }
    if (strcmp(h, "not") == 0) return req->as.list.len == 2 && !feature_holds(sl, req->as.list.items[1]);
    if (strcmp(h, "library") == 0 && req->as.list.len == 2 && is_srfi_libname(req->as.list.items[1]))
        return srfi_importable(srfi_row(srfi_libname_num(req->as.list.items[1])));
    if (strcmp(h, "library") == 0 && req->as.list.len == 2) {
        /* R7: a (scheme ...) requirement asks the library table, quietly -- a
         * deferred or unknown library is simply absent. */
        if (is_scheme_libname(req->as.list.items[1])) {
            int li = scheme_lib_index(req->as.list.items[1]);
            return li >= 0 && SCHEME_LIBS[li].kind != LIB_DEFERRED;
        }
        bool ok; const Symbol *m = library_module(sl, req->as.list.items[1], &ok);
        (void)m;
        return ok;   /* (scheme ...) and an auto-loaded stdlib file hold; a module we cannot check is assumed present */
    }
    return false;
}
/* The body of the first cond-expand clause that holds, or NULL. */
static Form *cond_expand_clause(SL *sl, Form *f, uint32_t *out_n, Form ***out_items) {
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        Form *cl = f->as.list.items[i];
        if (cl->tag != F_LIST || cl->as.list.len == 0) { err(cl, "cond-expand clause expects (<feature requirement> body...)"); return NULL; }
        if (feature_holds(sl, cl->as.list.items[0])) {
            *out_items = cl->as.list.items + 1;
            *out_n = cl->as.list.len - 1;
            return cl;
        }
    }
    *out_n = 0; *out_items = NULL;
    return NULL;
}

/* (define-record-type <name> (ctor f...) pred (f accessor [modifier])...)
 * -> a heap defstruct of `any` fields plus the procedures (D3: a record is
 * an ordinary Turmeric type, its predicate an `is?`).
 *
 * `local`: the form is one of a body's definitions.  The declarations still
 * go to `out` (the top level), but the struct gets a fresh name, and so does
 * each procedure -- bound in the body's scope, so the body's `(mk 7)` reads
 * the lifted `mk__vN` and a second body's record type of the same name is a
 * different type (r7rs-define-record-type-not-an-internal-definition). */
static void lower_record_type(SL *sl, Form *f, FB *out, bool local) {
    Span sp = f->span;
    if (f->as.list.len < 4 || f->as.list.items[1]->tag != F_SYM || f->as.list.items[2]->tag != F_LIST ||
        f->as.list.items[3]->tag != F_SYM) {
        err(f, "define-record-type expects (define-record-type <name> (ctor field...) pred (field accessor [modifier])...)");
        return;
    }
    Form *ctor = f->as.list.items[2];
    if (ctor->as.list.len < 1 || ctor->as.list.items[0]->tag != F_SYM) { err(ctor, "record constructor spec expects (name field...)"); return; }
    for (uint32_t i = 1; i < ctor->as.list.len; i++)
        if (ctor->as.list.items[i]->tag != F_SYM) { err(ctor->as.list.items[i], "constructor field must be an identifier"); return; }
    uint32_t nf = f->as.list.len - 4;
    for (uint32_t i = 0; i < nf; i++) {
        Form *spec = f->as.list.items[4 + i];
        if (spec->tag != F_LIST || spec->as.list.len < 2 || spec->as.list.len > 3 || spec->as.list.items[0]->tag != F_SYM) {
            err(spec, "record field spec expects (field accessor [modifier])"); return;
        }
        if (spec->as.list.items[1]->tag != F_SYM) { err(spec, "accessor must be an identifier"); return; }
        if (spec->as.list.len == 3 && spec->as.list.items[2]->tag != F_SYM) { err(spec, "modifier must be an identifier"); return; }
    }
    /* Struct name: the record name with `<`/`>` dropped and a prefix, so it
     * can never collide with a stdlib type. */
    char sbuf[128]; size_t at = 0;
    const char *rn_name = f->as.list.items[1]->as.sym->name;
    at += (size_t)snprintf(sbuf, sizeof sbuf, "R7rsRec_");
    for (const char *p = rn_name; *p && at + 3 < sizeof sbuf; p++) {
        char c = *p;
        if (c == '<' || c == '>') continue;
        sbuf[at++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    sbuf[at] = '\0';
    if (local) { sbuf[at++] = '_'; sbuf[at++] = '_'; sbuf[at] = '\0'; }
    const Symbol *sname = local ? fresh(sl, sbuf) : I(sl, sbuf);
    /* The procedures' names: a local record binds each one in the body's
     * scope first, then the rest is generated as top-level code, outside
     * every enclosing scope (a constructor parameter is not the enclosing
     * procedure's variable of the same name). */
    #define REC_NAME(s) (local ? bind_name(sl, (s), sp) : rn(sl, (s)))
    const Symbol *ctor_name = REC_NAME(ctor->as.list.items[0]->as.sym);
    const Symbol *pred_name = REC_NAME(f->as.list.items[3]->as.sym);
    const Symbol **acc_names = (const Symbol **)arena_alloc(sl->a, (nf + 1) * sizeof(*acc_names));
    const Symbol **mod_names = (const Symbol **)arena_alloc(sl->a, (nf + 1) * sizeof(*mod_names));
    for (uint32_t i = 0; i < nf; i++) {
        Form *spec = f->as.list.items[4 + i];
        acc_names[i] = REC_NAME(spec->as.list.items[1]->as.sym);
        mod_names[i] = spec->as.list.len == 3 ? REC_NAME(spec->as.list.items[2]->as.sym) : NULL;
    }
    #undef REC_NAME
    LFrame *saved_scope = sl->scope;
    if (local) sl->scope = NULL;
    /* Fields, in declaration order. */
    const Symbol **fields = (const Symbol **)arena_alloc(sl->a, (nf + 1) * sizeof(*fields));
    FB fvec = {0};
    for (uint32_t i = 0; i < nf; i++) {
        Form *spec = f->as.list.items[4 + i];
        fields[i] = spec->as.list.items[0]->as.sym;
        fb_push(&fvec, Sym(sl, spec->span, fields[i]));
        fb_push(&fvec, AnyAnn(sl, spec->span));
    }
    /* r7rs-record-type-without-fields: a record type may have no fields
     * (SRFI 41's stream-null marker), and a struct may not; it gets one
     * hidden field, always nil, that nothing reads. */
    if (nf == 0) {
        fb_push(&fvec, Sym(sl, sp, I(sl, "r7rs-no-fields__")));
        fb_push(&fvec, AnyAnn(sl, sp));
    }
    fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defstruct), Sym(sl, sp, sname), Kw(sl, sp, sl->t_heap),
                    fb_vec(sl, &fvec, sp)));
    /* Constructor: its parameters name a subset of the fields, in any order;
     * an unmentioned field starts as nil. */
    FB params = {0}, args = {0};
    for (uint32_t i = 1; i < ctor->as.list.len; i++) {
        Form *p = ctor->as.list.items[i];
        fb_push(&params, Sym(sl, p->span, rn(sl, p->as.sym)));
    }
    fb_push(&args, Sym(sl, sp, sname));
    for (uint32_t i = 0; i < nf; i++) {
        bool named = false;
        for (uint32_t j = 1; j < ctor->as.list.len; j++)
            if (ctor->as.list.items[j]->as.sym == fields[i]) { named = true; break; }
        fb_push(&args, named ? Sym(sl, sp, rn(sl, fields[i])) : Nil(sl, sp));
    }
    if (nf == 0) fb_push(&args, Nil(sl, sp));
    fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, ctor_name),
                    fb_vec(sl, &params, sp), fb_list(sl, &args, sp)));
    /* Predicate. */
    {
        const Symbol *x = I(sl, "x");
        Form *pv[1] = { Sym(sl, sp, x) };
        fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_defn), Sym(sl, sp, pred_name),
                        Vec(sl, sp, pv, 1), form_type_ann(sl->a, sp, Sym(sl, sp, sl->t_bool)),
                        Ln(sl, sp, 3, Sym(sl, sp, sl->t_is), Sym(sl, sp, x), Sym(sl, sp, sname))));
    }
    /* Accessors and modifiers. */
    for (uint32_t i = 0; i < nf; i++) {
        const Symbol *r = I(sl, "r");
        char fld[128]; snprintf(fld, sizeof fld, ".%s", fields[i]->name);
        Form *sann = form_type_ann(sl->a, sp, Sym(sl, sp, sname));
        Form *acc_params[2] = { Sym(sl, sp, r), sann };
        Form *read = Ln(sl, sp, 2, Sym(sl, sp, I(sl, fld)), Sym(sl, sp, r));
        fb_push(out, Ln(sl, sp, 5, Sym(sl, sp, sl->t_defn), Sym(sl, sp, acc_names[i]),
                        Vec(sl, sp, acc_params, 2), AnyAnn(sl, sp), read));
        if (mod_names[i]) {
            const Symbol *v = I(sl, "v");
            Form *mod_params[3] = { Sym(sl, sp, r), form_type_ann(sl->a, sp, Sym(sl, sp, sname)), Sym(sl, sp, v) };
            Form *store = Ln(sl, sp, 3, Sym(sl, sp, sl->t_set), read, Sym(sl, sp, v));
            fb_push(out, Ln(sl, sp, 4, Sym(sl, sp, sl->t_defn), Sym(sl, sp, mod_names[i]),
                            Vec(sl, sp, mod_params, 3), store));
        }
    }
    sl->scope = saved_scope;
}

bool scheme_lower_needed(Form *const *forms, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) return true;
    return false;
}

/* ---- R10: assignment conversion -----------------------------------------
 *
 * A compiled closure COPIES the variables it captures into its environment.
 * That is right for a variable nothing assigns, and wrong for one that is
 * `set!`: `(let ((sum 0)) (do ((i 0 (+ i 1))) ((= i n)) (set! sum (+ sum
 * i))) sum)` answered 0, because the `do` loop is a lambda and its `set!`
 * updated the loop's copy.  The interpreter's frames are shared, so it said
 * 10 -- the back ends disagreed on a basic R7RS program.
 *
 * The textbook fix, on the lowered forms: a `^mut` `let` binding that a
 * nested `fn` mentions becomes a heap cell (`R7rsBox`, the prelude), bound
 * once and never reassigned, so every closure's copy is the same pointer.
 * Each read of the name in its scope becomes `(r7rs-unbox__ n)` and each
 * `(set! n v)` becomes `(r7rs-box-set!__ n v)`; a scope that rebinds the name
 * (a `let`, `letrec`, `fn` or `defn` parameter) stops the rewrite.
 *
 * T5 widens it to EVERY `set!` variable, seen by a lambda or not.  A
 * re-entrant continuation is a copy of the C stack, so a plain mutable cell --
 * a C local -- comes back holding its value at the capture, where R7RS says a
 * variable is a location the continuation shares: a `results` list consed
 * onto after a re-entry lost the first result, forever.  A heap cell is the
 * location, so re-entry sees every assignment. */
static bool ac_is_set(SL *sl, const Form *f) {
    return f->tag == F_LIST && f->as.list.len == 3 &&
           (is_sym(f->as.list.items[0], sl->t_set) || is_sym(f->as.list.items[0], sl->s_set));
}
static bool ac_vec_binds(const Form *pv, const Symbol *n) {
    if (!pv || pv->tag != F_VEC) return false;
    for (uint32_t i = 0; i < pv->as.list.len; i++)
        if (is_sym(pv->as.list.items[i], n)) return true;
    return false;
}
/* One binding of a lowered `let` vector: `[^marker] name [: T] init`. */
typedef struct { uint32_t name, init; bool mut; } AcBind;
static uint32_t ac_parse_binds(SL *sl, const Form *v, AcBind *out) {
    uint32_t n = 0, i = 0, len = v->as.list.len;
    while (i < len) {
        bool mut = false;
        const Form *it = v->as.list.items[i];
        if (it->tag == F_SYM && it->as.sym->name[0] == '^') { mut = it->as.sym == sl->t_mut; i++; }
        if (i >= len) break;
        uint32_t name = i++;
        if (i < len && v->as.list.items[i]->tag == F_TYPE_ANN) i++;
        if (i >= len) break;
        out[n].name = name; out[n].init = i++; out[n].mut = mut;
        n++;
    }
    return n;
}
static Form *ac_copy(SL *sl, const Form *f, Form **items) {
    Form *c = (Form *)arena_alloc(sl->a, sizeof(Form));
    *c = *f;
    c->as.list.items = items;
    return c;
}
static Form *ac_subst(SL *sl, const Symbol *n, Form *f);
/* `f` with items[from..] substituted; `f` itself when nothing changed. */
static Form *ac_subst_from(SL *sl, const Symbol *n, Form *f, uint32_t from) {
    uint32_t len = f->as.list.len;
    Form **ni = NULL;
    for (uint32_t i = from; i < len; i++) {
        Form *x = ac_subst(sl, n, f->as.list.items[i]);
        if (x != f->as.list.items[i] && !ni) {
            ni = (Form **)arena_alloc(sl->a, len * sizeof(Form *));
            memcpy(ni, f->as.list.items, len * sizeof(Form *));
        }
        if (ni) ni[i] = x;
    }
    return ni ? ac_copy(sl, f, ni) : f;
}
static Form *ac_subst(SL *sl, const Symbol *n, Form *f) {
    if (!f) return f;
    Span sp = f->span;
    if (f->tag == F_SYM)
        return f->as.sym == n ? Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-unbox__")), f) : f;
    if (f->tag != F_LIST && f->tag != F_VEC) return f;
    if (f->tag == F_LIST && ac_is_set(sl, f) && is_sym(f->as.list.items[1], n))
        return Ln(sl, sp, 3, Sym(sl, sp, I(sl, "r7rs-box-set!__")), f->as.list.items[1],
                  ac_subst(sl, n, f->as.list.items[2]));
    if (head_is(f, sl->t_fn) && f->as.list.len >= 2)
        return ac_vec_binds(f->as.list.items[1], n) ? f : ac_subst_from(sl, n, f, 2);
    if (head_is(f, sl->t_defn) && f->as.list.len >= 3)
        return ac_vec_binds(f->as.list.items[2], n) ? f : ac_subst_from(sl, n, f, 3);
    if (head_is(f, sl->t_letrec) && f->as.list.len >= 2)
        return ac_vec_binds(f->as.list.items[1], n) ? f : ac_subst_from(sl, n, f, 1);
    if (head_is(f, sl->t_let) && f->as.list.len >= 2 && f->as.list.items[1]->tag == F_VEC) {
        /* Sequential: each init sees the bindings before it, so the rewrite
         * runs through the inits and stops after the one that rebinds `n`. */
        Form *v = f->as.list.items[1];
        AcBind *bs = (AcBind *)arena_alloc(sl->a, (v->as.list.len + 1) * sizeof(AcBind));
        uint32_t nb = ac_parse_binds(sl, v, bs);
        Form **vi = (Form **)arena_alloc(sl->a, (v->as.list.len + 1) * sizeof(Form *));
        memcpy(vi, v->as.list.items, v->as.list.len * sizeof(Form *));
        bool shadowed = false;
        for (uint32_t k = 0; k < nb && !shadowed; k++) {
            vi[bs[k].init] = ac_subst(sl, n, v->as.list.items[bs[k].init]);
            if (is_sym(v->as.list.items[bs[k].name], n)) shadowed = true;
        }
        Form **li = (Form **)arena_alloc(sl->a, f->as.list.len * sizeof(Form *));
        memcpy(li, f->as.list.items, f->as.list.len * sizeof(Form *));
        li[1] = ac_copy(sl, v, vi);
        if (!shadowed)
            for (uint32_t i = 2; i < f->as.list.len; i++) li[i] = ac_subst(sl, n, f->as.list.items[i]);
        return ac_copy(sl, f, li);
    }
    return ac_subst_from(sl, n, f, 0);
}
/* Convert binding number `k` of the `let` form `f` (already walked). */
static Form *ac_convert(SL *sl, Form *f, uint32_t k) {
    Form *v = f->as.list.items[1];
    uint32_t vlen = v->as.list.len;
    AcBind *bs = (AcBind *)arena_alloc(sl->a, (vlen + 1) * sizeof(AcBind));
    uint32_t nb = ac_parse_binds(sl, v, bs);
    const Symbol *n = v->as.list.items[bs[k].name]->as.sym;
    Span sp = v->as.list.items[bs[k].name]->span;
    FB nv = {0};
    bool shadowed = false;
    for (uint32_t j = 0; j < nb; j++) {
        uint32_t start = j == 0 ? 0 : bs[j - 1].init + 1;
        if (j == k) {
            fb_push(&nv, v->as.list.items[bs[j].name]);
            fb_push(&nv, Ln(sl, sp, 2, Sym(sl, sp, I(sl, "r7rs-box__")), v->as.list.items[bs[j].init]));
            continue;
        }
        for (uint32_t i = start; i < bs[j].init; i++) fb_push(&nv, v->as.list.items[i]);
        Form *init = v->as.list.items[bs[j].init];
        fb_push(&nv, (j > k && !shadowed) ? ac_subst(sl, n, init) : init);
        if (j > k && is_sym(v->as.list.items[bs[j].name], n)) shadowed = true;
    }
    Form **li = (Form **)arena_alloc(sl->a, f->as.list.len * sizeof(Form *));
    memcpy(li, f->as.list.items, f->as.list.len * sizeof(Form *));
    li[1] = fb_vec(sl, &nv, v->span);
    if (!shadowed)
        for (uint32_t i = 2; i < f->as.list.len; i++) li[i] = ac_subst(sl, n, f->as.list.items[i]);
    return ac_copy(sl, f, li);
}
static Form *ac_walk(SL *sl, Form *f) {
    if (!f || (f->tag != F_LIST && f->tag != F_VEC)) return f;
    uint32_t len = f->as.list.len;
    Form **ni = NULL;
    for (uint32_t i = 0; i < len; i++) {
        Form *x = ac_walk(sl, f->as.list.items[i]);
        if (x != f->as.list.items[i] && !ni) {
            ni = (Form **)arena_alloc(sl->a, len * sizeof(Form *));
            memcpy(ni, f->as.list.items, len * sizeof(Form *));
        }
        if (ni) ni[i] = x;
    }
    Form *g = ni ? ac_copy(sl, f, ni) : f;
    if (!(head_is(g, sl->t_let) && len >= 2 && g->as.list.items[1]->tag == F_VEC)) return g;
    uint32_t vlen = g->as.list.items[1]->as.list.len;
    AcBind *bs = (AcBind *)arena_alloc(sl->a, (vlen + 1) * sizeof(AcBind));
    uint32_t nb = ac_parse_binds(sl, g->as.list.items[1], bs);
    for (uint32_t k = 0; k < nb; k++) {
        /* Re-parse: a conversion drops the `^mut` marker and the annotation,
         * which moves the later bindings' indices (never their ordinals). */
        Form *v = g->as.list.items[1];
        nb = ac_parse_binds(sl, v, bs);
        if (k >= nb || !bs[k].mut) continue;
        g = ac_convert(sl, g, k);
    }
    return g;
}

/* R10: the names the stdlib forms ahead of the program define -- every
 * `defn`/`def`/`defmacro` at the top of a non-Scheme form or directly inside
 * its `defmodule`, and every type a form there names: a `defstruct`'s,
 * a `defdata`'s and its constructors, a `defclass`'s and its methods.  A
 * program global spelled like any of them would take the stdlib's name
 * (`(define (Some x) ...)`, `(define (Vec x) ...)`). */
static void stdlib_names_of(SL *sl, const Form *f, FB *out, int depth) {
    if (!f || f->tag != F_LIST || f->as.list.len < 2 || f->as.list.items[0]->tag != F_SYM) return;
    const char *h = f->as.list.items[0]->as.sym->name;
    if (strcmp(h, "defmodule") == 0 && depth == 0) {
        for (uint32_t i = 2; i < f->as.list.len; i++) stdlib_names_of(sl, f->as.list.items[i], out, 1);
        return;
    }
    static const char *const VALUE_HEADS[] = { "defn", "def", "defmacro", "defdynamic" };
    static const char *const TYPE_HEADS[] = { "defstruct", "defopaque", "deftype", "defeffect",
                                              "defdata", "defgadt", "defclass" };
    bool value = false, type = false, members = false;
    for (size_t k = 0; k < sizeof VALUE_HEADS / sizeof *VALUE_HEADS; k++) value |= strcmp(h, VALUE_HEADS[k]) == 0;
    for (size_t k = 0; k < sizeof TYPE_HEADS / sizeof *TYPE_HEADS; k++) type |= strcmp(h, TYPE_HEADS[k]) == 0;
    if (!value && !type) return;
    /* A `defdata`/`defgadt`'s constructors and a `defclass`'s methods are
     * globals too: `(defdata Option [A] (None) (Some A))`. */
    members = !strcmp(h, "defdata") || !strcmp(h, "defgadt") || !strcmp(h, "defclass");
    uint32_t i = 1;
    for (; i < f->as.list.len; i++) {
        const Form *x = f->as.list.items[i];
        if (x->tag != F_SYM) continue;
        if (x->as.sym->name[0] == '^') continue;
        fb_push(out, (Form *)x);
        break;
    }
    if (!members) return;
    for (i++; i < f->as.list.len; i++) {
        const Form *c = f->as.list.items[i];
        if (c->tag == F_LIST && c->as.list.len >= 1 && c->as.list.items[0]->tag == F_SYM)
            fb_push(out, c->as.list.items[0]);
    }
}
static void user_define_names(SL *sl, const Form *f, FB *out) {
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) user_define_names(sl, f->as.list.items[i], out);
        return;
    }
    if (head_is(f, sl->s_define_library)) {
        for (uint32_t i = 2; i < f->as.list.len; i++)
            if (head_is(f->as.list.items[i], sl->s_begin)) user_define_names(sl, f->as.list.items[i], out);
        return;
    }
    if (!head_is(f, sl->s_define) || f->as.list.len < 2) return;
    Form *t = f->as.list.items[1];
    while (t->tag == F_LIST && t->as.list.len >= 1) t = t->as.list.items[0];  /* (define ((f a) b) ...) */
    if (t->tag == F_SYM) fb_push(out, t);
}
/* r7rs-program-redefinition-refused: R7RS 5.3.1 -- at the outermost level of
 * a program, a definition of a variable that is already bound "has
 * essentially the same effect as the assignment expression set!".  SICP
 * redefines procedures as it refines them (make-rat in 2.1.1, then again in
 * 2.1.2), and the elaborator refuses a second `defn` of one name.  So a name
 * a program's top level defines more than once is one variable: its first
 * definition becomes `(define f (lambda ...))` and every later one a
 * `(set! f ...)`, which collect_muts then sees.  A library body is left
 * alone (a duplicate definition there is an error, R7RS 5.6.1). */
static bool srfi_span(Span sp);
typedef struct { Form **slot; const Symbol *name; } TopDef;
static void top_defs(SL *sl, Form **slot, TopDef **defs, uint32_t *n, uint32_t *cap) {
    Form *f = *slot;
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++) top_defs(sl, &f->as.list.items[i], defs, n, cap);
        return;
    }
    if (!head_is(f, sl->s_define) || f->as.list.len < 3) return;
    Form *t = f->as.list.items[1];
    const Symbol *name = NULL;
    if (t->tag == F_SYM) name = t->as.sym;
    else if (t->tag == F_LIST && t->as.list.len >= 1 && t->as.list.items[0]->tag == F_SYM) name = t->as.list.items[0]->as.sym;
    if (!name) return;   /* (define ((f a) b) ...) keeps its one meaning */
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 32;
        *defs = (TopDef *)realloc(*defs, *cap * sizeof **defs);
        if (!*defs) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    (*defs)[(*n)++] = (TopDef){ slot, name };
}
/* `(define (f . formals) body...)` -> `(lambda formals body...)`; `(define f
 * e)` -> `e`. */
static Form *define_value(SL *sl, Form *def) {
    Form *t = def->as.list.items[1];
    Span sp = def->span;
    if (t->tag == F_SYM) return def->as.list.items[2];
    Form *formals;
    if (t->as.list.len == 3 && is_sym(t->as.list.items[1], sl->s_dot)) {
        formals = t->as.list.items[2];
    } else {
        formals = List(sl, t->span, t->as.list.items + 1, t->as.list.len - 1);
    }
    uint32_t nb = def->as.list.len - 2;
    Form **items = (Form **)arena_alloc(sl->a, (nb + 2) * sizeof(Form *));
    items[0] = Sym(sl, sp, sl->s_lambda);
    items[1] = formals;
    for (uint32_t i = 0; i < nb; i++) items[2 + i] = def->as.list.items[2 + i];
    return List(sl, sp, items, nb + 2);
}
/* r7rs-saved-standard-procedure-follows-redefinition: a program that defines
 * a standard procedure's name -- SICP 4.1's `apply`, 2.1.3's `cons` -- gets
 * the same treatment, with one more definition in front: the variable
 * starts out holding the standard procedure, so an expression evaluated
 * before the program's define (`(define apply-in-underlying-scheme apply)`)
 * gets that, and one evaluated after gets the program's.  The standard
 * binding is named `--std--<name>`, which rn_global resolves past the
 * program's respelling.  Returns the forms, with those definitions inserted
 * after the leading imports (a copy when there are any), and their count. */
#define STD_BINDING_PREFIX "--std--"
static bool is_std_procedure_name(SL *sl, const Symbol *s) {
    for (size_t i = 0; i < N_RENAMES; i++) if (sl->rn_from[i] == s) return true;
    /* An on-demand library's procedure, when the unit imports the library
     * (mark_ondemand_imports has run): `(scheme eval)`'s `eval`. */
    for (size_t i = 0; i < N_ONDEMAND; i++)
        if (sl->od_from[i] == s && sl->od_lib[i] >= 0 && sl->lib_imported[sl->od_lib[i]]) return true;
    return false;
}
/* r7rs-redefining-eval-with-scheme-eval-fails-to-compile: which on-demand
 * libraries the unit imports is known before any form is lowered, so the
 * passes that decide what a program's own definitions shadow --
 * redefinitions_to_set and note_stdlib_clashes -- see `(scheme eval)`'s
 * `eval` as a standard name, as they see `(scheme base)`'s.  The import
 * lowering sets the same flags again later. */
static void mark_ondemand_set(SL *sl, const Form *set) {
    while (set && set->tag == F_LIST && set->as.list.len >= 2 && set->as.list.items[0]->tag == F_SYM &&
           (!strcmp(set->as.list.items[0]->as.sym->name, "only") || !strcmp(set->as.list.items[0]->as.sym->name, "except") ||
            !strcmp(set->as.list.items[0]->as.sym->name, "prefix") || !strcmp(set->as.list.items[0]->as.sym->name, "rename")))
        set = set->as.list.items[1];
    int li = scheme_lib_index(set);
    if (li >= 0 && SCHEME_LIBS[li].kind == LIB_ONDEMAND) sl->lib_imported[li] = true;
}
static void mark_ondemand_imports(SL *sl, Form *const *forms, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const Form *f = forms[i];
        if (!is_scheme_file(f) || prelude_span(f->span) || srfi_span(f->span)) continue;
        if (head_is(f, sl->s_import))
            for (uint32_t j = 1; j < f->as.list.len; j++) mark_ondemand_set(sl, f->as.list.items[j]);
    }
}
static Form **redefinitions_to_set(SL *sl, Form **forms, uint32_t n, uint32_t *out_n) {
    TopDef *defs = NULL;
    uint32_t nd = 0, cap = 0;
    *out_n = n;
    if (sl->repl_turn) return forms;   /* a REPL turn redefines through its own session state */
    for (uint32_t i = 0; i < n; i++) {
        if (!is_scheme_file(forms[i]) || prelude_span(forms[i]->span) || srfi_span(forms[i]->span)) continue;
        top_defs(sl, &forms[i], &defs, &nd, &cap);
    }
    FB std_inits = {0};
    for (uint32_t i = 0; i < nd; i++) {
        bool first = true, again = false;
        for (uint32_t j = 0; j < i; j++) if (defs[j].name == defs[i].name) first = false;
        if (!first) continue;
        for (uint32_t j = i + 1; j < nd; j++) if (defs[j].name == defs[i].name) again = true;
        bool std = is_std_procedure_name(sl, defs[i].name);
        if (!again && !std) continue;
        for (uint32_t j = i; j < nd; j++) {
            if (defs[j].name != defs[i].name) continue;
            Form *def = *defs[j].slot;
            Span sp = def->span;
            Form *head = Sym(sl, sp, (j == i && !std) ? sl->s_define : sl->s_set);
            *defs[j].slot = Ln(sl, sp, 3, head, Sym(sl, sp, defs[i].name), define_value(sl, def));
        }
        if (std) {
            Span sp = (*defs[i].slot)->span;
            char buf[256];
            snprintf(buf, sizeof buf, STD_BINDING_PREFIX "%s", defs[i].name->name);
            fb_push(&std_inits, Ln(sl, sp, 3, Sym(sl, sp, sl->s_define), Sym(sl, sp, defs[i].name),
                                   Sym(sl, sp, I(sl, buf))));
        }
    }
    free(defs);
    if (std_inits.n == 0) return forms;
    /* After the program's imports: before its first other form. */
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        at = i;
        if (!is_scheme_file(forms[i]) || prelude_span(forms[i]->span) || srfi_span(forms[i]->span) ||
            head_is(forms[i], sl->s_import)) { at = i + 1; continue; }
        break;
    }
    Form **out = (Form **)arena_alloc(sl->a, (n + std_inits.n) * sizeof(Form *));
    uint32_t k = 0;
    for (uint32_t i = 0; i < at; i++) out[k++] = forms[i];
    for (uint32_t i = 0; i < std_inits.n; i++) out[k++] = std_inits.items[i];
    for (uint32_t i = at; i < n; i++) out[k++] = forms[i];
    free(std_inits.items);
    *out_n = k;
    return out;
}
/* R10: every identifier the user's Scheme code BINDS -- formals, `let`-family
 * and `do` variables, named-let names, `guard` variables.  One named like a
 * Turmeric special form (`return`, `handle`, `perform`, `resume`, ...) was
 * elaborated AS that form wherever it headed a call: chibi's
 * `(call/cc (lambda (return) ... (return #f)))` compiled to an early return
 * (invalid C) and returned #f into a `+` on the interpreter. */
static void binders_of_formals(const Form *f, FB *out) {
    if (!f) return;
    if (f->tag == F_SYM) { fb_push(out, (Form *)f); return; }
    if (f->tag == F_LIST || f->tag == F_VEC)
        for (uint32_t i = 0; i < f->as.list.len; i++)
            if (f->as.list.items[i]->tag == F_SYM) fb_push(out, f->as.list.items[i]);
}
static void binders_of_bindings(const Form *b, FB *out) {
    if (!b || b->tag != F_LIST) return;
    for (uint32_t i = 0; i < b->as.list.len; i++) {
        const Form *x = b->as.list.items[i];
        if (x->tag == F_LIST && x->as.list.len >= 1) binders_of_formals(x->as.list.items[0], out);
    }
}
static void user_binders(SL *sl, const Form *f, FB *out) {
    if (!f || f->tag == F_QUOTE) return;
    if (f->tag != F_LIST && f->tag != F_VEC) return;
    uint32_t len = f->as.list.len;
    if (f->tag == F_LIST && len >= 2) {
        const Form *h = f->as.list.items[0];
        if (is_sym(h, sl->s_lambda)) binders_of_formals(f->as.list.items[1], out);
        else if (is_sym(h, sl->s_define) && f->as.list.items[1]->tag == F_LIST)
            binders_of_formals(f->as.list.items[1], out);
        else if (is_sym(h, sl->s_let) || is_sym(h, sl->s_letstar) || is_sym(h, sl->s_letrec) ||
                 is_sym(h, sl->s_letrecstar) || is_sym(h, sl->s_do) ||
                 is_sym(h, sl->s_let_values) || is_sym(h, sl->s_letstar_values)) {
            const Form *b = f->as.list.items[1];
            if (b->tag == F_SYM && len >= 3) { fb_push(out, (Form *)b); b = f->as.list.items[2]; }
            binders_of_bindings(b, out);
        } else if (is_sym(h, sl->s_case_lambda)) {
            for (uint32_t i = 1; i < len; i++)
                if (f->as.list.items[i]->tag == F_LIST && f->as.list.items[i]->as.list.len >= 1)
                    binders_of_formals(f->as.list.items[i]->as.list.items[0], out);
        } else if (is_sym(h, sl->s_guard) && f->as.list.items[1]->tag == F_LIST &&
                   f->as.list.items[1]->as.list.len >= 1 &&
                   f->as.list.items[1]->as.list.items[0]->tag == F_SYM) {
            fb_push(out, f->as.list.items[1]->as.list.items[0]);
        }
    }
    for (uint32_t i = 0; i < len; i++) user_binders(sl, f->as.list.items[i], out);
}
/* ---------------------------------------------------------------------------
 * r7rs-callcc-memory-never-freed: escape-only `call/cc`.
 *
 * `(call/cc (lambda (k) body...))` copies the stack so that `k` can be
 * invoked after the call/cc has returned.  When the body cannot let `k` out,
 * every invocation of `k` happens while the call/cc is still running -- an
 * escape -- and `r7rs-call/ec__`, the one-shot escape `guard` uses, gives the
 * same answers with no stack copy and, under the interpreter, no pin.
 *
 * `k` cannot get out when every occurrence of it is the operator of a call,
 * and every closure it occurs in cannot get out either.  A closure is let
 * through in two places only:
 *   - a `lambda` written as an argument of a standard procedure that calls
 *     its procedure arguments and keeps none of them (`for-each`, `map`,
 *     their vector and string twins, and `call/cc` itself), which is the
 *     `(for-each (lambda (x) (if (p x) (return x))) l)` idiom;
 *   - a named `let` whose name, too, only ever heads a call, which is the
 *     `(let scan ((l l)) ... (return x) ... (scan (cdr l)))` idiom.
 * Anything else that makes a closure over `k` -- a lambda stored, returned or
 * passed elsewhere, `delay`, `case-lambda`, an internal `define`, a macro use
 * or quasiquote mentioning it -- keeps the copying call/cc.  So does any
 * rebinding of `k`, or of one of those procedure names, inside the body.
 *
 * A continuation captured INSIDE the body and re-entered later restores the
 * escape as live along with the frames it copies (the capture's saved state
 * includes the live escapes, on both back ends), so `k` still works there.
 * `dynamic-wind` is deliberately not in the list: its `before` thunk runs
 * again on a re-entry BEFORE the stack is restored, where the escape is not
 * live yet.
 * ------------------------------------------------------------------------- */
static bool fb_has_sym(const FB *b, const Symbol *s);
static bool cc_mentions(const Form *f, const Symbol *k) {
    if (!f) return false;
    if (f->tag == F_SYM) return f->as.sym == k;
    if (f->tag == F_QUOTE) return false;
    switch (f->tag) {
        case F_LIST: case F_VEC: case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING:
        case F_MAP: case F_SET: case F_MAP_LITERAL: case F_SET_LITERAL: case F_TYPE_ANN:
            for (uint32_t i = 0; i < f->as.list.len; i++)
                if (cc_mentions(f->as.list.items[i], k)) return true;
            return false;
        default: return false;
    }
}
typedef struct { SL *sl; const Symbol *k; FB binders; } CcScan;
static bool cc_bound_inside(const CcScan *c, const Symbol *s) { return fb_has_sym(&c->binders, s); }
/* A head that names one of the non-retaining standard procedures. */
static bool cc_nonretaining_head(const CcScan *c, const Form *h) {
    if (!h || h->tag != F_SYM || cc_bound_inside(c, h->as.sym)) return false;
    static const char *const names[] = {
        "r7rs-for-each", "r7rs-map", "r7rs-vector-for-each", "r7rs-vector-map",
        "r7rs-string-for-each", "r7rs-string-map", "r7rs-call/cc",
    };
    const Symbol *t = rn(c->sl, h->as.sym);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(t->name, names[i]) == 0) return true;
    return false;
}
static bool cc_ok(CcScan *c, const Form *f);
static bool cc_ok_from(CcScan *c, const Form *f, uint32_t from) {
    for (uint32_t i = from; i < f->as.list.len; i++)
        if (!cc_ok(c, f->as.list.items[i])) return false;
    return true;
}
/* A `lambda` in a position that does not let it out: its body is checked as
 * if written in place. */
static bool cc_ok_lambda_body(CcScan *c, const Form *lam) {
    if (lam->as.list.len < 3) return false;
    return cc_ok_from(c, lam, 2);
}
static bool cc_is_lambda(const CcScan *c, const Form *f) {
    return f && f->tag == F_LIST && f->as.list.len >= 3 && f->as.list.items[0]->tag == F_SYM &&
           f->as.list.items[0]->as.sym == c->sl->s_lambda && !sr_lookup(c->sl, c->sl->s_lambda);
}
static bool cc_ok(CcScan *c, const Form *f) {
    SL *sl = c->sl;
    const Symbol *k = c->k;
    if (!f) return true;
    if (f->tag == F_SYM) return f->as.sym != k;          /* k as a value: it can get out */
    if (f->tag == F_QUOTE) return true;
    if (f->tag != F_LIST) return !cc_mentions(f, k);
    if (f->as.list.len == 0) return true;
    if (!cc_mentions(f, k)) return true;                  /* nothing here can reach k */
    const Form *h = f->as.list.items[0];
    if (h->tag == F_SYM && h->as.sym == k) return cc_ok_from(c, f, 1);   /* (k args...) */
    if (h->tag == F_SYM && !cc_bound_inside(c, h->as.sym)) {
        const Symbol *s = h->as.sym;
        if (sr_lookup(sl, s)) return false;               /* a macro use: opaque */
        if (s == sl->s_quote) return true;
        if (s == sl->s_if || s == sl->s_begin || s == sl->s_when || s == sl->s_unless ||
            s == sl->s_and || s == sl->s_or)
            return cc_ok_from(c, f, 1);
        if (s == sl->s_set)
            return f->as.list.len == 3 && f->as.list.items[1]->tag == F_SYM &&
                   f->as.list.items[1]->as.sym != k && cc_ok(c, f->as.list.items[2]);
        if (s == sl->s_cond || s == sl->s_guard || s == sl->s_case) {
            /* cond clauses; a guard's (var clause...) and body; a case's key
             * and clauses, whose data lists are data. */
            uint32_t i = 1;
            if (s == sl->s_case) { if (f->as.list.len < 2 || !cc_ok(c, f->as.list.items[1])) return false; i = 2; }
            if (s == sl->s_guard) {
                const Form *spec = f->as.list.len >= 2 ? f->as.list.items[1] : NULL;
                if (!spec || spec->tag != F_LIST || spec->as.list.len < 1) return false;
                for (uint32_t j = 1; j < spec->as.list.len; j++) {
                    const Form *cl = spec->as.list.items[j];
                    if (cl->tag != F_LIST || !cc_ok_from(c, cl, 0)) return false;
                }
                return cc_ok_from(c, f, 2);
            }
            for (; i < f->as.list.len; i++) {
                const Form *cl = f->as.list.items[i];
                if (cl->tag != F_LIST) return false;
                if (!cc_ok_from(c, cl, s == sl->s_case ? 1 : 0)) return false;
            }
            return true;
        }
        if (s == sl->s_let || s == sl->s_letstar || s == sl->s_letrec || s == sl->s_letrecstar ||
            s == sl->s_let_values || s == sl->s_letstar_values || s == sl->s_parameterize) {
            if (f->as.list.len < 3) return false;
            uint32_t bi = 1;
            const Form *name = NULL;
            if (s == sl->s_let && f->as.list.items[1]->tag == F_SYM) { name = f->as.list.items[1]; bi = 2; }
            const Form *b = f->as.list.items[bi];
            if (b->tag != F_LIST && b->tag != F_NIL) return false;
            for (uint32_t j = 0; b->tag == F_LIST && j < b->as.list.len; j++) {
                const Form *pair = b->as.list.items[j];
                if (pair->tag != F_LIST || pair->as.list.len < 1) return false;
                /* (v init), ((formals) init), (param value) */
                if (s == sl->s_parameterize ? !cc_ok(c, pair->as.list.items[0])
                                            : cc_mentions(pair->as.list.items[0], k))
                    return false;
                if (!cc_ok_from(c, pair, 1)) return false;
            }
            if (name) {
                /* The loop procedure closes over the body: it must not get
                 * out either, so its name may only head calls. */
                if (name->as.sym == k) return false;
                CcScan inner = { sl, name->as.sym, c->binders };
                if (!cc_ok_from(&inner, f, bi + 1)) return false;
            }
            return cc_ok_from(c, f, bi + 1);
        }
        if (s == sl->s_do) {
            /* (do ((var init step)...) (test expr...) command...) */
            if (f->as.list.len < 3) return false;
            const Form *b = f->as.list.items[1], *t = f->as.list.items[2];
            if ((b->tag != F_LIST && b->tag != F_NIL) || (t->tag != F_LIST && t->tag != F_NIL)) return false;
            for (uint32_t j = 0; b->tag == F_LIST && j < b->as.list.len; j++) {
                const Form *v = b->as.list.items[j];
                if (v->tag != F_LIST || v->as.list.len < 2 || cc_mentions(v->as.list.items[0], k)) return false;
                if (!cc_ok_from(c, v, 1)) return false;
            }
            if (t->tag == F_LIST && !cc_ok_from(c, t, 0)) return false;
            return cc_ok_from(c, f, 3);
        }
        if (is_scheme_syntax_name(s->name)) return false;  /* lambda, delay, define, ... */
    }
    /* An application.  The operator is an expression like any other; a
     * lambda argument is let through only for a non-retaining callee. */
    if (!cc_ok(c, h)) return false;
    bool keeps_none = cc_nonretaining_head(c, h);
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        const Form *a = f->as.list.items[i];
        if (keeps_none && cc_is_lambda(c, a)) {
            if (cc_mentions(a->as.list.items[1], k)) return false;   /* rebinds k */
            if (!cc_ok_lambda_body(c, a)) return false;
        } else if (!cc_ok(c, a)) {
            return false;
        }
    }
    return true;
}
/* Every name the body binds, variable defines included. */
static void cc_binders(SL *sl, const Form *f, FB *out) {
    user_binders(sl, f, out);
    if (!f || f->tag != F_LIST) return;
    for (uint32_t i = 0; i < f->as.list.len; i++) {
        const Form *x = f->as.list.items[i];
        if (x && x->tag == F_LIST && x->as.list.len >= 2 && is_sym(x->as.list.items[0], sl->s_define) &&
            x->as.list.items[1]->tag == F_SYM)
            fb_push(out, x->as.list.items[1]);
        if (x && x->tag == F_LIST) cc_binders(sl, x, out);
    }
}
/* Is `call` -- already known to be a list with a symbol head -- a
 * `(call/cc (lambda (k) body...))` whose `k` never outlives it? */
static bool callcc_escape_only(SL *sl, const Form *call) {
    if (call->as.list.len != 2) return false;
    const Form *h = call->as.list.items[0];
    if (h->tag != F_SYM || strcmp(rn(sl, h->as.sym)->name, "r7rs-call/cc") != 0) return false;
    const Form *lam = call->as.list.items[1];
    CcScan c = { sl, NULL, {0} };
    if (!cc_is_lambda(&c, lam)) return false;
    const Form *formals = lam->as.list.items[1];
    if (formals->tag != F_LIST || formals->as.list.len != 1 || formals->as.list.items[0]->tag != F_SYM)
        return false;
    c.k = formals->as.list.items[0]->as.sym;
    for (uint32_t i = 2; i < lam->as.list.len; i++) cc_binders(sl, lam->as.list.items[i], &c.binders);
    bool ok = !cc_bound_inside(&c, c.k);
    for (uint32_t i = 2; ok && i < lam->as.list.len; i++) {
        const Form *x = lam->as.list.items[i];
        /* An internal definition closes over k: keep the copying call/cc. */
        if (x->tag == F_LIST && x->as.list.len >= 1 && x->as.list.items[0]->tag == F_SYM &&
            is_scheme_syntax_name(x->as.list.items[0]->as.sym->name) &&
            strncmp(x->as.list.items[0]->as.sym->name, "define", 6) == 0 && cc_mentions(x, c.k))
            ok = false;
        else ok = cc_ok(&c, x);
    }
    free(c.binders.items);
    return ok;
}

/* The names an `(import ...)` binds by spelling them: an `(only ...)` list
 * and the new names of a `(rename ...)`.  A global spelled like a Turmeric
 * special form (`gen`, `handle`, `return`, ...) that the user defines or
 * imports this way is the user's own binding, and is renamed wherever it
 * occurs -- the definition, its uses, a library's export of it, the
 * importer's `only` -- so a library and its importer stay in step
 * (r7rs-toplevel-define-named-like-a-turmeric-form).  A Turmeric form
 * written in a Scheme file (r7rs-elaborates-as-saffron) stays reachable,
 * since nothing defines or imports its name. */
static bool is_scheme_syntax_name(const char *name) {
    /* The heads this lowering matches itself (R7RS 4.1-4.3, 5, 7.3): a
     * spelling shared with a Turmeric form (`define`, `let`, `if`, `do`,
     * `set!`, `case`, `quote`, `import`, `export`) is Scheme syntax here,
     * matched after the import-rename step, so it must not be renamed. */
    static const char *const syntax[] = {
        "define", "lambda", "let", "let*", "letrec", "letrec*", "do", "begin",
        "set!", "if", "cond", "case", "and", "or", "when", "unless",
        "case-lambda", "define-values", "let-values", "let*-values",
        "define-syntax", "let-syntax", "letrec-syntax", "syntax-rules",
        "syntax-error", "er-macro-transformer", "import", "export",
        "define-library", "define-record-type", "include", "include-ci",
        "cond-expand", "quote", "quasiquote", "unquote", "unquote-splicing",
        "guard", "parameterize", "delay", "delay-force", "make-promise",
    };
    for (size_t i = 0; i < sizeof syntax / sizeof syntax[0]; i++)
        if (strcmp(name, syntax[i]) == 0) return true;
    return false;
}
/* The names one import set spells, through any nesting: an `only` list,
 * both sides of a `rename` pair (the library's spelling, which the library
 * renamed the same way, and the new name). */
static const Symbol *import_set_prefix(const Form *set) {
    if (set->tag != F_LIST || set->as.list.len < 2 || set->as.list.items[0]->tag != F_SYM) return NULL;
    const char *h = set->as.list.items[0]->as.sym->name;
    if (strcmp(h, "prefix") == 0 && set->as.list.len == 3 && set->as.list.items[2]->tag == F_SYM)
        return set->as.list.items[2]->as.sym;
    if (strcmp(h, "only") == 0 || strcmp(h, "except") == 0 || strcmp(h, "rename") == 0)
        return import_set_prefix(set->as.list.items[1]);
    return NULL;
}
/* A name written outside a `prefix` names the library's `name` less the
 * prefix; push both spellings, since the library's is the one that clashes. */
static void push_name_and_unprefixed(SL *sl, Form *nm, const Symbol *pfx, FB *out) {
    fb_push(out, nm);
    if (pfx && nm->as.sym->len > pfx->len && memcmp(nm->as.sym->name, pfx->name, pfx->len) == 0)
        fb_push(out, Sym(sl, nm->span, I(sl, nm->as.sym->name + pfx->len)));
}
static void import_set_bound_names(SL *sl, const Form *set, FB *out) {
    if (set->tag != F_LIST || set->as.list.len < 2 || set->as.list.items[0]->tag != F_SYM) return;
    const char *h = set->as.list.items[0]->as.sym->name;
    bool mod = strcmp(h, "only") == 0 || strcmp(h, "except") == 0 || strcmp(h, "prefix") == 0 ||
               strcmp(h, "rename") == 0;
    if (!mod) return;
    import_set_bound_names(sl, set->as.list.items[1], out);
    const Symbol *pfx = import_set_prefix(set->as.list.items[1]);
    if (strcmp(h, "only") == 0 || strcmp(h, "except") == 0) {
        for (uint32_t k = 2; k < set->as.list.len; k++)
            if (set->as.list.items[k]->tag == F_SYM) push_name_and_unprefixed(sl, set->as.list.items[k], pfx, out);
    } else if (strcmp(h, "rename") == 0) {
        for (uint32_t k = 2; k < set->as.list.len; k++) {
            const Form *pr = set->as.list.items[k];
            if (pr->tag == F_LIST && pr->as.list.len == 2) {
                if (pr->as.list.items[0]->tag == F_SYM) push_name_and_unprefixed(sl, pr->as.list.items[0], pfx, out);
                if (pr->as.list.items[1]->tag == F_SYM) fb_push(out, pr->as.list.items[1]);
            }
        }
    }
}
static void import_bound_names(SL *sl, const Form *f, FB *out) {
    if (head_is(f, sl->s_define_library)) {
        for (uint32_t i = 2; i < f->as.list.len; i++) import_bound_names(sl, f->as.list.items[i], out);
        return;
    }
    if (!head_is(f, sl->s_import)) return;
    for (uint32_t i = 1; i < f->as.list.len; i++) import_set_bound_names(sl, f->as.list.items[i], out);
}
/* `(rename internal public)` in an `(export ...)` declaration. */
static bool is_export_rename(SL *sl, const Form *nm) {
    return nm->tag == F_LIST && nm->as.list.len == 3 && is_sym(nm->as.list.items[0], I(sl, "rename"));
}
/* The public names a define-library exports under `(rename a b)`: each is a
 * global the library defines by spelling it, for the clash table's purposes. */
static void export_rename_names(SL *sl, const Form *f, FB *out) {
    if (!head_is(f, sl->s_define_library)) return;
    for (uint32_t i = 2; i < f->as.list.len; i++) {
        const Form *d = f->as.list.items[i];
        if (!head_is(d, sl->s_export)) continue;
        for (uint32_t j = 1; j < d->as.list.len; j++) {
            Form *nm = d->as.list.items[j];
            if (is_export_rename(sl, nm) && nm->as.list.items[2]->tag == F_SYM) fb_push(out, nm->as.list.items[2]);
        }
    }
}
/* The globals a define-library's body defines by spelling them: `define`,
 * `define-values` and `define-record-type` in its `(begin ...)` declarations
 * (an `(include ...)` is one by now) and in a `cond-expand`'s chosen clause.
 * A definition a macro use expands to is not seen; what that costs is said
 * where this is read. */
static void body_defined_names(SL *sl, const Form *f, FB *out) {
    for (uint32_t i = 1; i < f->as.list.len; i++) {
        const Form *d = f->as.list.items[i];
        if (head_is(d, sl->s_begin)) body_defined_names(sl, d, out);
        else if (head_is(d, sl->s_define)) user_define_names(sl, d, out);
        else if (head_is(d, sl->s_define_values) && d->as.list.len >= 2) {
            const Form *fm = d->as.list.items[1];
            if (fm->tag == F_SYM) fb_push(out, (Form *)fm);
            else if (fm->tag == F_LIST)
                for (uint32_t k = 0; k < fm->as.list.len; k++)
                    if (fm->as.list.items[k]->tag == F_SYM && !is_sym(fm->as.list.items[k], sl->s_dot))
                        fb_push(out, fm->as.list.items[k]);
        } else if (head_is(d, sl->s_define_record_type) && d->as.list.len >= 4) {
            const Form *ctor = d->as.list.items[2];
            if (ctor->tag == F_LIST && ctor->as.list.len >= 1 && ctor->as.list.items[0]->tag == F_SYM)
                fb_push(out, ctor->as.list.items[0]);
            if (d->as.list.items[3]->tag == F_SYM) fb_push(out, d->as.list.items[3]);
            for (uint32_t k = 4; k < d->as.list.len; k++) {
                const Form *spec = d->as.list.items[k];
                if (spec->tag != F_LIST) continue;
                for (uint32_t m = 1; m < spec->as.list.len; m++)
                    if (spec->as.list.items[m]->tag == F_SYM) fb_push(out, spec->as.list.items[m]);
            }
        }
    }
}
static void library_defined_names(SL *sl, Form *f, FB *out) {
    for (uint32_t i = 2; i < f->as.list.len; i++) {
        Form *d = f->as.list.items[i];
        if (head_is(d, sl->s_begin)) body_defined_names(sl, d, out);
        else if (head_is(d, sl->s_cond_expand)) {
            uint32_t n; Form **items;
            if (cond_expand_clause(sl, d, &n, &items))
                for (uint32_t k = 0; k < n; k++)
                    if (head_is(items[k], sl->s_begin)) body_defined_names(sl, items[k], out);
        }
    }
}
/* ---------------------------------------------------------------------------
 * r7rs-define-library-cannot-export-syntax: a library's `syntax-rules` macros,
 * exported to its importers.
 *
 * A macro is expanded by this lowering, before any module is loaded, so it
 * cannot travel in the module's interface the way a procedure does.  Both
 * sides read it from the library's source instead, through one scan
 * (lib_scan) so they agree on every name:
 *
 *   - the library leaves an exported macro out of its module's exports (the
 *     module has no definition of it), and exports each of its own
 *     definitions a macro form names -- a helper the template calls -- under
 *     a hidden spelling, `<lib>--syntax--<name>` (lib_rewrite_exports);
 *   - an importer reads the library's file, renames every macro's name and
 *     every helper it names to those hidden spellings, registers the macros,
 *     then binds each exported one under the name the import set gives it,
 *     and imports the hidden helpers by name (lib_syntax_of,
 *     lib_syntax_import).
 *
 * So a template's free identifiers mean what they mean in the library,
 * whatever the importer defines (R7RS 4.3.2), and a helper the library does
 * not export stays out of the importer's namespace under its own name.
 * ------------------------------------------------------------------------- */
static void lib_syntax_forms(SL *sl, const Form *body, FB *out) {
    for (uint32_t i = 1; i < body->as.list.len; i++) {
        Form *d = body->as.list.items[i];
        if (head_is(d, sl->s_define_syntax)) fb_push(out, d);
        else if (head_is(d, sl->s_begin)) lib_syntax_forms(sl, d, out);
    }
}
static bool fb_has_sym(const FB *b, const Symbol *s) {
    for (uint32_t i = 0; i < b->n; i++) if (b->items[i]->tag == F_SYM && b->items[i]->as.sym == s) return true;
    return false;
}
static bool lib_is_macro(const LibScan *ls, const Symbol *s) {
    for (uint32_t i = 0; i < ls->syntax.n; i++) {
        const Form *d = ls->syntax.items[i];
        if (d->as.list.len >= 2 && d->as.list.items[1]->tag == F_SYM && d->as.list.items[1]->as.sym == s) return true;
    }
    return false;
}
/* Does `f` name `s` as code -- outside quote, and outside quasiquote except
 * under its unquotes? */
static bool form_names(const Form *f, const Symbol *s, int qq) {
    if (!f) return false;
    switch (f->tag) {
        case F_SYM: return qq == 0 && f->as.sym == s;
        case F_QUOTE: return false;
        case F_QUASIQUOTE: return form_names(f->as.list.items[0], s, qq + 1);
        case F_UNQUOTE: case F_UNQUOTE_SPLICING: return form_names(f->as.list.items[0], s, qq > 0 ? qq - 1 : 0);
        case F_LIST: case F_VEC:
            for (uint32_t i = 0; i < f->as.list.len; i++) if (form_names(f->as.list.items[i], s, qq)) return true;
            return false;
        default: return false;
    }
}
static void lib_scan(SL *sl, Form *deflib, LibScan *out) {
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        if (head_is(d, sl->s_begin)) lib_syntax_forms(sl, d, &out->syntax);
        else if (head_is(d, sl->s_cond_expand)) {
            /* A malformed clause is the library lowering's to report, once. */
            bool wf = true;
            for (uint32_t c = 1; c < d->as.list.len && wf; c++)
                wf = d->as.list.items[c]->tag == F_LIST && d->as.list.items[c]->as.list.len > 0;
            uint32_t n; Form **items;
            if (wf && cond_expand_clause(sl, d, &n, &items))
                for (uint32_t k = 0; k < n; k++)
                    if (head_is(items[k], sl->s_begin)) lib_syntax_forms(sl, items[k], &out->syntax);
        }
    }
    if (!out->syntax.n) return;
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        if (!head_is(d, sl->s_export)) continue;
        for (uint32_t j = 1; j < d->as.list.len; j++) {
            Form *nm = d->as.list.items[j];
            if (nm->tag == F_SYM && lib_is_macro(out, nm->as.sym)) {
                fb_push(&out->exp_pub, nm);
                fb_push(&out->exp_in, nm);
            } else if (is_export_rename(sl, nm) && nm->as.list.items[1]->tag == F_SYM &&
                       nm->as.list.items[2]->tag == F_SYM && lib_is_macro(out, nm->as.list.items[1]->as.sym)) {
                fb_push(&out->exp_pub, nm->as.list.items[2]);
                fb_push(&out->exp_in, nm->as.list.items[1]);
            }
        }
    }
    if (!out->exp_pub.n) return;
    FB defined = {0};
    library_defined_names(sl, deflib, &defined);
    for (uint32_t d = 0; d < defined.n; d++) {
        const Symbol *s = defined.items[d]->as.sym;
        if (fb_has_sym(&out->helpers, s) || lib_is_macro(out, s)) continue;
        for (uint32_t k = 0; k < out->syntax.n; k++)
            if (form_names(out->syntax.items[k], s, 0)) { fb_push(&out->helpers, defined.items[d]); break; }
    }
    free(defined.items);
}
static void lib_scan_free(LibScan *ls) {
    free(ls->syntax.items); free(ls->exp_pub.items); free(ls->exp_in.items); free(ls->helpers.items);
    memset(ls, 0, sizeof *ls);
}
static const Symbol *lib_hidden(SL *sl, const Symbol *mod, const Symbol *name) {
    char buf[512]; size_t at = 0;
    for (const char *p = mod->name; *p && at + 1 < sizeof buf; p++) buf[at++] = *p == '/' ? '-' : *p;
    buf[at] = '\0';
    snprintf(buf + at, sizeof buf - at, "--syntax--%s", name->name);
    return I(sl, buf);
}
/* The library's side: drop its exported macros from `(export ...)`, and
 * export each helper under its hidden spelling -- `(export (rename h
 * <lib>--syntax--h))`, which the export-rename lowering turns into the
 * definition's own spelling or a forwarding alias. */
static Form *lib_rewrite_exports(SL *sl, Form *deflib, const Symbol *mod, const LibScan *ls) {
    FB decls = {0};
    fb_push(&decls, deflib->as.list.items[0]);
    fb_push(&decls, deflib->as.list.items[1]);
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        if (!head_is(d, sl->s_export)) { fb_push(&decls, d); continue; }
        FB e = {0};
        fb_push(&e, d->as.list.items[0]);
        for (uint32_t j = 1; j < d->as.list.len; j++) {
            Form *nm = d->as.list.items[j];
            if (nm->tag == F_SYM && lib_is_macro(ls, nm->as.sym)) continue;
            if (is_export_rename(sl, nm) && nm->as.list.items[1]->tag == F_SYM &&
                lib_is_macro(ls, nm->as.list.items[1]->as.sym)) continue;
            fb_push(&e, nm);
        }
        fb_push(&decls, fb_list(sl, &e, d->span));
    }
    if (ls->helpers.n) {
        Span sp = deflib->span;
        FB e = {0};
        fb_push(&e, Sym(sl, sp, sl->s_export));
        for (uint32_t k = 0; k < ls->helpers.n; k++) {
            const Symbol *h = ls->helpers.items[k]->as.sym;
            fb_push(&e, Ln(sl, sp, 3, Sym(sl, sp, I(sl, "rename")), Sym(sl, sp, h),
                           Sym(sl, sp, lib_hidden(sl, mod, h))));
        }
        fb_push(&decls, fb_list(sl, &e, sp));
    }
    return fb_list(sl, &decls, deflib->span);
}

/* The importer's side. */
struct LibSyntax {
    const Symbol *mod;
    FB   defs;              /* the macros, renamed: (define-syntax <hidden> <spec>) */
    FB   exp_pub, exp_in;   /* exported macros: public name, name in the library */
    FB   helpers;           /* hidden helper spellings, imported by name */
    /* r7rs-library-defines-standard-or-stdlib-name: public exports the
     * library spells `<name>--user` in its module (lib_needs_respelling). */
    FB   respelled;
    bool registered, helpers_imported;
};
static bool lib_syntax_exports(const LibSyntax *lx, const Symbol *pub) {
    return fb_has_sym(&lx->exp_pub, pub);
}
/* r7rs-library-defines-standard-or-stdlib-name: would a library's own
 * definition named `s` collide?  A standard procedure's name is spelled onto
 * the prelude (`square` -> `r7rs-square`, an auto-loaded global), and an
 * auto-loaded stdlib global (`None`, `list-length`) is one already.  The test
 * is context-free -- it ignores what the library imports or excludes -- so
 * the library, spelling its definition, and every importer, reading its
 * source, agree on the spelling without seeing each other's imports. */
static bool lib_needs_respelling(SL *sl, const Symbol *s) {
    for (size_t i = 0; i < N_RENAMES; i++) if (sl->rn_from[i] == s) return true;
    for (size_t i = 0; i < N_ONDEMAND; i++) if (sl->od_from[i] == s) return true;
    return std_file_of(sl, s) != NULL;
}
static const Symbol *lib_user_spelling(SL *sl, const Symbol *s) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s--user", s->name);
    return I(sl, buf);
}
/* The module's spelling of the library export `pub`, when the library
 * respelled it; NULL otherwise. */
static const Symbol *lib_respelling(SL *sl, const struct LibSyntax *lx, const Symbol *pub) {
    if (!lx || !fb_has_sym(&lx->respelled, pub)) return NULL;
    return lib_user_spelling(sl, pub);
}
/* Which of a library's exports it respells: a plain export of a name it
 * defines, or the public name of an `(export (rename in pub))`, that
 * lib_needs_respelling holds for.  The library's own pass spells them so
 * (note_stdlib_clashes, and the export-rename arm of define-library). */
static void lib_respelled_exports(SL *sl, Form *deflib, FB *out) {
    FB defined = {0};
    library_defined_names(sl, deflib, &defined);
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *decl = deflib->as.list.items[i];
        if (!head_is(decl, sl->s_export)) continue;
        for (uint32_t j = 1; j < decl->as.list.len; j++) {
            Form *nm = decl->as.list.items[j];
            if (is_export_rename(sl, nm)) {
                Form *pub = nm->as.list.items[2];
                if (pub->tag == F_SYM && lib_needs_respelling(sl, pub->as.sym)) fb_push(out, pub);
            } else if (nm->tag == F_SYM && fb_has_sym(&defined, nm->as.sym) &&
                       lib_needs_respelling(sl, nm->as.sym)) {
                fb_push(out, nm);
            }
        }
    }
    free(defined.items);
}
/* Bind an importer's bare names to a library's respelled exports: what the
 * import set keeps of them under their own names (not excluded, not renamed
 * away, in an `only` list when there is one; a prefix has its own rule). */
static void lib_bind_respelled(SL *sl, const LibSyntax *lx, const SchemeImportSpec *spec) {
    if (!lx || !lx->respelled.n || (spec && spec->prefix)) return;
    for (uint32_t k = 0; k < lx->respelled.n; k++) {
        const Symbol *pub = lx->respelled.items[k]->as.sym;
        if (spec) {
            if (spec_excludes(spec, pub)) continue;
            bool renamed = false, kept = !spec->has_only;
            for (uint32_t i = 0; i + 1 < spec->renames.n && !renamed; i += 2)
                renamed = spec->renames.items[i + 1]->as.sym == pub;
            for (uint32_t i = 0; i < spec->only.n && !kept; i++) kept = spec->only.items[i]->as.sym == pub;
            if (renamed || !kept) continue;
        }
        if (sl->n_renames >= 64) { err(lx->respelled.items[k], "too many (rename ...) names"); return; }
        sl->renames[sl->n_renames].from = pub;
        sl->renames[sl->n_renames].to   = lib_user_spelling(sl, pub);
        sl->n_renames++;
    }
}
/* Rename `f`'s symbols through from[i] -> to[i], as code: quoted data and a
 * quasiquote's template are left alone, its unquotes are not. */
static Form *lib_rename(SL *sl, Form *f, const FB *from, const FB *to, int qq) {
    if (!f) return f;
    switch (f->tag) {
        case F_SYM:
            if (qq == 0)
                for (uint32_t i = 0; i < from->n; i++)
                    if (from->items[i]->as.sym == f->as.sym) return Sym(sl, f->span, to->items[i]->as.sym);
            return f;
        case F_QUOTE: return f;
        case F_QUASIQUOTE: case F_UNQUOTE: case F_UNQUOTE_SPLICING: case F_LIST: case F_VEC: {
            int sub = f->tag == F_QUASIQUOTE ? qq + 1
                    : (f->tag == F_UNQUOTE || f->tag == F_UNQUOTE_SPLICING) ? (qq > 0 ? qq - 1 : 0) : qq;
            Form *c = (Form *)arena_alloc(sl->a, sizeof(Form));
            *c = *f;
            c->as.list.items = (Form **)arena_alloc(sl->a, (f->as.list.len ? f->as.list.len : 1) * sizeof(Form *));
            for (uint32_t i = 0; i < f->as.list.len; i++) c->as.list.items[i] = lib_rename(sl, f->as.list.items[i], from, to, sub);
            return c;
        }
        default: return f;
    }
}
/* Read a Scheme library's source for its macros; NULL when `path` is not
 * Scheme or does not read. */
static Form **lib_read_source(SL *sl, const char *path, uint32_t *nf) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, len = 0, got;
    char *raw = (char *)malloc(cap);
    if (!raw) { fclose(fp); fprintf(stderr, "tur: oom\n"); abort(); }
    while ((got = fread(raw + len, 1, cap - len - 1, fp)) > 0) {
        len += got;
        if (cap - len < 2) {
            cap *= 2;
            raw = (char *)realloc(raw, cap);
            if (!raw) { fclose(fp); fprintf(stderr, "tur: oom\n"); abort(); }
        }
    }
    fclose(fp);
    raw[len] = '\0';
    const char *body = raw; size_t blen = len;
    const char *bad = NULL; size_t bad_len = 0;
    LangDialect dialect = LANG_TURMERIC;
    ReaderType rt = detect_lang_dialect(raw, len, &body, &blen, &bad, &bad_len, &dialect);
    size_t pl = strlen(path);
    bool scm = pl > 4 && strcmp(path + pl - 4, ".scm") == 0;
    if (bad || (rt != READER_R7RS && rt != READER_R7RS_SWEET && !scm)) { free(raw); return NULL; }
    char *src = (char *)arena_alloc(sl->a, blen + 1);
    memcpy(src, body, blen);
    src[blen] = '\0';
    free(raw);
    char *path_copy = (char *)arena_alloc(sl->a, pl + 1);
    memcpy(path_copy, path, pl + 1);
    SourceFile *sf = (SourceFile *)arena_alloc(sl->a, sizeof(SourceFile));
    *sf = (SourceFile){0};
    sf->path        = path_copy;
    sf->src         = src;
    sf->len         = blen;
    sf->file_id     = diag_alloc_file_id();
    /* A library may be written in `#lang r7rs/sweet`; a `.scm` file with no
     * line is the Scheme reader's. */
    sf->reader_type = rt == READER_R7RS_SWEET ? READER_R7RS_SWEET : READER_R7RS;
    sf->lang        = LANG_R7RS;
    diag_register_file(sf);
    return read_all_with_registry(sl->a, sl->st, sf, NULL, nf);
}
static LibSyntax *lib_syntax_of(SL *sl, const Form *libname, const Symbol *mod) {
    if (!mod || !libname || libname->tag != F_LIST || libname->as.list.len == 0 ||
        libname->as.list.items[0]->tag != F_SYM)
        return NULL;
    const char *head = libname->as.list.items[0]->as.sym->name;
    if (strcmp(head, "scheme") == 0 || strcmp(head, "turmeric") == 0) return NULL;
    for (uint32_t i = 0; i < sl->n_libsyn; i++)
        if (sl->libsyn[i]->mod == mod)
            return (sl->libsyn[i]->exp_pub.n || sl->libsyn[i]->respelled.n) ? sl->libsyn[i] : NULL;
    LibSyntax *lx = (LibSyntax *)arena_alloc(sl->a, sizeof(LibSyntax));
    memset(lx, 0, sizeof *lx);
    lx->mod = mod;
    if (sl->n_libsyn == sl->cap_libsyn) {
        sl->cap_libsyn = sl->cap_libsyn ? sl->cap_libsyn * 2 : 4;
        sl->libsyn = (LibSyntax **)realloc(sl->libsyn, sl->cap_libsyn * sizeof(LibSyntax *));
        if (!sl->libsyn) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->libsyn[sl->n_libsyn++] = lx;
    char path[4096];
    if (!sl->lib_resolve || !sl->lib_resolve(sl->lib_resolve_ud, mod->name, path, sizeof path)) return NULL;
    uint32_t nf = 0;
    Form **fs = lib_read_source(sl, path, &nf);
    Form *deflib = NULL;
    for (uint32_t i = 0; fs && i < nf && !deflib; i++)
        if (head_is(fs[i], sl->s_define_library)) deflib = fs[i];
    if (!deflib) return NULL;
    deflib = expand_library_includes(sl, deflib);
    lib_respelled_exports(sl, deflib, &lx->respelled);
    LibScan ls = {0};
    lib_scan(sl, deflib, &ls);
    if (!ls.exp_pub.n) { lib_scan_free(&ls); return lx->respelled.n ? lx : NULL; }
    /* from -> to: every macro and every helper to its hidden spelling. */
    FB from = {0}, to = {0};
    for (uint32_t k = 0; k < ls.syntax.n; k++) {
        Form *nm = ls.syntax.items[k]->as.list.items[1];
        fb_push(&from, nm);
        fb_push(&to, Sym(sl, nm->span, lib_hidden(sl, mod, nm->as.sym)));
    }
    for (uint32_t k = 0; k < ls.helpers.n; k++) {
        Form *h = ls.helpers.items[k];
        fb_push(&from, h);
        fb_push(&to, Sym(sl, h->span, lib_hidden(sl, mod, h->as.sym)));
        fb_push(&lx->helpers, to.items[to.n - 1]);
    }
    for (uint32_t k = 0; k < ls.syntax.n; k++) {
        Form *d = ls.syntax.items[k];
        if (d->as.list.len != 3 || d->as.list.items[1]->tag != F_SYM) continue;
        Form *spec = d->as.list.items[2];
        if (!head_is(spec, sl->s_syntax_rules)) continue;   /* the library's own pass says why */
        /* (syntax-rules [<ellipsis>] (<literal> ...) <rule> ...): the
         * literals and the ellipsis match by name at the use site, so they
         * keep theirs; the rules are renamed. */
        uint32_t first_rule = 1;
        if (first_rule < spec->as.list.len && spec->as.list.items[first_rule]->tag == F_SYM) first_rule++;
        first_rule++;
        FB ns = {0};
        for (uint32_t i = 0; i < spec->as.list.len; i++)
            fb_push(&ns, i < first_rule ? spec->as.list.items[i] : lib_rename(sl, spec->as.list.items[i], &from, &to, 0));
        Form *nspec = fb_list(sl, &ns, spec->span);
        fb_push(&lx->defs, Ln(sl, d->span, 3, d->as.list.items[0],
                              Sym(sl, d->as.list.items[1]->span, lib_hidden(sl, mod, d->as.list.items[1]->as.sym)),
                              nspec));
    }
    for (uint32_t k = 0; k < ls.exp_pub.n; k++) {
        fb_push(&lx->exp_pub, ls.exp_pub.items[k]);
        fb_push(&lx->exp_in, ls.exp_in.items[k]);
    }
    free(from.items); free(to.items);
    lib_scan_free(&ls);
    return lx;
}
static void lib_syntax_import(SL *sl, LibSyntax *lx, const SchemeImportSpec *spec, Span sp) {
    if (!lx->registered) {
        for (uint32_t k = 0; k < lx->defs.n; k++) sr_define(sl, lx->defs.items[k]);
        lx->registered = true;
    }
    for (uint32_t k = 0; k < lx->exp_pub.n; k++) {
        const Symbol *pub = lx->exp_pub.items[k]->as.sym;
        const Symbol *vis = pub;
        if (spec) {
            if (spec->has_only) {
                bool kept = false;
                for (uint32_t i = 0; i < spec->only.n && !kept; i++) kept = spec->only.items[i]->as.sym == pub;
                if (!kept) continue;
            }
            if (spec_excludes(spec, pub)) continue;
            bool renamed = false;
            for (uint32_t i = 0; i + 1 < spec->renames.n; i += 2)
                if (spec->renames.items[i + 1]->as.sym == pub) { vis = spec->renames.items[i]->as.sym; renamed = true; }
            if (!renamed && spec->prefix) {
                char buf[512];
                snprintf(buf, sizeof buf, "%s%s", spec->prefix->name, pub->name);
                vis = I(sl, buf);
            }
        }
        SMacro *m = sr_lookup(sl, lib_hidden(sl, lx->mod, lx->exp_in.items[k]->as.sym));
        if (!m) continue;
        SMacro *c = (SMacro *)arena_alloc(sl->a, sizeof(SMacro));
        *c = *m;
        c->name = vis;
        sr_push(sl, c);
    }
    if (lx->helpers.n && !lx->helpers_imported) {
        FB refer = {0};
        for (uint32_t k = 0; k < lx->helpers.n; k++) fb_push(&refer, lx->helpers.items[k]);
        fb_push(&sl->imports, Ln(sl, sp, 4, Sym(sl, sp, sl->t_import), Sym(sl, sp, lx->mod),
                                 Kw(sl, sp, sl->t_refer), fb_vec(sl, &refer, sp)));
        sl->needs_module = true;
        lx->helpers_imported = true;
    }
}

/* ---------------------------------------------------------------------------
 * r7rs-srfi-plan S1: `(import (srfi N))`.
 *
 * An importable SRFI is one file, stdlib/srfi/<N>.scm, holding one
 * `(define-library (srfi N) ...)` in plain R7RS.  It is not a module:
 *
 *   - The load expander splices the file in, once per compile, when a Scheme
 *     file imports the SRFI (lib_files_of_set).  srfi_source_forms turns its
 *     define-library into its body's definitions, each spelled
 *     srfi<N>--<name> so it meets neither the program's names nor the
 *     stdlib's, and the lowering emits them in place like the prelude's --
 *     outside a program's module wrapper -- so every module of the compile
 *     sees them (srfi_span).
 *   - Every importer, in every lowering pass, knows the SRFI's export list
 *     and macros (SrfiLib: from the spliced form, or read from the file in a
 *     pass the splice did not reach), registers its macros under hidden
 *     spellings, and binds each name its import set keeps (srfi_import): a
 *     macro to its macro, a definition to its srfi<N>-- spelling, and a
 *     re-export of an R7RS name to R7RS's own binding -- which needs no rule
 *     at all when the name is unchanged, so a built-in SRFI's import emits
 *     nothing.
 *
 * An SRFI file imports (scheme ...) libraries and other SRFIs: an on-demand
 * library's names, and another SRFI's exports, are spelled onto their
 * targets in the SRFI's own forms, so the SRFI's import does not make them
 * visible to the program.
 * ------------------------------------------------------------------------- */
static bool srfi_span(Span sp) {
    const SourceFile *f = diag_source_file(sp.file_id);
    return f && f->path && (strstr(f->path, "stdlib/srfi/") != NULL || strstr(f->path, "stdlib/sicp/") != NULL);
}
static const Symbol *srfi_spelling(SL *sl, int64_t num, const Symbol *name) {
    char buf[256];
    if (num == SICP_EXTRAS_NUM) snprintf(buf, sizeof buf, "sicpx--%s", name->name);
    else snprintf(buf, sizeof buf, "srfi%lld--%s", (long long)num, name->name);
    return I(sl, buf);
}
typedef struct SrfiLib {
    int64_t num;
    bool    registered;
    bool    spliced;           /* its body's forms went into this pass's stream */
    FB      exp_pub, exp_in;   /* exports: the public name, the name in the library */
    FB      macros;            /* its define-syntax names */
    FB      defined;           /* the names its body defines */
    FB      defs;              /* its macros, renamed: (define-syntax srfi<N>--m <spec>) */
    FB      from, to;          /* its own names -> their spellings (definitions,
                                * macros, and its on-demand imports' names) */
} SrfiLib;
static SrfiLib *srfi_lib_of(SL *sl, int64_t num);
static void srfi_lib_register(SL *sl, SrfiLib *lib);
/* The imports of an SRFI's define-library: an on-demand (scheme ...)
 * library's names map onto its procedures, and another SRFI's exports onto
 * their spellings (S5: SRFI 13 imports SRFI 14), its macros registered.  An
 * SRFI is imported whole, by a bare `(srfi N)`.  Anything else is refused. */
static void srfi_scan_imports(SL *sl, Form *deflib, FB *from, FB *to) {
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        if (!head_is(d, sl->s_import)) continue;
        for (uint32_t j = 1; j < d->as.list.len; j++) {
            Form *set = d->as.list.items[j];
            int64_t dep_num = srfi_libname_num(set);
            if (dep_num >= 0 && srfi_importable(srfi_row(dep_num))) {
                SrfiLib *dep = srfi_lib_of(sl, dep_num);
                if (!dep) { err(set, "%s: its library file was not found", lib_label(dep_num).s); continue; }
                srfi_lib_register(sl, dep);
                for (uint32_t k = 0; k < dep->exp_pub.n; k++) {
                    const Symbol *in = dep->exp_in.items[k]->as.sym;
                    bool own = fb_has_sym(&dep->defined, in) || fb_has_sym(&dep->macros, in);
                    fb_push(from, Sym(sl, set->span, dep->exp_pub.items[k]->as.sym));
                    fb_push(to, Sym(sl, set->span, own ? srfi_spelling(sl, dep_num, in) : in));
                }
                continue;
            }
            int li = scheme_lib_index(set);
            if (li < 0) {
                err(set, "an SRFI's library file may import (scheme ...) libraries and other SRFIs only");
                continue;
            }
            if (SCHEME_LIBS[li].kind != LIB_ONDEMAND) continue;
            for (size_t k = 0; k < N_ONDEMAND; k++)
                if (sl->od_lib[k] == li) {
                    fb_push(from, Sym(sl, set->span, sl->od_from[k]));
                    fb_push(to, Sym(sl, set->span, sl->od_to[k]));
                }
        }
    }
}
/* The forms of an SRFI body: its `begin` declarations' (and a chosen
 * cond-expand clause's), flattened; `each` sees each one. */
static void srfi_body_forms(SL *sl, Form *deflib, FB *out) {
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        FB sub = {0};
        if (head_is(d, sl->s_begin)) {
            for (uint32_t k = 1; k < d->as.list.len; k++) fb_push(&sub, d->as.list.items[k]);
        } else if (head_is(d, sl->s_cond_expand)) {
            uint32_t n; Form **items;
            if (cond_expand_clause(sl, d, &n, &items))
                for (uint32_t c = 0; c < n; c++)
                    if (head_is(items[c], sl->s_begin))
                        for (uint32_t k = 1; k < items[c]->as.list.len; k++) fb_push(&sub, items[c]->as.list.items[k]);
        }
        for (uint32_t k = 0; k < sub.n; k++) {
            Form *x = sub.items[k];
            if (head_is(x, sl->s_begin)) {
                /* a nested (begin ...) of definitions */
                Form *wrap[3] = { Sym(sl, x->span, I(sl, "define-library")), Sym(sl, x->span, I(sl, "_")), x };
                srfi_body_forms(sl, List(sl, x->span, wrap, 3), out);
            } else fb_push(out, x);
        }
        free(sub.items);
    }
}
static SrfiLib *srfi_lib_build(SL *sl, int64_t num, Form *deflib) {
    SrfiLib *lib = (SrfiLib *)arena_alloc(sl->a, sizeof(SrfiLib));
    memset(lib, 0, sizeof *lib);
    lib->num = num;
    if (sl->n_srfilib == sl->cap_srfilib) {
        sl->cap_srfilib = sl->cap_srfilib ? sl->cap_srfilib * 2 : 4;
        sl->srfilib = (SrfiLib **)realloc(sl->srfilib, sl->cap_srfilib * sizeof(SrfiLib *));
        if (!sl->srfilib) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->srfilib[sl->n_srfilib++] = lib;
    FB body = {0};
    srfi_body_forms(sl, deflib, &body);
    for (uint32_t k = 0; k < body.n; k++) {
        Form *x = body.items[k];
        if (head_is(x, sl->s_define_syntax) && x->as.list.len >= 2 && x->as.list.items[1]->tag == F_SYM)
            fb_push(&lib->macros, x->as.list.items[1]);
    }
    library_defined_names(sl, deflib, &lib->defined);
    srfi_scan_imports(sl, deflib, &lib->from, &lib->to);
    for (uint32_t k = 0; k < lib->defined.n; k++) {
        fb_push(&lib->from, lib->defined.items[k]);
        fb_push(&lib->to, Sym(sl, lib->defined.items[k]->span, srfi_spelling(sl, num, lib->defined.items[k]->as.sym)));
    }
    for (uint32_t k = 0; k < lib->macros.n; k++) {
        fb_push(&lib->from, lib->macros.items[k]);
        fb_push(&lib->to, Sym(sl, lib->macros.items[k]->span, srfi_spelling(sl, num, lib->macros.items[k]->as.sym)));
    }
    for (uint32_t i = 2; i < deflib->as.list.len; i++) {
        Form *d = deflib->as.list.items[i];
        if (!head_is(d, sl->s_export)) continue;
        for (uint32_t j = 1; j < d->as.list.len; j++) {
            Form *nm = d->as.list.items[j];
            if (nm->tag == F_SYM) { fb_push(&lib->exp_pub, nm); fb_push(&lib->exp_in, nm); }
            else if (is_export_rename(sl, nm) && nm->as.list.items[1]->tag == F_SYM && nm->as.list.items[2]->tag == F_SYM) {
                fb_push(&lib->exp_pub, nm->as.list.items[2]);
                fb_push(&lib->exp_in, nm->as.list.items[1]);
            }
        }
    }
    for (uint32_t k = 0; k < body.n; k++) {
        Form *d = body.items[k];
        if (!head_is(d, sl->s_define_syntax) || d->as.list.len != 3 || d->as.list.items[1]->tag != F_SYM) continue;
        Form *spec = d->as.list.items[2];
        if (!head_is(spec, sl->s_syntax_rules)) { err(spec, "an SRFI's macro must be a syntax-rules"); continue; }
        uint32_t first_rule = 1;
        if (first_rule < spec->as.list.len && spec->as.list.items[first_rule]->tag == F_SYM) first_rule++;
        first_rule++;
        FB ns = {0};
        for (uint32_t i = 0; i < spec->as.list.len; i++)
            fb_push(&ns, i < first_rule ? spec->as.list.items[i] : lib_rename(sl, spec->as.list.items[i], &lib->from, &lib->to, 0));
        fb_push(&lib->defs, Ln(sl, d->span, 3, d->as.list.items[0],
                               Sym(sl, d->as.list.items[1]->span, srfi_spelling(sl, num, d->as.list.items[1]->as.sym)),
                               fb_list(sl, &ns, spec->span)));
    }
    free(body.items);
    return lib;
}
static SrfiLib *srfi_lib_find(SL *sl, int64_t num) {
    for (uint32_t i = 0; i < sl->n_srfilib; i++) if (sl->srfilib[i]->num == num) return sl->srfilib[i];
    return NULL;
}
/* The SrfiLib of an importable SRFI: from the spliced define-library if this
 * pass had it, else read from its file through the module loader's search. */
static SrfiLib *srfi_lib_of(SL *sl, int64_t num) {
    SrfiLib *lib = srfi_lib_find(sl, num);
    if (lib) return lib;
    char mod[64], path[4096];
    if (num == SICP_EXTRAS_NUM) snprintf(mod, sizeof mod, "sicp/extras");
    else snprintf(mod, sizeof mod, "srfi/%lld", (long long)num);
    if (!sl->lib_resolve || !sl->lib_resolve(sl->lib_resolve_ud, mod, path, sizeof path)) return NULL;
    uint32_t nf = 0;
    Form **fs = lib_read_source(sl, path, &nf);
    for (uint32_t i = 0; fs && i < nf; i++)
        if (head_is(fs[i], sl->s_define_library) && srfi_libname_num(fs[i]->as.list.items[1]) == num)
            return srfi_lib_build(sl, num, expand_library_includes(sl, fs[i]));
    return NULL;
}
static void srfi_lib_register(SL *sl, SrfiLib *lib) {
    if (lib->registered) return;
    for (uint32_t k = 0; k < lib->defs.n; k++) sr_define(sl, lib->defs.items[k]);
    lib->registered = true;
}
/* The pre-pass: a spliced SRFI file's define-library -> its body's forms,
 * macros left out and every name spelled onto its target.  Its macros are
 * registered for this pass here, since its own procedures may use them. */
static void srfi_source_forms(SL *sl, Form *deflib, FB *out) {
    int64_t num = deflib->as.list.len >= 2 ? srfi_libname_num(deflib->as.list.items[1]) : -1;
    if (num < 0 || !srfi_importable(srfi_row(num))) {
        err(deflib, "a file under stdlib/srfi/ holds one (define-library (srfi N) ...) of an importable SRFI "
                    "(stdlib/sicp/extras.scm, the (sicp extras) one)");
        return;
    }
    /* Spliced twice into one pass: once is enough.  A library already read
     * from its file (another SRFI imports it) is not spliced yet. */
    SrfiLib *lib = srfi_lib_find(sl, num);
    if (lib && lib->spliced) return;
    if (!lib) lib = srfi_lib_build(sl, num, deflib);
    lib->spliced = true;
    srfi_lib_register(sl, lib);
    FB body = {0};
    srfi_body_forms(sl, deflib, &body);
    for (uint32_t k = 0; k < body.n; k++)
        if (!head_is(body.items[k], sl->s_define_syntax))
            fb_push(out, lib_rename(sl, body.items[k], &lib->from, &lib->to, 0));
    free(body.items);
}
/* What a standard name means to the program with no SRFI in the way: its
 * prelude procedure, an imported on-demand library's, or itself. */
static const Symbol *std_meaning(SL *sl, const Symbol *s) {
    for (size_t i = 0; i < N_RENAMES; i++) if (sl->rn_from[i] == s) return sl->rn_to[i];
    for (size_t i = 0; i < N_ONDEMAND; i++)
        if (sl->od_from[i] == s && sl->od_lib[i] >= 0 && sl->lib_imported[sl->od_lib[i]]) return sl->od_to[i];
    return s;
}
static bool is_std_name(SL *sl, const Symbol *s) {
    return std_meaning(sl, s) != s || is_scheme_syntax_name(s->name);
}
/* Whether an import set makes `s`, spelled as it is, mean its library's `s`:
 * no `only` leaves it out, no `except` drops it, no `rename` moves it (or
 * puts another name in its place), and no `prefix` respells it. */
static bool set_keeps_bare(const Form *set, const Symbol *s) {
    while (set && set->tag == F_LIST && set->as.list.len >= 2 && set->as.list.items[0]->tag == F_SYM) {
        const char *h = set->as.list.items[0]->as.sym->name;
        if (!strcmp(h, "only") || !strcmp(h, "except")) {
            bool listed = false;
            for (uint32_t i = 2; i < set->as.list.len; i++)
                if (set->as.list.items[i]->tag == F_SYM && set->as.list.items[i]->as.sym == s) listed = true;
            if (listed != (h[0] == 'o')) return false;
        } else if (!strcmp(h, "rename")) {
            for (uint32_t i = 2; i < set->as.list.len; i++) {
                const Form *pr = set->as.list.items[i];
                if (pr->tag != F_LIST) continue;
                for (uint32_t k = 0; k < pr->as.list.len; k++)
                    if (pr->as.list.items[k]->tag == F_SYM && pr->as.list.items[k]->as.sym == s) return false;
            }
        } else if (!strcmp(h, "prefix")) {
            return false;
        } else {
            return true;
        }
        set = set->as.list.items[1];
    }
    return true;
}
/* Whether the unit imports R7RS's own `s` under that name.  The (scheme
 * base) import sets stand for every standard library here -- the lowering
 * keeps no per-library name lists -- which is exact for the names an SRFI
 * so far conflicts on (SRFI 13's string-map and string-for-each). */
static bool std_imported(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->base_sets.n; i++)
        if (set_keeps_bare(sl->base_sets.items[i], s)) return true;
    return false;
}
/* D5: bind `vis` to `target` for an import of (srfi N) -- unless that gives
 * one imported name two meanings (R7RS 5.2), or the program defines it. */
static bool srfi_bind(SL *sl, const Symbol *vis, const Symbol *target, int64_t num, const Form *at) {
    for (uint32_t i = 0; i < sl->n_srfi; i++) {
        if (sl->srfi_from[i] != vis) continue;
        if (sl->srfi_to[i] == target) return true;
        err(at, "'%s' is imported from %s and from %s with different meanings; R7RS 5.2 "
                "allows one binding per imported name -- rename or prefix one of them",
            vis->name, lib_label(sl->srfi_by[i]).s, lib_label(num).s);
        return false;
    }
    if (std_imported(sl, vis) && is_std_name(sl, vis) && std_meaning(sl, vis) != target) {
        err(at, "'%s' would name both R7RS's own '%s' and %s's; R7RS 5.2 allows one binding per "
                "imported name -- rename or prefix the SRFI's, or leave R7RS's out with (except (scheme base) %s)",
            vis->name, vis->name, lib_label(num).s, vis->name);
        return false;
    }
    if (fb_has_sym(&sl->user_globals, vis)) {
        err(at, "'%s' is imported from %s and also defined by this program (R7RS 5.2); import it "
                "with (except %s %s) to define your own",
            vis->name, lib_label(num).s, lib_label(num).s, vis->name);
        return false;
    }
    if (sl->n_srfi == sl->cap_srfi) {
        sl->cap_srfi = sl->cap_srfi ? sl->cap_srfi * 2 : 16;
        sl->srfi_from = (const Symbol **)realloc((void *)sl->srfi_from, sl->cap_srfi * sizeof(Symbol *));
        sl->srfi_to = (const Symbol **)realloc((void *)sl->srfi_to, sl->cap_srfi * sizeof(Symbol *));
        sl->srfi_by = (int64_t *)realloc(sl->srfi_by, sl->cap_srfi * sizeof(int64_t));
        if (!sl->srfi_from || !sl->srfi_to || !sl->srfi_by) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->srfi_from[sl->n_srfi] = vis;
    sl->srfi_to[sl->n_srfi] = target;
    sl->srfi_by[sl->n_srfi] = num;
    sl->n_srfi++;
    return true;
}
/* The name an import set gives the library's `pub`, or NULL when the set
 * leaves it out (only / except).  NULL spec: a bare import. */
static const Symbol *import_visible_name(SL *sl, const SchemeImportSpec *spec, const Symbol *pub) {
    if (!spec) return pub;
    if (spec->has_only) {
        bool kept = false;
        for (uint32_t i = 0; i < spec->only.n && !kept; i++) kept = spec->only.items[i]->as.sym == pub;
        if (!kept) return NULL;
    }
    if (spec_excludes(spec, pub)) return NULL;
    for (uint32_t i = 0; i + 1 < spec->renames.n; i += 2)
        if (spec->renames.items[i + 1]->as.sym == pub) return spec->renames.items[i]->as.sym;
    if (spec->prefix) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s%s", spec->prefix->name, pub->name);
        return I(sl, buf);
    }
    return pub;
}
static void srfi_import(SL *sl, Form *libname, const SchemeImportSpec *spec, Span sp) {
    (void)sp;
    int64_t num = srfi_libname_num(libname);
    if (num < 0) { err(libname, "%s", bad_libname_msg(libname)); return; }
    const SrfiRow *row = srfi_row(num);
    if (!row) {
        err(libname, "(srfi %lld): no such SRFI in #lang r7rs; the SRFI table in docs/guides/r7rs-guide.md "
                     "lists the ones it has", (long long)num);
        return;
    }
    switch (row->kind) {
        case SRFI_NOLIB:
            err(libname, "(srfi %d) (%s) has no library: %s", row->num, row->title, row->why);
            return;
        case SRFI_NOTPLANNED:
            err(libname, "(srfi %d) (%s) is not planned: %s", row->num, row->title, row->why);
            return;
        case SRFI_NOTYET:
            err(libname, "(srfi %d) (%s) is not supported yet; the SRFI table in docs/guides/r7rs-guide.md "
                         "says which SRFIs are", row->num, row->title);
            return;
        default: break;
    }
    SrfiLib *lib = srfi_lib_of(sl, num);
    if (!lib) {
        err(libname, "%s: its library file %s could not be read -- is the stdlib installed?",
            lib_label(row->num).s, row->file);
        return;
    }
    srfi_lib_register(sl, lib);
    if (spec) {
        FB *named[2] = { (FB *)&spec->only, (FB *)&spec->except };
        for (int w = 0; w < 2; w++)
            for (uint32_t i = 0; i < named[w]->n; i++)
                if (!fb_has_sym(&lib->exp_pub, named[w]->items[i]->as.sym))
                    err(named[w]->items[i], "%s does not export '%s'", lib_label(row->num).s, named[w]->items[i]->as.sym->name);
        for (uint32_t i = 0; i + 1 < spec->renames.n; i += 2)
            if (!fb_has_sym(&lib->exp_pub, spec->renames.items[i + 1]->as.sym))
                err(spec->renames.items[i + 1], "%s does not export '%s'", lib_label(row->num).s,
                    spec->renames.items[i + 1]->as.sym->name);
    }
    for (uint32_t k = 0; k < lib->exp_pub.n; k++) {
        const Symbol *pub = lib->exp_pub.items[k]->as.sym, *in = lib->exp_in.items[k]->as.sym;
        const Symbol *vis = import_visible_name(sl, spec, pub);
        if (!vis) continue;
        if (fb_has_sym(&lib->macros, in)) {
            /* A macro of the SRFI's: its hidden macro, under the visible name. */
            const Symbol *hid = srfi_spelling(sl, num, in);
            SMacro *m = sr_lookup(sl, hid);
            if (!m || !srfi_bind(sl, vis, hid, num, libname)) continue;
            SMacro *c = (SMacro *)arena_alloc(sl->a, sizeof(SMacro));
            *c = *m;
            c->name = vis;
            sr_push(sl, c);
        } else if (fb_has_sym(&lib->defined, in)) {
            srfi_bind(sl, vis, srfi_spelling(sl, num, in), num, libname);
        } else if (is_scheme_syntax_name(in->name)) {
            /* A re-export of R7RS syntax.  Under its own name it IS the
             * syntax; under another, a macro forwards to it.  SRFI 61's
             * `cond` is R7RS's with one more clause shape: importing it, by
             * any name, turns that shape on in this unit (cond_chain).  SRFI
             * 17's `set!` is the same arrangement: importing it turns the
             * `(set! (f arg ...) v)` target on (srfi17_place). */
            if (num == 61 && in == sl->s_cond) sl->srfi61_cond = true;
            if (num == 17 && in == sl->s_set) sl->srfi17_set = true;
            if (vis == in) continue;
            if (!srfi_bind(sl, vis, in, num, libname)) continue;
            Span s = libname->span;
            Form *pat = Ln(sl, s, 3, Sym(sl, s, sl->s_underscore), Sym(sl, s, sl->s_dot), Sym(sl, s, I(sl, "args__")));
            Form *tmpl = Ln(sl, s, 3, Sym(sl, s, in), Sym(sl, s, sl->s_dot), Sym(sl, s, I(sl, "args__")));
            Form *rule = Ln(sl, s, 2, pat, tmpl);
            Form *spec_form = Ln(sl, s, 3, Sym(sl, s, sl->s_syntax_rules), Ln(sl, s, 0), rule);
            sr_define(sl, Ln(sl, s, 3, Sym(sl, s, sl->s_define_syntax), Sym(sl, s, vis), spec_form));
        } else {
            /* A re-export of an R7RS procedure: what it means in the SRFI (an
             * on-demand library's procedure, or the prelude's).  Unchanged,
             * it needs no rule: the import is a no-op. */
            const Symbol *target = in;
            for (uint32_t f = 0; f < lib->from.n; f++)
                if (lib->from.items[f]->as.sym == in) { target = lib->to.items[f]->as.sym; break; }
            if (target == in) target = std_meaning(sl, in);
            if (vis == in && target == std_meaning(sl, vis)) continue;
            srfi_bind(sl, vis, target, num, libname);
        }
    }
}

/* Point a global's spelling at `to`, replacing a clash rename it already had. */
static void set_clash(SL *sl, const Symbol *from, const Symbol *to) {
    for (uint32_t i = 0; i < sl->n_clash; i++)
        if (sl->clash_from[i] == from) { sl->clash_to[i] = to; return; }
    if (sl->n_clash == sl->cap_clash) {
        sl->cap_clash = sl->cap_clash ? sl->cap_clash * 2 : 8;
        sl->clash_from = (const Symbol **)realloc((void *)sl->clash_from, sl->cap_clash * sizeof(Symbol *));
        sl->clash_to = (const Symbol **)realloc((void *)sl->clash_to, sl->cap_clash * sizeof(Symbol *));
        if (!sl->clash_from || !sl->clash_to) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    sl->clash_from[sl->n_clash] = from;
    sl->clash_to[sl->n_clash++] = to;
}
static const Symbol *clash_spelling(const SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_clash; i++) if (sl->clash_from[i] == s) return sl->clash_to[i];
    return s;
}
static const Symbol *std_meaning(SL *sl, const Symbol *s);
/* r7rs-saved-standard-procedure-follows-redefinition: what an eager
 * reference to `s` (spelled `r` by rn) means from the current top-level
 * form -- the standard procedure when the program redefines `s` only in a
 * later form, else NULL (rn's answer stands). */
static const Symbol *std_before_redefinition(SL *sl, const Symbol *s, const Symbol *r) {
    if (clash_spelling(sl, s) != r) return NULL;
    for (uint32_t i = 0; i < sl->n_early; i++) {
        if (sl->early_sym[i] != s) continue;
        if (sl->cur_top >= sl->early_at[i]) return NULL;
        const Symbol *std = std_meaning(sl, s);
        return std != s ? std : NULL;
    }
    return NULL;
}
/* Record, for each clash name, the first top-level form that defines it. */
static void note_redefinition_points(SL *sl, Form *const *forms, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!is_scheme_file(forms[i]) || prelude_span(forms[i]->span) || srfi_span(forms[i]->span)) continue;
        if (head_is(forms[i], sl->s_define_library)) return;   /* a library: not a program */
        FB names = {0};
        user_define_names(sl, forms[i], &names);
        for (uint32_t k = 0; k < names.n; k++) {
            const Symbol *s = names.items[k]->as.sym;
            if (clash_spelling(sl, s) == s) continue;
            bool seen = false;
            for (uint32_t j = 0; j < sl->n_early && !seen; j++) seen = sl->early_sym[j] == s;
            if (seen) continue;
            if (sl->n_early == sl->cap_early) {
                sl->cap_early = sl->cap_early ? sl->cap_early * 2 : 8;
                sl->early_sym = (const Symbol **)realloc((void *)sl->early_sym, sl->cap_early * sizeof(Symbol *));
                sl->early_at = (uint32_t *)realloc(sl->early_at, sl->cap_early * sizeof(uint32_t));
                if (!sl->early_sym || !sl->early_at) { fprintf(stderr, "tur: oom\n"); abort(); }
            }
            sl->early_sym[sl->n_early] = s;
            sl->early_at[sl->n_early++] = i;
        }
        free(names.items);
    }
}
static void add_clash(SL *sl, const Symbol *s) {
    for (uint32_t i = 0; i < sl->n_clash; i++) if (sl->clash_from[i] == s) return;
    if (sl->n_clash == sl->cap_clash) {
        sl->cap_clash = sl->cap_clash ? sl->cap_clash * 2 : 8;
        sl->clash_from = (const Symbol **)realloc((void *)sl->clash_from, sl->cap_clash * sizeof(Symbol *));
        sl->clash_to = (const Symbol **)realloc((void *)sl->clash_to, sl->cap_clash * sizeof(Symbol *));
        if (!sl->clash_from || !sl->clash_to) { fprintf(stderr, "tur: oom\n"); abort(); }
    }
    char buf[256];
    snprintf(buf, sizeof buf, "%s--user", s->name);
    sl->clash_from[sl->n_clash] = s;
    sl->clash_to[sl->n_clash++] = I(sl, buf);
}
static void note_stdlib_clashes(SL *sl, Form *const *forms, uint32_t n) {
    FB lib = {0}, user = {0};
    bool library = false;
    for (uint32_t i = 0; i < n; i++) {
        if (!is_scheme_file(forms[i])) stdlib_names_of(sl, forms[i], &lib, 0);
        else if (!prelude_span(forms[i]->span) && !srfi_span(forms[i]->span)) {
            if (head_is(forms[i], sl->s_define_library)) library = true;
            user_define_names(sl, forms[i], &user);
        }
    }
    /* A library's names live in its module and are exported by name. */
    for (uint32_t u = 0; u < user.n; u++) {
        const Symbol *s = user.items[u]->as.sym;
        if (s->name[0] == '^' && s->len > 1) { set_clash(sl, s, caret_spelling(sl, s)); continue; }
        if (library) continue;
        /* A standard name the program defines for itself -- SICP's
         * `(define (square x) ...)` -- is the program's own from then on:
         * it shadows R7RS's, which the prelude and every SRFI keep.  R7RS
         * 5.2 calls redefining an import an error; like chibi and most
         * Schemes, a program here may do it.  (`(scheme base)` names are
         * global whether or not the program imports them, so refusing
         * would refuse programs that never imported the name.) */
        if (rn(sl, s) != s) { add_clash(sl, s); continue; }
        /* ... and so is an on-demand library's name -- `eval` of (scheme
         * eval) -- once the program imports that library.  rn cannot say so
         * yet: lib_imported is set as the import is lowered, after this scan,
         * and from then on the program's `(define (eval ...))` would be
         * spelled onto the library's own `r7rs-eval`, two C functions of one
         * name.  SICP 4.1 defines its own `eval`. */
        bool od_clash = false;
        for (size_t k = 0; k < N_ONDEMAND && !od_clash; k++)
            od_clash = sl->od_from[k] == s && sl->od_lib[k] >= 0 && sl->lib_named[sl->od_lib[k]];
        if (od_clash) { add_clash(sl, s); continue; }
        /* (A name Turmeric reserves for a type is rn'd by type_named_global.) */
        bool clash = false;
        for (uint32_t k = 0; k < lib.n && !clash; k++) clash = lib.items[k]->as.sym == s;
        if (!clash && sl->global_kind) clash = sl->global_kind(sl->lib_resolve_ud, s->name) == SCHEME_GLOBAL_STDLIB;
        if (clash) add_clash(sl, s);
    }
    /* r7rs-library-defines-standard-or-stdlib-name: ... except a name the
     * library defines that would collide in its module -- a standard one,
     * which would be spelled onto the prelude's `r7rs-square`, or an
     * auto-loaded stdlib global.  It is `<name>--user` there, and in every
     * importer (lib_respelled_exports), by one context-free rule. */
    if (library) {
        FB defined = {0};
        for (uint32_t i = 0; i < n; i++)
            if (is_scheme_file(forms[i]) && !prelude_span(forms[i]->span) && !srfi_span(forms[i]->span) &&
                head_is(forms[i], sl->s_define_library))
                library_defined_names(sl, forms[i], &defined);
        for (uint32_t d = 0; d < defined.n; d++) {
            const Symbol *s = defined.items[d]->as.sym;
            if (lib_needs_respelling(sl, s)) add_clash(sl, s);
        }
        free(defined.items);
    }
    FB binders = {0};
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i]) && !prelude_span(forms[i]->span) && !srfi_span(forms[i]->span))
            user_binders(sl, forms[i], &binders);
    for (uint32_t b = 0; b < binders.n; b++) {
        const Symbol *s = binders.items[b]->as.sym;
        if (rn(sl, s) == s && tur_name_is_reserved_special_form(s->name)) add_clash(sl, s);
    }
    free(binders.items);
    /* ... and a global the user defines (a program's, a library's) or
     * imports by name, whose uses then follow the definition. */
    FB reserved = {0};
    for (uint32_t u = 0; u < user.n; u++) fb_push(&reserved, user.items[u]);
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i]) && !prelude_span(forms[i]->span) && !srfi_span(forms[i]->span)) {
            import_bound_names(sl, forms[i], &reserved);
            export_rename_names(sl, forms[i], &reserved);
        }
    for (uint32_t r = 0; r < reserved.n; r++) {
        const Symbol *s = reserved.items[r]->as.sym;
        if (rn(sl, s) != s) continue;
        if (tur_name_is_reserved_special_form(s->name) && !is_scheme_syntax_name(s->name)) add_clash(sl, s);
    }
    free(reserved.items);
    free(lib.items);
    free(user.items);
}

/* r7rs-repl-forgets-macros-and-set: the raw forms of a REPL or `eval`
 * session's earlier turns, which an incremental elaboration does not hand the
 * lowering (scheme_lower_set_session_prior). */
static Form *const *g_session_prior;
static uint32_t     g_n_session_prior;
void scheme_lower_set_session_prior(Form *const *forms, uint32_t n) {
    g_session_prior = forms;
    g_n_session_prior = forms ? n : 0;
}
/* A top-level `define-syntax` of an earlier turn, unless a later top-level
 * definition of the same name replaced it.  `keep` holds the surviving
 * define-syntax forms, in order. */
static void session_macro_drop(FB *keep, const Symbol *name) {
    for (uint32_t k = 0; k < keep->n; k++) {
        if (keep->items[k]->as.list.items[1]->as.sym != name) continue;
        for (uint32_t j = k + 1; j < keep->n; j++) keep->items[j - 1] = keep->items[j];
        keep->n--;
        return;
    }
}
static void session_macro_scan(SL *sl, Form *f, FB *keep, bool take_syntax) {
    if (!f || !is_scheme_file(f) || !span_is_synthetic(f->span) || prelude_span(f->span)) return;
    if (head_is(f, sl->s_begin)) {
        for (uint32_t i = 1; i < f->as.list.len; i++)
            session_macro_scan(sl, f->as.list.items[i], keep, take_syntax);
        return;
    }
    if (head_is(f, sl->s_define_syntax)) {
        if (f->as.list.len != 3 || f->as.list.items[1]->tag != F_SYM) return;
        session_macro_drop(keep, f->as.list.items[1]->as.sym);
        if (take_syntax) fb_push(keep, f);
        return;
    }
    FB names = {0};
    user_define_names(sl, f, &names);
    for (uint32_t i = 0; i < names.n; i++) session_macro_drop(keep, names.items[i]->as.sym);
    free(names.items);
}
/* Register the macros the session's earlier turns defined, so a macro one
 * turn defines expands in the next.  Each turn is lowered on its own, so its
 * macro table started empty: `(define-syntax sw ...)` then `(sw 1 2)` was
 * "unknown function or operator 'sw'", at the prompt and through `eval`.
 *
 * The same holds for what an earlier turn's `import` set up in the lowering:
 * an SRFI's names (`(import (srfi 1))`, then `(fold + 0 l)` was unknown),
 * its macros (`cut`, `and-let*`), a (scheme ...) library's `except` and
 * `rename`, and a library's exported macros.  Replay those import sets for
 * that state.  What they emit -- a library's Turmeric `import`, which wraps
 * the turn in a module -- the earlier turn already did, so it is dropped. */
static void session_imports_load(SL *sl) {
    for (uint32_t i = 0; i < g_n_session_prior; i++) {
        Form *f = g_session_prior[i];
        if (!f || !is_scheme_file(f) || !span_is_synthetic(f->span) || prelude_span(f->span)) continue;
        if (!head_is(f, sl->s_import)) continue;
        uint32_t n_imports = sl->imports.n;
        bool needs_module = sl->needs_module;
        for (uint32_t j = 1; j < f->as.list.len; j++)
            if (f->as.list.items[j]->tag == F_LIST) lower_import_set(sl, f->as.list.items[j]);
        sl->imports.n = n_imports;
        sl->needs_module = needs_module;
    }
}
static void session_macros_load(SL *sl, Form *const *forms, uint32_t n) {
    session_imports_load(sl);
    FB keep = {0};
    for (uint32_t i = 0; i < g_n_session_prior; i++)
        session_macro_scan(sl, g_session_prior[i], &keep, true);
    /* This turn's own definitions replace an earlier macro of that name; its
     * own define-syntax forms register themselves as it is lowered. */
    for (uint32_t i = 0; i < n; i++) session_macro_scan(sl, forms[i], &keep, false);
    sl->in_user = true;
    for (uint32_t k = 0; k < keep.n; k++) sr_define(sl, keep.items[k]);
    sl->in_user = false;
    free(keep.items);
}

Form **scheme_lower_program(Arena *a, SymbolTable *st,
                            Form *const *forms, uint32_t n, uint32_t *out_n,
                            SchemeLibResolveFn resolve, SchemeGlobalFn global_kind,
                            SchemeStdlibFileFn stdlib_file, void *resolve_ud) {
    SL sl;
    sl_init(&sl, a, st);
    sl.lib_resolve = resolve;
    sl.lib_resolve_ud = resolve_ud;
    sl.global_kind = global_kind;
    sl.stdlib_file = stdlib_file;
    FB included = {0};
    expand_includes(&sl, forms, n, &included);
    forms = included.items;
    n = included.n;
    /* r7rs-turmeric-syntax-leaks item 8: the auto-loaded stdlib's globals,
     * each with its file, off the stdlib forms ahead of the program. */
    for (uint32_t i = 0; i < n; i++) {
        if (is_scheme_file(forms[i])) continue;
        const SourceFile *sf = diag_source_file(forms[i]->span.file_id);
        if (!sf || !sf->path) continue;
        const char *b = sf->path;
        for (const char *p = sf->path; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
        size_t bl = strlen(b);
        char base[128];
        if (bl < 5 || bl - 4 >= sizeof base || strcmp(b + bl - 4, ".tur") != 0) continue;
        memcpy(base, b, bl - 4);
        base[bl - 4] = '\0';
        if (!stdlib_autoloaded(base)) continue;
        FB names = {0};
        stdlib_names_of(&sl, forms[i], &names, 0);
        const Symbol *file = I(&sl, base);
        for (uint32_t k = 0; k < names.n; k++) std_put(&sl, names.items[k]->as.sym, file);
        free(names.items);
    }
    bool repl_turn = false;
    for (uint32_t i = 0; i < n && !repl_turn; i++)
        repl_turn = is_scheme_file(forms[i]) && span_is_synthetic(forms[i]->span);
    if (repl_turn) repl_grants_load(&sl);
    sl.repl_turn = repl_turn;
    /* r7rs-srfi-plan S1: a spliced SRFI file's define-library -> its
     * definitions, spelled onto their targets, before any scan reads the
     * program (the `set!` targets see the spelled names). */
    FB srfi_expanded = {0};
    {
        bool any = false;
        for (uint32_t i = 0; i < n && !any; i++) any = is_scheme_file(forms[i]) && srfi_span(forms[i]->span);
        if (any) {
            for (uint32_t i = 0; i < n; i++) {
                if (is_scheme_file(forms[i]) && srfi_span(forms[i]->span) && head_is(forms[i], sl.s_define_library))
                    srfi_source_forms(&sl, forms[i], &srfi_expanded);
                else fb_push(&srfi_expanded, forms[i]);
            }
            forms = srfi_expanded.items;
            n = srfi_expanded.n;
        }
    }
    {
        /* The caller's array is not ours to rewrite; the rewritten top level
         * is a copy (arena-owned, like the forms it holds). */
        Form **copy = (Form **)arena_alloc(sl.a, (n ? n : 1) * sizeof(Form *));
        for (uint32_t i = 0; i < n; i++) copy[i] = forms[i];
        uint32_t nn = n;
        mark_ondemand_imports(&sl, copy, n);
        forms = redefinitions_to_set(&sl, copy, n, &nn);
        n = nn;
    }
    /* D5's two facts about the unit: does it import (scheme base), and what
     * does it define at top level (a program's defines, a library's body)? */
    for (uint32_t i = 0; i < n; i++) {
        Form *f = forms[i];
        if (!is_scheme_file(f) || prelude_span(f->span) || srfi_span(f->span)) continue;
        FB sets = {0};
        if (head_is(f, sl.s_import))
            for (uint32_t j = 1; j < f->as.list.len; j++) fb_push(&sets, f->as.list.items[j]);
        if (head_is(f, sl.s_define_library)) {
            library_defined_names(&sl, f, &sl.user_globals);
            for (uint32_t d = 2; d < f->as.list.len; d++)
                if (head_is(f->as.list.items[d], sl.s_import))
                    for (uint32_t j = 1; j < f->as.list.items[d]->as.list.len; j++)
                        fb_push(&sets, f->as.list.items[d]->as.list.items[j]);
        } else {
            user_define_names(&sl, f, &sl.user_globals);
        }
        for (uint32_t j = 0; j < sets.n; j++) {
            const Form *set = sets.items[j];
            while (set && set->tag == F_LIST && set->as.list.len >= 2 && set->as.list.items[0]->tag == F_SYM &&
                   (!strcmp(set->as.list.items[0]->as.sym->name, "only") || !strcmp(set->as.list.items[0]->as.sym->name, "except") ||
                    !strcmp(set->as.list.items[0]->as.sym->name, "prefix") || !strcmp(set->as.list.items[0]->as.sym->name, "rename")))
                set = set->as.list.items[1];
            int li = scheme_lib_index(set);
            if (li >= 0 && strcmp(SCHEME_LIBS[li].name, "base") == 0) fb_push(&sl.base_sets, sets.items[j]);
            if (li >= 0 && head_is(f, sl.s_import)) sl.lib_named[li] = true;
        }
        free(sets.items);
    }
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) collect_setter_macros(&sl, forms[i]);
    for (uint32_t i = 0; i < n; i++)
        if (is_scheme_file(forms[i])) collect_muts(&sl, forms[i]);
    note_stdlib_clashes(&sl, forms, n);
    if (!repl_turn) note_redefinition_points(&sl, forms, n);
    FB out = {0}, sforms = {0};
    {
        /* r7rs-procedure-body-forward-reference: the user's forms, in order,
         * with the prelude's left out. */
        FB user = {0}, srfi = {0};
        for (uint32_t i = 0; i < n; i++) {
            if (!is_scheme_file(forms[i]) || prelude_span(forms[i]->span)) continue;
            if (srfi_span(forms[i]->span)) fb_push(&srfi, forms[i]);
            else fb_push(&user, forms[i]);
        }
        note_forward_defs(&sl, user.items, user.n);
        note_forward_defs(&sl, srfi.items, srfi.n);
        free(user.items); free(srfi.items);
    }
    if (repl_turn && g_n_session_prior > 0) session_macros_load(&sl, forms, n);
    Span first_sp = SPAN_UNKNOWN;
    bool have_first = false;
    for (uint32_t i = 0; i < n; i++) {
        if (is_scheme_file(forms[i]) && (prelude_span(forms[i]->span) || srfi_span(forms[i]->span))) {
            /* R9: the prelude and the on-demand library files are `#lang
             * r7rs` too, and they share this stream with the user's file.
             * Lower them, but in place: they are neither part of a user
             * `define-library` (which may hold nothing else, so a project
             * library build was refused over the prelude's first defstruct)
             * nor of the module a program with imports is wrapped in. */
            /* r7rs-srfi-plan S1: an SRFI's definitions, too -- Scheme, lowered
             * as Scheme (srfi_span is not prelude_span), but the unit's own,
             * emitted where every module sees them. */
            FB pf = {0};
            sl.in_user = false;
            lower_toplevel(&sl, forms[i], &pf);
            for (uint32_t k = 0; k < pf.n; k++) fb_push(&out, pf.items[k]);
            free(pf.items);
        } else if (is_scheme_file(forms[i])) {
            if (!have_first) { first_sp = forms[i]->span; have_first = true; }
            uint32_t from = sforms.n, lib_from = sl.lib_body.n;
            sl.cur_top = i;
            sl.in_user = true;
            lower_toplevel(&sl, forms[i], &sforms);
            sl.in_user = false;
            for (uint32_t k = from; k < sforms.n; k++) sforms.items[k] = ac_walk(&sl, sforms.items[k]);
            for (uint32_t k = lib_from; k < sl.lib_body.n; k++)
                sl.lib_body.items[k] = ac_walk(&sl, sl.lib_body.items[k]);
        } else {
            fb_push(&out, forms[i]);
        }
    }
    if (sl.has_library) {
        /* A library file: exactly the defmodule.  Anything else at top level
         * in the same file has nowhere to go. */
        if (sforms.n > 0) err(sforms.items[0], "a file with a define-library may hold nothing else at top level");
        FB m = {0};
        fb_push(&m, Sym(&sl, first_sp, sl.t_defmodule));
        fb_push(&m, Sym(&sl, first_sp, sl.lib_name));
        FB ex = {0};
        fb_push(&ex, Sym(&sl, first_sp, sl.t_export));
        for (uint32_t i = 0; i < sl.lib_exports.n; i++) fb_push(&ex, sl.lib_exports.items[i]);
        fb_push(&m, fb_list(&sl, &ex, first_sp));
        for (uint32_t i = 0; i < sl.imports.n; i++) fb_push(&m, sl.imports.items[i]);
        for (uint32_t i = 0; i < sl.lib_body.n; i++) fb_push(&m, sl.lib_body.items[i]);
        fb_push(&out, fb_list(&sl, &m, first_sp));
        free(sl.lib_exports.items); free(sl.lib_body.items); free(sl.imports.items);
    } else if (sl.needs_module && have_first && repl_turn) {
        /* r7rs-repl-forgets-macros-and-set: a REPL or `eval` turn that
         * imports a library.  Wrapped like a program, its expressions became a
         * `main` nothing calls (`(import (lb m)) (display "x")` printed
         * nothing) and its definitions were private to the wrapper module.
         * The module carries only the imports; the turn's own forms stay at
         * the session's top level, where the imported names are visible. */
        FB m = {0};
        fb_push(&m, Sym(&sl, first_sp, sl.t_defmodule));
        fb_push(&m, Sym(&sl, first_sp, I(&sl, "r7rs-repl-imports")));
        for (uint32_t i = 0; i < sl.imports.n; i++) fb_push(&m, sl.imports.items[i]);
        fb_push(&out, fb_list(&sl, &m, first_sp));
        for (uint32_t i = 0; i < sforms.n; i++) fb_push(&out, sforms.items[i]);
        free(sl.imports.items);
    } else if (sl.needs_module && have_first) {
        /* A program with imports: wrap it in a defmodule named after its file,
         * imports first, definitions next, and the top-level expressions as
         * the body of a synthesized `main` (the top-level fold does not look
         * inside a module). */
        /* r7rs-program-file-named-with-leading-digit-fails-to-compile: the
         * name becomes the C prefix of every definition in the module, so it
         * must not start with a digit -- SICP readers name files `1.1.scm`
         * and `3.5-streams.scm`.  Such a stem is prefixed `r7rs-program-`.
         * Only the LAST dot is the extension: `1.1.scm` is `1-1`, not `1`,
         * which every other section's file would share. */
        const SourceFile *sf = diag_source_file(first_sp.file_id);
        char mbuf[128] = "r7rs-program";
        if (sf && sf->path) {
            const char *base = strrchr(sf->path, '/');
            base = base ? base + 1 : sf->path;
            const char *ext = strrchr(base, '.');
            if (!ext || ext == base) ext = base + strlen(base);
            size_t at = 0;
            if (*base >= '0' && *base <= '9') {
                memcpy(mbuf, "r7rs-program-", 13);
                at = 13;
            }
            for (const char *p = base; p < ext && at + 1 < sizeof mbuf; p++) {
                mbuf[at++] = (*p == '-' || *p == '_' || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                              (*p >= '0' && *p <= '9')) ? *p : '-';
            }
            if (at) mbuf[at] = '\0';
        }
        FB m = {0}, stmts = {0};
        fb_push(&m, Sym(&sl, first_sp, sl.t_defmodule));
        fb_push(&m, Sym(&sl, first_sp, I(&sl, mbuf)));
        for (uint32_t i = 0; i < sl.imports.n; i++) fb_push(&m, sl.imports.items[i]);
        for (uint32_t i = 0; i < sforms.n; i++) {
            Form *f = sforms.items[i];
            bool is_def = f->tag == F_LIST && f->as.list.len > 0 && f->as.list.items[0]->tag == F_SYM &&
                          strncmp(f->as.list.items[0]->as.sym->name, "def", 3) == 0;
            if (is_def) fb_push(&m, f); else fb_push(&stmts, f);
        }
        if (stmts.n > 0) {
            if (sl.user_main) {
                err(stmts.items[0], "a program that imports libraries and defines (main) cannot also have top-level expressions; move them into main");
            }
            FB body = {0};
            fb_push(&body, Sym(&sl, first_sp, sl.t_do));
            for (uint32_t i = 0; i < stmts.n; i++) fb_push(&body, stmts.items[i]);
            fb_push(&body, Int(&sl, first_sp, 0));
            fb_push(&m, Ln(&sl, first_sp, 5, Sym(&sl, first_sp, sl.t_defn), Sym(&sl, first_sp, sl.s_main),
                           Vec(&sl, first_sp, NULL, 0),
                           form_type_ann(sl.a, first_sp, Sym(&sl, first_sp, sl.t_int)),
                           fb_list(&sl, &body, first_sp)));
        }
        free(stmts.items);
        fb_push(&out, fb_list(&sl, &m, first_sp));
        free(sl.imports.items);
    } else {
        for (uint32_t i = 0; i < sforms.n; i++) fb_push(&out, sforms.items[i]);
        free(sl.imports.items);
    }
    free(sforms.items);
    Form **res = (Form **)arena_alloc(a, (out.n + 1) * sizeof(Form *));
    for (uint32_t i = 0; i < out.n; i++) res[i] = out.items[i];
    res[out.n] = NULL;
    *out_n = out.n;
    free(out.items);
    free((void *)sl.muts);
    free(included.items);
    free((void *)sl.clash_from);
    free((void *)sl.early_sym);
    free(sl.early_at);
    free((void *)sl.clash_to);
    free((void *)sl.setters);
    free(sl.macros);
    for (uint32_t i = 0; i < sl.n_srfilib; i++) {
        SrfiLib *lib = sl.srfilib[i];
        free(lib->exp_pub.items); free(lib->exp_in.items); free(lib->macros.items);
        free(lib->defined.items); free(lib->defs.items); free(lib->from.items); free(lib->to.items);
    }
    free(sl.srfilib);
    free((void *)sl.srfi_from); free((void *)sl.srfi_to); free(sl.srfi_by);
    free(sl.user_globals.items);
    free(sl.base_sets.items);
    if (repl_turn) repl_grants_save(&sl);
    free((void *)sl.std_keys);
    free((void *)sl.std_files);
    free((void *)sl.granted);
    free((void *)sl.denied);
    free(srfi_expanded.items);
    for (uint32_t i = 0; i < sl.n_libsyn; i++) {
        LibSyntax *lx = sl.libsyn[i];
        free(lx->defs.items); free(lx->exp_pub.items); free(lx->exp_in.items); free(lx->helpers.items);
        free(lx->respelled.items);
    }
    free(sl.libsyn);
    return res;
}

/* R7: the on-demand library files a top-level form of a Scheme file needs --
 * an `(import ...)` of (scheme time) / (scheme process-context) /
 * (scheme file), directly or under only/prefix/rename/except, or the same
 * inside a define-library's import declarations.  The load expander splices
 * each one in (deduplicated by its visited set) before the lowering runs, so
 * the library's forms are lowered with everything else. */
static void lib_files_of_set(const Form *set, const char **out, uint32_t cap, uint32_t *n) {
    while (set && set->tag == F_LIST && set->as.list.len >= 2 && set->as.list.items[0]->tag == F_SYM) {
        const char *h = set->as.list.items[0]->as.sym->name;
        if (strcmp(h, "only") && strcmp(h, "prefix") && strcmp(h, "rename") && strcmp(h, "except")) break;
        set = set->as.list.items[1];
    }
    /* r7rs-srfi-plan D3: an importable SRFI's file is spliced in, once per
     * compile; its own (scheme ...) imports splice theirs in turn. */
    const SrfiRow *srow = srfi_row(srfi_libname_num(set));
    if (srfi_importable(srow)) {
        for (uint32_t i = 0; i < *n; i++) if (out[i] == srow->file) return;
        for (size_t h = 0; h < sizeof SRFI_HELPERS / sizeof SRFI_HELPERS[0]; h++)
            if (SRFI_HELPERS[h].num == srow->num && *n < cap) out[(*n)++] = SRFI_HELPERS[h].file;
        if (*n < cap) out[(*n)++] = srow->file;
        return;
    }
    int li = scheme_lib_index(set);
    if (li < 0 || SCHEME_LIBS[li].kind != LIB_ONDEMAND) return;
    for (uint32_t i = 0; i < *n; i++) if (out[i] == SCHEME_LIBS[li].what) return;
    if (*n < cap) out[(*n)++] = SCHEME_LIBS[li].what;
}
uint32_t scheme_import_library_files(const Form *f, const char **out, uint32_t cap) {
    uint32_t n = 0;
    if (!f || f->tag != F_LIST || f->as.list.len == 0 || f->as.list.items[0]->tag != F_SYM) return 0;
    if (!is_scheme_file(f)) return 0;
    const char *h = f->as.list.items[0]->as.sym->name;
    if (strcmp(h, "import") == 0) {
        for (uint32_t i = 1; i < f->as.list.len; i++) lib_files_of_set(f->as.list.items[i], out, cap, &n);
    } else if (strcmp(h, "define-library") == 0) {
        for (uint32_t i = 2; i < f->as.list.len; i++) {
            const Form *d = f->as.list.items[i];
            if (d->tag == F_LIST && d->as.list.len > 0 && d->as.list.items[0]->tag == F_SYM &&
                strcmp(d->as.list.items[0]->as.sym->name, "import") == 0)
                for (uint32_t j = 1; j < d->as.list.len; j++) lib_files_of_set(d->as.list.items[j], out, cap, &n);
        }
    }
    return n;
}
