/* r7rs_embed.h -- the evaluator behind R7RS `eval` (r7rs-lang-plan T4).
 *
 * `(import (scheme eval))` in a `#lang r7rs` program splices in
 * stdlib/r7rs/eval.tur, whose inline C carries a `__tur_autolink__: -lturi`
 * marker, so a COMPILED program that imports the library links the
 * interpreter and one that does not links nothing new.  `tur --interpret`
 * reaches the same functions through native twins
 * (src/turi/interpreter_natives.c).
 *
 * The evaluator is ONE embedded TuriEnv per process, preloaded with the R7RS
 * prelude and created on the first use.  Values cross between the program
 * and it three ways:
 *
 *   - a DATUM travels as `write` text and is read back on the far side, so
 *     numbers, booleans, characters, strings, symbols, lists and vectors
 *     copy (a pair mutated inside `eval` is a different pair);
 *   - an EMBEDDED procedure (one evaluated code made) stays in the embedded
 *     env and crosses as an id into a table here -- the program calls it
 *     through turi_r7rs_embed_apply;
 *   - a HOST procedure (one the program passed in) stays in the program and
 *     crosses as an id the program assigned -- evaluated code calls it
 *     through a registered native that calls the program's host function,
 *     which reads the arguments with turi_r7rs_embed_frame_* and answers
 *     with one turi_r7rs_embed_answer_*.
 *
 * A result is a KIND byte: 'D' a datum (turi_r7rs_embed_result_text), 'P' an
 * embedded procedure (turi_r7rs_embed_result_id), 'H' a host procedure the
 * program passed in and gets back (result_id), 'U' the unspecified value,
 * 'R' an object evaluated code raised and did not catch (result_text is its
 * condition text, see r7rs-bridge-condition__ in stdlib/r7rs/read.tur), and
 * 'E' an error the evaluator itself reports (result_text is its message).
 *
 * This header is included by emitted C, so it depends on <stdint.h> only. */
#ifndef TURI_R7RS_EMBED_H
#define TURI_R7RS_EMBED_H

#include <stdint.h>

/* The program's side of a host call: `host_id` names the procedure. */
typedef int64_t (*TuriR7rsHostFn)(int64_t host_id);

/* A compiled program's one-time setup: the diagnostics subsystem (stderr),
 * the stdlib root it was built against (TUR_STDLIB_DIR still wins), and the
 * host function.  Idempotent. */
void turi_r7rs_embed_init_program(const char *stdlib_root, TuriR7rsHostFn host);

/* The interpreter's setup: the host function, and the TuriEnv running the
 * program (diagnostics are live), whose elaborator state every crossing
 * restores. */
void turi_r7rs_embed_set_host(TuriR7rsHostFn host, void *host_env);

/* Arguments for the next turi_r7rs_embed_apply, pushed in order.  A datum
 * that does not read is kept as an error the apply reports. */
void turi_r7rs_embed_push_datum(const char *text);
void turi_r7rs_embed_push_host(int64_t host_id);
void turi_r7rs_embed_push_proc(int64_t proc_id);

/* Evaluate `expr_text` in the embedded env with the import sets `sets_text`
 * (a written list of them, e.g. "((scheme base))") in scope.  Returns the
 * result kind. */
int turi_r7rs_embed_eval(const char *sets_text, const char *expr_text);

/* `(load path)`: evaluate every form of the file, as eval does.  Returns the
 * result kind ('U' when it ran). */
int turi_r7rs_embed_load(const char *sets_text, const char *path);

/* Call embedded procedure `proc_id` with the pushed arguments. */
int turi_r7rs_embed_apply(int64_t proc_id);

/* The innermost call from evaluated code to a program procedure that is in
 * progress (0: none), and whether call `serial` still is.  A continuation
 * captured inside such a call cannot be re-entered once the call has
 * returned: the evaluator's frames under it are gone (interpreter_natives.c,
 * r7rs-cont-restore__). */
uint64_t turi_r7rs_embed_call_serial(void);
int      turi_r7rs_embed_call_live(uint64_t serial);

/* For the pool sweep of the embedded env (interpreter_natives.c): calls `fn`
 * on each range of that env's values this file keeps in its own arrays --
 * the procedures handed to the program, the host procedures' wrappers, the
 * pending arguments.  Nothing when `env` is another env. */
typedef void (*TuriR7rsRootFn)(const void *p, uint64_t nbytes, void *ud);
void turi_r7rs_embed_roots(const void *env, TuriR7rsRootFn fn, void *ud);

/* The last result's payload (valid until the next eval/apply/answer). */
const char *turi_r7rs_embed_result_text(void);
int64_t     turi_r7rs_embed_result_id(void);

/* Inside a host call: the arguments, each a kind ('D', 'P' or 'H') with a
 * text or an id. */
int         turi_r7rs_embed_frame_count(void);
int         turi_r7rs_embed_frame_kind(int i);
const char *turi_r7rs_embed_frame_text(int i);
int64_t     turi_r7rs_embed_frame_id(int i);

/* Inside a host call: the answer. */
void turi_r7rs_embed_answer_datum(const char *text);
void turi_r7rs_embed_answer_host(int64_t host_id);
void turi_r7rs_embed_answer_proc(int64_t proc_id);
void turi_r7rs_embed_answer_unspecified(void);
void turi_r7rs_embed_answer_error(const char *msg);
/* A program procedure raised: `condition` is r7rs-bridge-condition__'s
 * text for it, and the embedded side raises it again. */
void turi_r7rs_embed_answer_raise(const char *condition);

#endif /* TURI_R7RS_EMBED_H */
