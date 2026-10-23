#ifndef TUR_STDLIB_AUTOLOAD_H
#define TUR_STDLIB_AUTOLOAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "arena.h"
#include "forms.h"
#include "symbols.h"
#include "diag.h"   /* SourceFile */

/* -------------------------------------------------------------------------
 * Stdlib autoload
 *
 * Turmeric source sees `Cons`, `Option`, `when`, `assert!` and the rest
 * without importing anything, because every compilation unit gets the stdlib's
 * forms prepended to it. That prepend is what this file owns.
 *
 * It used to live in main.c alongside the CLI. That was fine while `tur` was
 * the only front end; it stopped being fine the moment a second one appeared
 * (the WASM playground's in-process analyzer, src/web/wasm_lsp.c), because a
 * second front end that does not prepend the same list is a second front end
 * that disagrees with the compiler about what is in scope -- and the way that
 * surfaces is a browser reporting "unknown function `cons`" on code the
 * compiler accepts.
 *
 * The stdlib *directory* is a parameter rather than something resolved here:
 * the CLI finds it by walking up from the executable, and the browser has it
 * mounted at a fixed path in its virtual filesystem. Neither resolution belongs
 * to the prepend.
 * --------------------------------------------------------------------- */

/* NULL-terminated list of stdlib basenames, in load order. Order is
 * load-bearing -- macros.tur first, and the typeclass files before the
 * containers that instantiate them. */
const char *const *tur_stdlib_autoload_files(void);

/* quote-list-data-plan: does the entry program quote a list or vector under
 * --enable=quote-data, so stdlib/datum.tur should join the autoload?  Over
 * read forms (the compiled paths) or source text (the interpreter, which
 * preloads before it reads).  Both answer false for a Scheme program. */
bool tur_quote_data_wanted_forms(Form *const *forms, uint32_t n);
bool tur_quote_data_wanted_source(const char *src, size_t len);

/* Read every autoload file out of `stdlib_dir`, parse it, and prepend the
 * resulting forms onto *forms_in_out (reallocating into `arena`). Updates
 * *nforms_in_out and the running *file_id_in_out counter.
 *
 * Returns the number of prepended forms, which the caller passes to
 * elaborate_program as `stdlib_prefix` so the elab loop can bracket them with
 * `in_stdlib_load = true` -- that is what sets `is_from_stdlib` on the
 * resulting bindings, which is in turn what the separate-compilation emit path
 * keys off to static-ify stdlib defns per TU.
 *
 * `entry_path` is the user-visible input file. When `no_auto_stdlib` is set
 * and the entry file IS one of the autoload files, everything from that file
 * onward is skipped -- the file cannot be autoloaded ahead of itself.
 *
 * Files that cannot be read are skipped silently: a partial stdlib is the
 * caller's problem to diagnose, and every missing file would otherwise print
 * during ordinary `--no-auto-stdlib` builds.
 */
/* Apply an autoloaded stdlib file's `#lang` header to its SourceFile.
 *
 * `sf` must already carry `src`/`len` for the WHOLE file and a default
 * `reader_type`; on a directive this advances them past the header and records
 * the reader, language and layer set.  A file with no directive is left alone.
 *
 * EXISTS BECAUSE THE SAME BLOCK WAS WRITTEN TWICE AND MISSED TWICE MORE. Four
 * separate loaders have now been found constructing a SourceFile with a
 * hardcoded `READER_TURMERIC` and no detection, each breaking on `#lang` and
 * each fixed only when someone walked that particular path:
 *
 *   - the single-file stdlib autoload (S6)
 *   - the module import path (S7) -- a Saffron module could not be imported
 *   - `tur fmt` -- rejected every `#lang` file
 *   - `load_project_prelude` (S8) -- a Saffron LIBRARY could not build, while
 *     the binary path could, because they use different loaders
 *
 * The two STDLIB loaders had identical fourteen-line copies; they call this
 * instead.  `elab_module.c`'s import path deliberately does NOT: it carries an
 * extension override and its own diagnostics for a bad layer and an
 * unimplemented reader, which a stdlib file has no use for. */
void tur_source_file_apply_lang_header(SourceFile *sf, const char *whole_src,
                                       size_t whole_len);

uint32_t tur_stdlib_prepend_forms(Arena *arena, SymbolTable *st,
                                  const char *stdlib_dir,
                                  const char *entry_path,
                                  bool no_auto_stdlib,
                                  Form ***forms_in_out,
                                  uint32_t *nforms_in_out,
                                  uint8_t *file_id_in_out);

#endif
