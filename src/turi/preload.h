/* preload.h -- shared stdlib preload sequence for interpreter entry points.
 *
 * The tree-walking interpreter starts from a bare `TuriEnv` that only knows the
 * elaborator builtins (`+`, `let`, `if`, `fn`, ...).  Every real program needs
 * the core macros (`when`/`cond`/`for`/`and`/`or`) and the typed-collection
 * stdlib (Map/Set/Vec/Option/Result/...) loaded on top of that env before user
 * code runs.  The `--interpret` entry point (`cmd_eval_h`) has always
 * done this inline; the WASM/browser REPL (`src/web/wasm_glue.c`) skipped it,
 * so reader-macro data literals such as `#map{...}` (which lower to the
 * `hamt-of` macro defined in `stdlib/map.tur`) failed with "unknown function or
 * operator 'hamt-of'".  See docs/archive/history/web-repl-missing-stdlib-preload.md.
 *
 * These two helpers factor that load sequence out of the driver so the native
 * interpreter and the WASM REPL share one copy and cannot drift.  They only
 * emit `(load "<root>/<file>")` forms; the underlying Vec/Set/Map/HAMT runtime
 * ops are already registered as natives by `turi_env_new`
 * (`turi_register_collection_natives`), so no extra native wiring is required
 * for the collection literals to evaluate.
 */
#ifndef TURI_PRELOAD_H
#define TURI_PRELOAD_H

#include "turi/env.h"

/* Load `macros.tur` then `contract.tur`, each in its own `turi_eval` call so
 * each file gets a distinct file_id (the per-file "one defmodule" reset fires)
 * and both land at the front of the accumulated source, where the Phase M7
 * macro-promotion makes their `tur/`-prefixed macros globally visible without
 * an explicit import.  `stdlib_root` is prepended to each basename (e.g. an
 * absolute path from `tur_stdlib_path`, or the FS-relative "stdlib" the WASM
 * MEMFS embed uses); NULL/"" defaults to "stdlib". */
void turi_env_preload_macros(TuriEnv *env, const char *stdlib_root);

/* Inject the typed native-function stubs (nil-value/cons/head/tail, the
 * benchmark helpers, none?, ...) the elaborator needs to type a bare call to a
 * runtime native.  Critically, `cons`/`head`/`tail` are otherwise elaborator
 * builtins the tree-walker's eval_builtin cannot execute (it returns nil),
 * which is the REPL-only `(list-head (cons 65 ...)) => nil` bug.  Call this
 * AFTER turi_env_preload_macros and BEFORE turi_env_preload_collections, and
 * before turi_env_register_interpreter_natives registers the shims that
 * override each stub body at runtime.  Takes no stdlib_root -- it emits only
 * `(defn ...)` forms.  Shared by `--interpret` (cmd_eval) and the REPL so the
 * two entry points cannot drift. */
void turi_env_preload_native_stubs(TuriEnv *env);

/* Load the typeclass-stub + typed-collection stdlib set (safe, typeclass-*,
 * hamt/set/map, vec/slice, option/result, pair/tuple/list, grid/zipper, mutmap,
 * unique, sym) as one batched `(load A)(load B)...` form so the whole prelude
 * elaborates in a single `turi_eval` with deps resolved in order.  Mirrors the
 * array `cmd_eval` loads; keep the two in sync.  `stdlib_root` as above. */
void turi_env_preload_collections(TuriEnv *env, const char *stdlib_root);

/* Load `stdlib/typeclass-show.tur` (the Show class, its primitive instances,
 * and the Show[Vec]/Show[Set]/Show[Map] collection instances).  This is a
 * REPL-only convenience: it lets the interactive prompt render a collection
 * result through its Show instance (turi_try_show_by_tag) and resolve a bare
 * `(show x)` without an explicit `(load ...)`.
 *
 * It is deliberately NOT part of turi_env_preload_collections: that helper
 * backs `--interpret` and the fixture worker too, and globally installing the
 * Show instances there would change instance resolution for programs that
 * define their own Show class or assert a "missing instance" error.  Only the
 * Show slice is loaded (not the full typeclass.tur) so the interpreter's async
 * error-message builtin is not shadowed by typeclass.tur's inline-C
 * Error[ptr<void>] instance.  Call this only from the interactive REPL, after
 * turi_env_preload_collections.  `stdlib_root` as above. */
void turi_env_preload_typeclasses(TuriEnv *env, const char *stdlib_root);

#endif /* TURI_PRELOAD_H */
