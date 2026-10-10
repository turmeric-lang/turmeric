#include "reader_macros.h"

#include <string.h>

#include "diag.h"

/* Reserved built-in #-prefixes. Any attempt to register one collides.
 *
 * Layout: { name_chars, delim }. delim 0 means the built-in is selected by
 * just the name (e.g. `#lang`); otherwise the named prefix is followed by
 * a specific delimiter (e.g. `#s` only collides with `#s(`).
 *
 * The trailing { NULL, 0 } sentinel ends the list.
 */
typedef struct ReservedPrefix {
    const char *name;
    int         delim;
} ReservedPrefix;

static const ReservedPrefix kReserved[] = {
    /* From read_form's hardcoded #-dispatch (src/compiler/reader.c). */
    { ";",    0   },  /* #; datum comment */
    { "",     '{' },  /* #{...} map literal */
    { "s",    '(' },  /* #s(...) set literal */
    { "r",    '{' },  /* #r{...} range literal */
    { "",     '[' },  /* #[...] attribute */
    { "?",    '(' },  /* #?(...) reader conditional */
    { "|",    0   },  /* #| ... |# block comment */
    { "lang", 0   },  /* #lang directive */
    /* Built-in named string macros: cannot be redefined by user macros. */
    { "rx",   '"' },  /* #rx"..." regex literal */
    { "dedent", '"' },  /* #dedent"""...""" indent-aware string literal */
    { NULL,   0   },
};

static bool strslice_eq_cstr(StrSlice s, const char *cstr) {
    size_t n = strlen(cstr);
    if (s.len != n) return false;
    return memcmp(s.p, cstr, n) == 0;
}

void reader_macros_init(ReaderMacroRegistry *reg, Arena *arena) {
    reg->arena   = arena;
    reg->entries = NULL;
    reg->len     = 0;
    reg->cap     = 0;
    reg->strict  = false;
    reg->keep_define_forms = false;
}

bool reader_macros_is_reserved(StrSlice name, int delim) {
    for (const ReservedPrefix *p = kReserved; p->name != NULL; ++p) {
        if (!strslice_eq_cstr(name, p->name)) continue;
        if (p->delim == 0 || p->delim == delim) return true;
    }
    return false;
}

const ReaderMacroEntry *reader_macros_lookup(const ReaderMacroRegistry *reg,
                                             StrSlice name, int delim) {
    if (!reg) return NULL;
    for (uint32_t i = 0; i < reg->len; ++i) {
        ReaderMacroEntry *e = reg->entries[i];
        if (e->delim != delim) continue;
        if (e->name.len != name.len) continue;
        if (memcmp(e->name.p, name.p, name.len) != 0) continue;
        return e;
    }
    return NULL;
}

const ReaderMacroEntry *reader_macros_lookup_any(const ReaderMacroRegistry *reg,
                                                 StrSlice name) {
    if (!reg) return NULL;
    for (uint32_t i = 0; i < reg->len; ++i) {
        ReaderMacroEntry *e = reg->entries[i];
        if (e->name.len != name.len) continue;
        if (memcmp(e->name.p, name.p, name.len) != 0) continue;
        return e;
    }
    return NULL;
}

/* The reader's dispatch peeks `is_sym_start`/`is_sym_cont` after '#'
 * (with one carve-out: `#` is not a continuation byte; see reader.c::
 * is_macro_id_cont). A name that doesn't fit this shape can be registered
 * but never matched, which is a silent footgun — reject it at registration
 * time instead. The predicate is duplicated here rather than exported to
 * avoid leaking reader internals through reader.h. */
