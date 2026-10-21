#ifndef TUR_LSP_SYM_H
#define TUR_LSP_SYM_H

/* What kind of definition a symbol came from.
 *
 * Recorded at collection time, where the defining Expr is still in hand.
 * Everything downstream used to re-derive it from `type_str` -- "does the
 * rendered type start with `(fn`" -- which can only ever answer
 * function-or-not, so a `defstruct` and a `def` were the same entry in an
 * outline and in a completion list.
 *
 * Deliberately not the LSP SymbolKind numbering: the wire enum belongs to
 * lsp.c, and completion items use a *different* enum (CompletionItemKind) for
 * the same distinctions. One neutral tag maps cleanly to both. */
typedef enum {
    LSP_KIND_UNKNOWN = 0, /* not set -- callers fall back to the type_str test */
    LSP_KIND_FUNCTION,    /* defn, and the implicit top-level `main` */
    LSP_KIND_VALUE,       /* def */
    LSP_KIND_STRUCT,      /* a record: defstruct, or a single-ctor defdata */
    LSP_KIND_ENUM,        /* a sum: multi-ctor defdata, or any defgadt */
} LspSymKind;

/* LspSymbol -- one entry in the per-document symbol index.
 * Built by tur_collect_symbols after each compilation pass. */
typedef struct {
    char name[128];      /* interned name */
    char type_str[256];  /* rendered type, e.g. "(fn [int] : int)" */
    char doc[512];       /* first ;;; block above definition, or "" */
    char file_path[512]; /* filesystem path of defining file */
    int  line;           /* 1-based line of the defn/defstruct/... */
    int  col_start;      /* 1-based column, inclusive */
    int  col_end;        /* 1-based column, exclusive */
    LspSymKind kind;     /* what defined it; LSP_KIND_UNKNOWN if unrecorded */
} LspSymbol;

/* Run elaboration on source_path and fill out[0..cap-1].
 * *count_out receives the number of symbols written (capped at cap).
 * Must be called with diag_lsp_begin() active (caller's responsibility).
 * Returns 0 on success.
 *
 * `logical_path` is where the buffer LIVES, when that differs from where it
 * was written -- the LSP analyses a scratch copy of the editor's text, and
 * spice resolution (the enclosing build.tur, its src/, each :spices dep)
 * has to walk up from the real location or a module inside a spice comes
 * back with every intra-spice import unresolved and no symbols at all.
 * NULL when source_path is already the real one. */
int tur_collect_symbols(const char *source_path, const char *logical_path,
                        LspSymbol *out, int cap, int *count_out);

/* Type-check `path` the way `tur check` does, with every registered
 * experiment on, and discard the generated C; returns compile_to_c's status.
 * The diagnostics go to whatever sink the caller set up (the MCP server calls
 * it inside diag_lsp_begin).  Defined by the driver beside
 * tur_collect_symbols; web/wasm_lsp.c supplies a stand-in for the WASM
 * build. */
int tur_check_only(const char *path);

#endif
