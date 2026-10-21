#ifndef TUR_READER_MACROS_H
#define TUR_READER_MACROS_H

/* User-defined reader macros — see docs/archive/reader-macros-plan.md.
 *
 * RM0 is the registry skeleton: types, reserved-prefix table, and lookup
 * API. No reader behavior changes yet — the registry starts empty and the
 * dispatch hook is a no-op when the registry is NULL or has no entries.
 */

#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "forms.h"
#include "symbols.h"

/* Body mode determines how the reader consumes the body of a #foo<delim>...
 * form. RM_BODY_NONE is for the bare-identifier form (#foo with no body).
 */
typedef enum ReaderMacroMode {
    RM_BODY_NONE = 0,   /* bare identifier, e.g. #nan */
    RM_BODY_RAW,        /* verbatim source slice, balanced delim scan */
    RM_BODY_DATUM,      /* recursive read_form until close-delim */
    RM_BODY_STRING,     /* standard string literal body, e.g. #name"..." */
} ReaderMacroMode;

typedef struct ReaderMacroEntry {
    StrSlice         name;       /* identifier after '#' */
    int              delim;      /* '(', '[', '{', or 0 for bare */
    ReaderMacroMode  mode;
    Form            *template;   /* expansion template (RM1+) */
    Span             site;       /* where it was registered */
} ReaderMacroEntry;

typedef struct ReaderMacroRegistry {
    Arena              *arena;
    ReaderMacroEntry  **entries;   /* arena-allocated; grown by doubling */
    uint32_t            len;
    uint32_t            cap;
    /* Transitive-RM decision #2: when true, re-registering the same
     * (name, delim) is a hard error with a "previously registered here"
     * note. Compile entry points set this; the REPL/TuriEnv registry
     * leaves it false so iterative redefinition (and `src_acc` replay)
     * continues to work. */
    bool                strict;
    /* When true, a top-level `(reader-macros/define ...)` directive is still
     * registered but is ALSO kept in the emitted Form stream instead of being
     * stripped.  The formatter sets this so it can round-trip the directive
     * (stripping it silently deletes source -- see fmt_format_source); the
     * compiler leaves it false so the directive stays a pure read-time
     * side effect. */
    bool                keep_define_forms;
} ReaderMacroRegistry;

/* Initialize an empty registry. Backing storage uses `arena`. */
void reader_macros_init(ReaderMacroRegistry *reg, Arena *arena);

/* Check whether a (name, delim) tuple collides with a built-in reader
 * prefix. Names match exact bytes; delim is the bracket char that would
 * follow the name (0 for bare). Returns true if reserved. */
bool reader_macros_is_reserved(StrSlice name, int delim);

/* Look up a registered handler by (name, delim). Returns NULL if absent
 * or if `reg` itself is NULL. */
const ReaderMacroEntry *reader_macros_lookup(const ReaderMacroRegistry *reg,
                                             StrSlice name, int delim);

/* Look up any registered handler by name alone, ignoring delim. Used by
 * the reader to produce a helpful "expects 'X' body" diagnostic when the
 * caller invoked a known macro with the wrong delimiter. Returns NULL if
 * no entry has this name. */
const ReaderMacroEntry *reader_macros_lookup_any(const ReaderMacroRegistry *reg,
                                                 StrSlice name);

/* Register a handler. Returns 0 on success, -1 on collision with a built-in
 * or a previously registered (name, delim). On collision, emits a diagnostic
 * at `site` and leaves the registry unchanged. */
int reader_macros_register(ReaderMacroRegistry *reg,
                           StrSlice name, int delim,
                           ReaderMacroMode mode, Form *template,
                           Span site);

/* RM1: recognize `(reader-macros/define NAME MODE TEMPLATE)` at the top
 * level. Returns true if `f` is shaped like a reader-macro registration
 * directive (the args themselves are validated by *_register_from_form). */
bool reader_macros_is_define_form(const Form *f);

/* RM1: register a macro from a parsed `(reader-macros/define ...)` form.
 * Returns 0 on success, -1 on error (diagnostic emitted). */
int reader_macros_register_from_form(ReaderMacroRegistry *reg, const Form *f);

/* Install the built-in `#`-dispatch macros every file gets, whatever its
 * `#lang` line says.  Called from read_all_with_registry_from after the
 * registry is established and before the read loop.
 *
 * Today that is one macro, `#s"..."` => `(string/from-cstr "...")`.  It used
 * to be the `stringed` `#lang` layer; the layer axis was decommissioned
 * (docs/archive/lang-layers-decommission-plan.md) because every other
 * `#`-dispatch in the language is unconditional and this one had no reason
 * not to be.  `(load "stdlib/string.tur")` is still what brings in the code
 * the expansion calls -- registration is read-time, the String code is not.
 *
 * `arena`/`st` build the expansion templates; both must outlive `reg`. */
void reader_macros_install_builtins(ReaderMacroRegistry *reg,
                                    Arena *arena, SymbolTable *st);

#endif
