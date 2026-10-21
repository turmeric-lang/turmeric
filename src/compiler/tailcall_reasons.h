/* tailcall_reasons.h -- why a `^tailcall` call is not in tail position, one
 * sentence per context.  Shared by the emitter's tail-position walker
 * (emit_fns.c, TUR-E0716) and the interpreter's syntactic check of the same
 * grammar (turi/eval.c, tc_interp_walk), so a program gets the same words from
 * `tur run` and `tur --interpret`.  A new context goes here, once. */
#ifndef TUR_TAILCALL_REASONS_H
#define TUR_TAILCALL_REASONS_H

#define TC_DEFER_USER "a `defer` written in this block runs AFTER the call, " \
                      "innermost first -- running it before a backedge would " \
                      "reorder what it does, so nothing in the block is in " \
                      "tail position"
#define TC_ARG        "the call sits in an argument position, and arguments are " \
                      "evaluated before the call they belong to"
#define TC_IF_COND    "the condition of an `if` is evaluated before either branch"
#define TC_IF_ONE_ARM "a one-armed `if` (no else branch) is not part of the tail " \
                      "grammar -- give it an else branch"
#define TC_NOT_LAST   "only the LAST form of a `do` block is in tail position"
#define TC_LET_INIT   "a `let` initializer is evaluated before the body"
#define TC_MATCH_SCR  "the scrutinee of a `match` is evaluated before any arm"
#define TC_SC_TEST    "only the LAST operand of `and` / `or` is in tail position; " \
                      "the others are tests evaluated before it"
/* T3 landed, so a `match` ARM is in the tail grammar.  What is left to refuse
 * is the match whose SHAPE emit_tail cannot serve -- see tco_match_tail_ok. */
#define TC_MATCH_ARM  "the enclosing `match` is not one the tail path can " \
                      "serve: it yields no value, has no arms, or offers over " \
                      "a session channel (whose arms are emitted by the " \
                      "session-offer path)"
#define TC_ASCRIBE    "an ascription sits between the call and the return, and " \
                      "is not part of the tail grammar"
#define TC_RETURN     "an explicit `return` form is not part of the tail grammar " \
                      "-- make the call the block's value instead"
#define TC_WHILE      "a `while` body is not in tail position; its value is " \
                      "discarded and the loop continues"
#define TC_GUARD      "a `when`-guard is evaluated before the arm it guards"
#define TC_CPS_BODY   "the enclosing function is lowered through the CPS " \
                      "backend -- it is effect-colored, or reaches a " \
                      "delimited-control construct -- and that backend emits " \
                      "every call as an ordinary call followed by a " \
                      "continuation invocation"

#endif /* TUR_TAILCALL_REASONS_H */
