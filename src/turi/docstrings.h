/* docstrings.h -- read stdlib/docstrings.tur's doc table directly from C.
 *
 * tools/gendocs.py --emit-tur generates stdlib/docstrings.tur, whose
 * `doc-lookup` is an inline-C body holding the whole table as a C array.  The
 * tree-walking interpreter cannot run inline C, so the table used to be
 * reachable from C only by booting a compile (or, in the playground, by
 * evaluating `(doc-lookup "name")` INTO the user's session, which failed and
 * poisoned it -- doc-lookup-poisons-the-playground-eval-session).  This reads
 * the generated file itself, once, and answers lookups from memory.
 *
 * Consumers: `tur doc`, the playground doc panel (turi_doc_lookup in
 * src/web/wasm_glue.c), and the `doc-lookup` / `doc-print` interpreter
 * natives behind the `doc` macro.
 */
#ifndef TURI_DOCSTRINGS_H
#define TURI_DOCSTRINGS_H

/* Look up `name` in the docstrings file at `path`.  Returns the docstring, or
 * NULL when the file has no entry for it or cannot be read.  The string is
 * owned by a process-lifetime cache: do not free it.  The cache holds one file
 * at a time; asking for a different path reloads it.  Not thread-safe. */
const char *tur_docstring_lookup_in(const char *path, const char *name);

/* As above, against <stdlib>/docstrings.tur, where <stdlib> is
 * $TUR_STDLIB_DIR when set and "stdlib" otherwise -- the same resolution the
 * elaborator uses for `(load "stdlib/...")`. */
const char *tur_docstring_lookup(const char *name);

/* Look up the short doc string for a builtin operator or special form (`+`,
 * `let`, ...).  Returns a static string, or NULL if `sym` is not one.  Also
 * declared in turi/repl.h, which has always exported it. */
const char *turi_doc_lookup_builtin(const char *sym);

/* The documentation to show for `name`, given its stdlib-table entry
 * (`from_stdlib`, NULL when there is none): the stdlib entry, which carries
 * signature, Parameters, Returns and Example, else the builtin one-liner.
 * A stdlib entry that is only the bare name -- a definition with no docstring,
 * such as macros.tur's `when` -- says less than the one-liner, so the
 * one-liner wins there.  NULL when neither has anything.  Every doc front end
 * (`tur doc`, `doc-print`, the REPL's `:doc`, the playground panel) answers
 * through this, so they cannot disagree. */
const char *tur_doc_prefer(const char *name, const char *from_stdlib);

#endif /* TURI_DOCSTRINGS_H */
