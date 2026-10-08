#ifndef TUR_LSP_UTIL_H
#define TUR_LSP_UTIL_H

#include <stddef.h>

/* Extract the Turmeric identifier that spans the given (1-based) line and
 * column in text.  Writes a NUL-terminated name into out_name[0..name_cap-1].
 * Returns 1 on success (non-empty word found), 0 otherwise.
 *
 * Turmeric identifier characters: [A-Za-z0-9\-?!*\/><+=_] */
int lsp_word_at_pos(const char *text, size_t text_len,
                    int line_1based, int col_1based,
                    char *out_name, size_t name_cap);

/* Byte offset of a (1-based) line/column position in text.  A position past
 * the end of a line clamps to that line's end; a position past the end of the
 * buffer clamps to text_len.  Never fails, so callers get a usable offset for
 * any position a client can send. */
size_t lsp_offset_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based);

/* Extract the partial identifier ending at the cursor -- the completion
 * prefix.  Unlike lsp_word_at_pos this only ever scans *left*: the word the
 * cursor sits before is not what the user is typing.
 *
 * That difference is load-bearing at the start of a buffer.  lsp_word_at_pos
 * steps right when the cursor is not on an identifier character, so a request
 * at {line:0, character:0} of `(defn add ...)` came back with the prefix
 * "defn" and completion filtered every candidate away -- an empty list for the
 * most obvious request a client can make.  Scanning left yields "", which
 * correctly means "no prefix, offer everything".
 *
 * Writes a NUL-terminated (possibly empty) prefix.  Returns its length. */
size_t lsp_prefix_at_pos(const char *text, size_t text_len,
                         int line_1based, int col_1based,
                         char *out_prefix, size_t cap);

/* The call being written at the cursor, for signatureHelp.
 *
 * Scans forward from the start of the buffer -- string- and comment-aware --
 * maintaining a stack of open `(` forms, and reports the head symbol of the
 * innermost one plus which argument the cursor sits in.  Forward scanning is
 * what makes quoting work: a `(` inside a string or a `;` comment never opens
 * a frame, which a backward scan cannot tell without re-reading the file
 * anyway.
 *
 * Returns 1 when the cursor is inside a call with a named head, 0 otherwise
 * (top level, or a head that is itself still being typed).
 * *active_param_out receives the 0-based argument index. */
int lsp_enclosing_call(const char *text, size_t text_len,
                       size_t cursor_off,
                       char *out_name, size_t name_cap,
                       int *active_param_out);

/* The identifier that covers byte offset `off`, as a half-open byte range.
 *
 * Unlike lsp_word_at_pos this answers in offsets rather than in a copied
 * name, which is what an *edit* needs: prepareRename has to hand the client
 * the exact range it is about to rewrite, and a rename has to replace that
 * range rather than re-find the word.  A cursor sitting immediately after an
 * identifier belongs to it (that is where the caret is when the user has just
 * finished typing the name), a cursor on a space does not.
 *
 * Returns 1 and fills the two out params on a hit, 0 otherwise. */
int lsp_ident_range_at(const char *text, size_t text_len, size_t off,
                       size_t *start, size_t *end);

/* Find the form in `text` that pulled `foreign_path` into the compile: a
 * `load`, `import` or `include` line naming it, as `"b.tur"`, `b.tur`, or a
 * module path such as `foo/bar` that resolves to `.../foo/bar.tur`.  The match
 * is a path-component suffix, so `b.tur` does not match `/x/sub.tur`.
 *
 * Returns 1 and the 0-based line and byte-column range of the naming token
 * (quotes included) on a hit; 0 when no form in `text` names the file -- a
 * file loaded by another loaded file, or the auto-loaded stdlib. */
int lsp_reference_anchor(const char *text, size_t text_len,
                         const char *foreign_path, unsigned *line0,
                         unsigned *col_start0, unsigned *col_end0);

/* Build a "file://" URI from an absolute filesystem path.
 * Writes into dest[0..dest_cap-1].  Returns dest. */
char *lsp_path_to_uri(const char *path, char *dest, size_t dest_cap);

/* Reported once per occurrence.  `off` is the match's byte offset in `text`;
 * `line0` is 0-based; `col0` is a *byte* offset into that line, matching the
 * utf-8 positionEncoding the server negotiates.  `len` is the match length in
 * bytes.
 *
 * `off` is what a scope-bounded consumer filters on.  The scan itself always
 * runs from byte zero -- starting it at an offset would put the comment and
 * string state machine in the wrong state, so a match inside a literal that
 * opened earlier in the file would be reported as a use.  Scan everything,
 * report everything, and let the caller drop what is out of range. */
typedef void (*LspOccurrenceFn)(size_t off, int line0, int col0, int len,
                                void *user);

/* Every whole-identifier occurrence of `name` in `text`, skipping the places
 * where a name is not a use of that name.
 *
 * The skipping is the entire value of this over a regular expression, and it
 * is the line c2mp draws too (vite-wasm/src/symbols.js:721): `total` inside
 * `subtotal`, inside a comment, or inside a string literal is not a use of
 * `total`, and a pattern match over the source cannot tell.  Four regions are
 * skipped: `;` line comments, `"..."` string literals (backslash-escaped),
 * `#| ... |#` block comments (nesting), and ```` ```c ... ``` ```` inline-C
 * bodies -- the last because a C identifier that happens to spell a Turmeric
 * one is a different language's variable.
 *
 * Word boundaries use the same identifier character class as
 * lsp_word_at_pos, so a highlight lands on exactly what a hover would. */
void lsp_scan_occurrences(const char *text, size_t text_len,
                          const char *name,
                          LspOccurrenceFn fn, void *user);

#endif