#include <ctype.h>
static bool rm_is_sym_start(unsigned char c) {
    if (isalpha(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.': case '^': case '|': case 0x27 /* ' */:
        case 0xCE: case 0xE2:
            return true;
    }
    return false;
}
static bool rm_is_sym_cont(unsigned char c) {
    if (isalnum(c)) return true;
    switch (c) {
        case '+': case '-': case '*': case '/':
        case '=': case '<': case '>':
        case '!': case '?':
        case '_': case '$': case '&':
        case '.': case '^': case '|': case 0x27 /* ' */:
        case 0xCE: case 0xE2:
        case 0x80: case 0x83: case 0x88: case 0xBB:
            return true;
    }
    return false;
}
static bool is_valid_macro_name(StrSlice name) {
    if (name.len == 0) return false;
    if (!rm_is_sym_start((unsigned char)name.p[0])) return false;
    for (uint32_t i = 1; i < name.len; ++i) {
        if (!rm_is_sym_cont((unsigned char)name.p[i])) return false;
    }
    return true;
}

int reader_macros_register(ReaderMacroRegistry *reg,
                           StrSlice name, int delim,
                           ReaderMacroMode mode, Form *template,
                           Span site) {
    if (!is_valid_macro_name(name)) {
        diag_emit(DIAG_ERROR, site,
                  "reader macro name '#%.*s' is not a valid identifier "
                  "(must match [a-zA-Z][a-zA-Z0-9-_]*)",
                  (int)name.len, name.p);
        return -1;
    }
    if (reader_macros_is_reserved(name, delim)) {
        diag_emit(DIAG_ERROR, site,
                  "reader macro '#%.*s' cannot shadow built-in '#%.*s'",
                  (int)name.len, name.p, (int)name.len, name.p);
        return -1;
    }
    /* Re-registration policy depends on `reg->strict`:
     *   - strict=false (REPL/TuriEnv): silently update in place. Matches
     *     `defn` redefinition semantics; lets the REPL `src_acc` replay
     *     prior `(reader-macros/define ...)` forms each turn without
     *     spurious "already registered" errors.
     *   - strict=true (batch compile): treat collision as a hard error
     *     with a "previously registered here" note pointing at the
     *     existing entry's site. Catches the realistic "two third-party
     *     modules both register `#json`" footgun. */
    for (uint32_t i = 0; i < reg->len; ++i) {
        ReaderMacroEntry *existing = reg->entries[i];
        if (existing->delim != delim) continue;
        if (existing->name.len != name.len) continue;
        if (memcmp(existing->name.p, name.p, name.len) != 0) continue;
        if (reg->strict) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "reader macro '#%.*s' already registered",
                     (int)name.len, name.p);
            DiagNote note = {
                .level = DIAG_NOTE,
                .span = existing->site,
                .message = "previously registered here",
            };
            diag_emit_with_notes(DIAG_ERROR, site, msg, &note, 1);
            return -1;
        }
        existing->mode     = mode;
        existing->template = template;
        existing->site     = site;
        return 0;
    }

    if (reg->len == reg->cap) {
        uint32_t new_cap = reg->cap ? reg->cap * 2 : 8;
        ReaderMacroEntry **fresh = (ReaderMacroEntry **)arena_alloc(
            reg->arena, sizeof(ReaderMacroEntry *) * new_cap);
        if (reg->entries) {
            memcpy(fresh, reg->entries,
                   sizeof(ReaderMacroEntry *) * reg->len);
        }
        reg->entries = fresh;
        reg->cap = new_cap;
    }

    /* Copy the name bytes into the arena so the entry outlives the caller's
     * scratch buffer. */
    char *name_copy = (char *)arena_alloc(reg->arena, name.len);
    memcpy(name_copy, name.p, name.len);

    ReaderMacroEntry *e = (ReaderMacroEntry *)arena_alloc(
        reg->arena, sizeof(ReaderMacroEntry));
    e->name.p   = name_copy;
    e->name.len = name.len;
    e->delim    = delim;
    e->mode     = mode;
    e->template = template;
    e->site     = site;

    reg->entries[reg->len++] = e;
    return 0;
}

/* ------------------------------------------------------------------------
 * RM1: parse `(reader-macros/define NAME MODE TEMPLATE)` directives.
 * ------------------------------------------------------------------------ */

static const char kDefineHead[] = "reader-macros/define";

static const Form *unwrap_quote(const Form *f) {
    if (!f) return NULL;
    if (f->tag == F_QUOTE && f->as.list.len == 1) return f->as.list.items[0];
    return f;
}

bool reader_macros_is_define_form(const Form *f) {
    if (!f || f->tag != F_LIST || f->as.list.len < 1) return false;
    const Form *head = f->as.list.items[0];
    if (!head || head->tag != F_SYM || !head->as.sym) return false;
    const Symbol *s = head->as.sym;
    size_t n = sizeof(kDefineHead) - 1;
    return s->len == n && memcmp(s->name, kDefineHead, n) == 0;
}

static bool keyword_to_mode_delim(const Symbol *kw,
                                  ReaderMacroMode *out_mode,
                                  int *out_delim) {
    static const struct {
        const char      *name;
        ReaderMacroMode  mode;
        int              delim;
    } table[] = {
        { "none",          RM_BODY_NONE,  0   },
        { "raw-paren",     RM_BODY_RAW,   '(' },
        { "raw-bracket",   RM_BODY_RAW,   '[' },
        { "raw-brace",     RM_BODY_RAW,   '{' },
        { "datum-paren",   RM_BODY_DATUM, '(' },
        { "datum-bracket", RM_BODY_DATUM, '[' },
        { "datum-brace",   RM_BODY_DATUM, '{' },
        { "string",        RM_BODY_STRING, '"' },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(*table); ++i) {
        size_t n = strlen(table[i].name);
        if (kw->len == n && memcmp(kw->name, table[i].name, n) == 0) {
            *out_mode  = table[i].mode;
            *out_delim = table[i].delim;
            return true;
        }
    }
    return false;
}

