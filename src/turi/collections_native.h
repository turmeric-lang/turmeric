#ifndef TURI_COLLECTIONS_NATIVE_H
#define TURI_COLLECTIONS_NATIVE_H

/* -------------------------------------------------------------------------
 * Collection native overrides (Vec / Set / Map / HAMT) for the tree-walking
 * interpreter.
 *
 * These implementations back the inline-C bodies of stdlib/vec.tur, set.tur,
 * map.tur and hamt.tur under --interpret.  They were relocated out of the
 * `tur` CLI (main.c) into tur_core so that every consumer of libturi -- the
 * `tur` binary, turi_eval C-API embedders, the WASM REPL, and the interpreter
 * test harnesses -- resolves the same native overrides.  See
 * docs/archive/history/turi-interp-collections-libturi-plan.md.
 * ---------------------------------------------------------------------- */

#include "eval.h"   /* TuriEnv, TuriValue, turi_env_register_native */

/* Register every Vec / Set / Map / HAMT native override on `env`.  Called
 * automatically from turi_env_new so collections are always available to
 * interpreter embedders. */
void turi_register_collection_natives(TuriEnv *env);

/* Exposed for wk_register_sym_natives (interpreter_natives.c): MapKey[Sym] reuses the
 * int-carrier comparator and the cstr box (a Sym is a one-word interned
 * pointer). */
TuriValue native_mk_cmp_int(TuriEnv *env, TuriValue *a, uint32_t n, void *ud);
TuriValue native_mk_box_cstr(TuriEnv *env, TuriValue *a, uint32_t n, void *ud);

#endif /* TURI_COLLECTIONS_NATIVE_H */
