/* interpreter_natives.h -- interpreter native overrides for stdlib inline-C ops.
 *
 * The tree-walking interpreter cannot execute inline-C function bodies, so the
 * stdlib ops whose bodies are inline-C (sym/:Sym, contracts, json, schema, seq,
 * safe box/unbox, comonad, mutex, future, chan, taskgroup, backtrack, process/
 * fs, serial, bytes, and the option/result/list/grid/mutmap/... families) are
 * overridden by layout-exact C natives.  This block used to live only in
 * src/main.c, so the two interpreter entry points that do not link main.c --
 * the WASM REPL (src/web/wasm_glue.c) and the interactive `tur repl`
 * (src/turi/repl.c) -- could not evaluate any op that resolved to one of these
 * natives (`eval: inline-C not supported in interpreter mode`).
 *
 * Relocating it into tur_core (mirroring turi/collections_native.c and
 * turi/preload.c) exposes turi_env_register_interpreter_natives(env), which the
 * three entry points call after turi_env_preload_*.  See
 * docs/archive/history/web-repl-repl-inline-c-native-gap.md.
 */
#ifndef TURI_INTERPRETER_NATIVES_H
#define TURI_INTERPRETER_NATIVES_H

#include <stdint.h>

#include "turi/eval.h"

/* Register the full set of interpreter native overrides onto `env`.  Call
 * AFTER turi_env_preload_macros / turi_env_preload_collections so the native
 * shims win over the loaded (inline-C) stdlib bodies. */
void turi_env_register_interpreter_natives(TuriEnv *env);

/* Individually exposed for the forked fixture-runner worker in src/main.c
 * (wk_eval_fixture), which registers a narrower subset than the full sequence
 * above and threads its own preload. */
void wk_register_stdlib_natives(TuriEnv *env);

/* The json/decode native's parser, callable without an env: the decoded node
 * tree (layout-exact with stdlib/json.tur's), or 0 on a parse error.
 * turi_json_free_tree releases one -- the json/free native is a no-op under
 * the interpreter's process-lifetime policy.  Exposed for tests/fuzz
 * (security-audit-plan WP4). */
int64_t turi_json_decode_cstr(const char *s);
void    turi_json_free_tree(int64_t node);
void wk_register_safe_natives(TuriEnv *env);
void wk_register_typeclass_natives(TuriEnv *env);
TuriValue native_contract_check(TuriEnv *env, TuriValue *args,
                                uint32_t n, void *ud);
TuriValue native_contract_check_at(TuriEnv *env, TuriValue *args,
                                   uint32_t n, void *ud);
TuriValue native_contract_check_inv(TuriEnv *env, TuriValue *args,
                                    uint32_t n, void *ud);
TuriValue native_contract_enabled(TuriEnv *env, TuriValue *args,
                                  uint32_t n, void *ud);

#endif /* TURI_INTERPRETER_NATIVES_H */
