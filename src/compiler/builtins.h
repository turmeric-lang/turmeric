#ifndef TUR_BUILTINS_H
#define TUR_BUILTINS_H

#include "expr.h"
#include "symbols.h"
#include "types.h"

/* The operator dispatch table per §1.1 — single source of truth for primitive
 * operator codegen. Typeclasses (§12.2) will extend this table; phase 1
 * populates it statically. */

typedef enum BuiltinShape {
    BS_BIN_INFIX,        /* "(a) <op> (b)" */
    BS_VARIADIC_FOLD,    /* "((a) <op> (b)) <op> (c) ..." (left-fold) */
    BS_PREFIX_UNARY,     /* "<op>(a)" */
    BS_PREFIX_UNARY_FREE,/* "free(a)" - special case for ref drop */
    BS_PRINTLN_INT,      /* printf("%lld\n", (long long)(x))           */
    BS_PRINTLN_FLOAT,    /* printf("%g\n", x)                          */
    BS_PRINTLN_BOOL,     /* puts(x ? "true" : "false")                 */
    BS_PRINTLN_CSTR,     /* puts(x)                                    */
    BS_PRINTLN_UINT,     /* printf("%llu\n", (unsigned long long)(x))  */
    BS_PRINTLN_FLOAT32,  /* printf("%.7g\n", (double)(x))              */
    BS_DIV_CHECK,        /* Division with zero check: (a / b) with runtime check */
    BS_AND_SC,           /* short-circuit (handled in codegen)   */
    BS_OR_SC,            /* short-circuit (handled in codegen)   */
    /* Phase U3: Unsafe primitives */
    BS_PTR_WRITE,        /* *ptr = value - emit as statement, return nil */
    BS_PTR_ARITH,        /* ptr arithmetic: (char *)ptr op offset */
    BS_PTR_DEREF,        /* ptr dereference: *((T *)ptr) */
    BS_UNSAFE_CAST,      /* unsafe-cast: C-style cast */
    BS_REINTERPRET,      /* reinterpret: bitwise reinterpretation */
    BS_TRANSMUTE,        /* transmute: type-punning with size check */
    BS_ARRAY_GET_UNCHECKED,  /* array-get-unchecked: *(ptr + index) */
    BS_ARRAY_SET_UNCHECKED,  /* array-set-unchecked: *(ptr + index) = value */
    BS_RAW_MALLOC,       /* raw-malloc: malloc(size) */
    BS_RAW_FREE,         /* raw-free: free(ptr) */
    BS_RAW_REALLOC,      /* raw-realloc: realloc(ptr, new_size) */
    BS_RAW_MEMCPY,       /* raw-memcpy: memcpy(dest, src, n) */
    BS_RAW_MEMSET,       /* raw-memset: memset(dest, byte, n) */
    /* Phase U3: FFI */
    BS_DLOPEN,           /* dlopen: dlopen(path, RTLD_LAZY) */
    BS_DLSYM,           /* dlsym: dlsym(handle, symbol) */
    BS_DLCLOSE,          /* dlclose: dlclose(handle) */
    /* Generic function-call shape: c_op(arg0, arg1, ...) */
    BS_FUNC_CALL,        /* c_op(args...) */
} BuiltinShape;

struct BuiltinSpec {
    const char    *name;          /* string form, used for registration */
    const Symbol  *name_sym;      /* interned at builtins_init */
    int            min_arity;
    int            max_arity;     /* -1 = variadic */
    Type           arg_type;      /* uniform arg type; TY_UNKNOWN means "any" */
    Type           result_type;
    BuiltinShape   shape;
    const char    *c_op;
    /* effect-row-honesty-plan W4: the builtin's declared effect row, as the
     * name of one compiler-known `^capability` effect ("IO" on `println`), or
     * NULL for a pure builtin.  A name rather than an Effect* because the
     * effect pass resolves it against its own env.  Last field so the
     * positional initializers that predate it read as NULL. */
    const char    *effect;
};

/* The declared effect (see BuiltinSpec.effect) of the builtin `name` --
 * the same for every overload of it -- or NULL.  For callers that have a
 * name rather than a spec (a Saffron EX_DYN_OP). */
const char *builtin_effect_for_name(const char *name);

/* One-time setup. Interns the names of every builtin into `st`. */
void builtins_init(SymbolTable *st);

/* Resolve an operator. Returns NULL if no entry matches. */
const BuiltinSpec *builtin_lookup(const Symbol *name, Type first_arg_type,
                                  uint32_t n_args);

/* For diagnostic messages — returns the first entry with this name,
 * regardless of arity/type. NULL if no entry exists with this name. */
const BuiltinSpec *builtin_first_with_name(const Symbol *name);

/* Describe a builtin by its spelled name, for hover and signature help.
 *
 * Writes up to `cap` bytes of NUL-terminated text into `out`: one
 * "(name : (fn [T ...] : R))" line per distinct overload, and a trailing
 * count when the overload set is longer than the lines shown. Returns the
 * total number of table rows with this name; 0 means it is not a builtin and
 * `out` is set to the empty string.
 *
 * Keyed by string rather than by Symbol: the language server holds a word
 * scraped out of a buffer, not an interned name, and it has no SymbolTable of
 * its own to intern into. */
int builtin_describe(const char *name, char *out, size_t cap);

/* True when a BS_DIV_CHECK spec divides floating-point operands.
 *
 * BS_DIV_CHECK backs both integer and float `/`. Integer division by zero is
 * UB in C and has no value to produce, so it keeps the runtime guard. Float
 * division by zero is well-defined in IEEE 754 (inf / NaN), so guarding it
 * turns a legitimate value into a process abort -- and the guard is emitted as
 * `(b) ? ... : ...`, which additionally costs a branch per division and draws
 * -Wliteral-conversion when the divisor is a constant like 8.0. Callers use
 * this to emit a bare `(a) / (b)` for the float rows. */
bool builtin_div_is_ieee(const BuiltinSpec *spec);

/* stdlib-os-surface-plan P0.5: the print family's destination.  Every
 * BS_PRINTLN_* row is `println` (stdout, trailing newline) unless its c_op
 * says otherwise: "stderr" is `eprintln`, "stderr-nonl" is `eprint`. */
bool builtin_print_to_stderr(const BuiltinSpec *spec);
bool builtin_print_newline(const BuiltinSpec *spec);

/* The C statement (no indent, no trailing newline) that prints `arg` for a
 * BS_PRINTLN_* `shape` through `spec`'s destination.  Heap string; the caller
 * frees it.  Shared by the direct and CPS emitters so both spell it alike. */
char *builtin_print_stmt(const BuiltinSpec *spec, BuiltinShape shape,
                         const char *arg);

/* Collect overloads for a builtin name.
 * Returns number of entries written to `out` (up to max_out). */
uint32_t builtin_collect_with_name(const Symbol *name,
                                   const BuiltinSpec **out,
                                   uint32_t max_out);

#endif