int reader_macros_register_from_form(ReaderMacroRegistry *reg, const Form *f) {
    /* Form shape: (reader-macros/define 'name :mode 'template) — 4 items. */
    if (f->as.list.len != 4) {
        diag_emit(DIAG_ERROR, f->span,
                  "reader-macros/define: expected 3 arguments "
                  "(name, mode-keyword, template), got %u",
                  (unsigned)(f->as.list.len - 1));
        return -1;
    }
    const Form *name_arg = unwrap_quote(f->as.list.items[1]);
    const Form *mode_arg = f->as.list.items[2];
    const Form *tmpl_arg = unwrap_quote(f->as.list.items[3]);

    if (!name_arg || name_arg->tag != F_SYM || !name_arg->as.sym) {
        diag_emit(DIAG_ERROR, f->as.list.items[1]->span,
                  "reader-macros/define: first argument must be a quoted "
                  "symbol (the macro name)");
        return -1;
    }
    if (!mode_arg || mode_arg->tag != F_KEYWORD || !mode_arg->as.sym) {
        diag_emit(DIAG_ERROR,
                  mode_arg ? mode_arg->span : f->span,
                  "reader-macros/define: second argument must be a mode "
                  "keyword (:none, :raw-paren, :raw-bracket, :raw-brace, "
                  ":datum-paren, :datum-bracket, :datum-brace, :string)");
        return -1;
    }
    ReaderMacroMode mode;
    int delim;
    if (!keyword_to_mode_delim(mode_arg->as.sym, &mode, &delim)) {
        diag_emit(DIAG_ERROR, mode_arg->span,
                  "reader-macros/define: unknown mode keyword ':%.*s'",
                  (int)mode_arg->as.sym->len, mode_arg->as.sym->name);
        return -1;
    }
    if (!tmpl_arg) {
        diag_emit(DIAG_ERROR, f->span,
                  "reader-macros/define: template argument is required");
        return -1;
    }

    StrSlice name = strslice(name_arg->as.sym->name, name_arg->as.sym->len);
    return reader_macros_register(reg, name, delim, mode,
                                  (Form *)tmpl_arg, f->span);
}

/* ------------------------------------------------------------------------- *
 * Built-in reader macros (see the declaration in reader_macros.h).
 * ------------------------------------------------------------------------- */

/* `#s"text"` reads as `(string/from-cstr "text")`, a fresh owned String,
 * while bare `"text"` stays a borrowed `cstr`.  Was the `stringed` `#lang`
 * layer's reader hook; unconditional since the layer axis was decommissioned.
 *
 * Deliberately REGISTERED rather than added to kReserved above: the REPL's
 * registry is non-strict, so a user `reader-macros/define` still updates `#s"`
 * in place, matching `defn` redefinition semantics.  Reserving it would also
 * turn a pre-existing `#use-reader-macros "stdlib/string-reader.tur"` into a
 * "cannot shadow built-in" error rather than the silent no-op that file is
 * now gutted to be. */
static void install_string_literal_macro(ReaderMacroRegistry *reg,
                                         Arena *arena, SymbolTable *st) {
    StrSlice name = strslice("s", 1);
    /* LOAD-BEARING, and not obvious from the call site -- do not delete this
     * as a redundant guard:
     *
     *   - the interpreter/REPL registry is env-lifetime and reaches this
     *     function on every eval;
     *   - the batch-compile registry is STRICT and is SHARED ACROSS `(load)`:
     *     load_expand_forms passes `e->user_macros` back into
     *     read_all_with_registry (elab_toplevel.c), so a program with one
     *     `(load)` reaches this path twice on the same strict registry.
     *
     * Without the early return the second install is a hard
     * "reader macro '#s' already registered" on every loading program --
     * which is most of them. */
    if (reader_macros_lookup(reg, name, '"')) return;

    /* Build the expansion template `(string/from-cstr $body)`.  `$body` is
     * replaced by a string literal of the `#s"..."` body at dispatch time
     * (reader.c::expand_raw_template). */
    Form **items = (Form **)arena_alloc(arena, sizeof(Form *) * 2);
    items[0] = form_sym(arena, SPAN_UNKNOWN,
                        symtab_intern(st, strslice("string/from-cstr", 16)));
    items[1] = form_sym(arena, SPAN_UNKNOWN,
                        symtab_intern(st, strslice("$body", 5)));
    Form *tmpl = form_list(arena, SPAN_UNKNOWN, items, 2);

    reader_macros_register(reg, name, '"', RM_BODY_STRING, tmpl, SPAN_UNKNOWN);
}

void reader_macros_install_builtins(ReaderMacroRegistry *reg,
                                    Arena *arena, SymbolTable *st) {
    if (!reg || !arena || !st) return;
    install_string_literal_macro(reg, arena, st);
}
