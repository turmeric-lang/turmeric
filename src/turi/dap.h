#ifndef TURI_DAP_H
#define TURI_DAP_H

#include "turi/env.h"

/* ---------------------------------------------------------------------------
 * Debugger Phase 3: Debug Adapter Protocol server over the interpreter.
 *
 * `tur dap` speaks the Debug Adapter Protocol (JSON-RPC 2.0 with Content-Length
 * framing, the same transport as `tur lsp`) on stdin/stdout, driving the Phase 2
 * TuriDebugger through its Phase 3 control API (eval.h).  Any DAP client -- VS
 * Code, nvim-dap, etc. -- can launch a .tur program under the interpreter and
 * get breakpoints / stepping / call-stack / locals.
 *
 * The server is transport-only: it does not know how to construct an interpreter
 * environment.  The embedder supplies a launch callback that builds the env,
 * calls dap_begin_session() once the debugger is enabled, runs the program, and
 * returns its exit code.  See dap_launch_cb (src/driver/cmd_eval.c).
 * --------------------------------------------------------------------------- */

/* Embedder-provided launcher.  Invoked by the server after the client has sent
 * `launch` and `configurationDone`.  It must:
 *   1. construct the interpreter env for `program` (with `args`/`n_args`),
 *   2. enable the debugger and call dap_begin_session(state, env) BEFORE arming,
 *   3. arm and run main to completion,
 *   4. call dap_end_session(state, env) before freeing the env,
 *   5. return the program's exit code.
 * `state` is the opaque DapState handle to forward to dap_begin_session. */
typedef int (*DapLaunchFn)(const char *program, char **args, int n_args,
                           void *state, void *ud);

/* Run the DAP server loop over the given fds.  Returns 0 on clean disconnect. */
int dap_server_run(int in_fd, int out_fd, DapLaunchFn launch, void *ud);

/* Called by the launch callback after the debugger is enabled but before the
 * program is armed: records the env, installs the DAP pause / conditional
 * handlers, and flushes the client's staged breakpoints into the debugger. */
void dap_begin_session(void *state, TuriEnv *env);

/* Called by the launch callback immediately before the env is freed. In a
 * replay session the recorder's pause handler lives on the env and has to come
 * off while the env is still there; the recording outlives it. A no-op for a
 * live session. */
void dap_end_session(void *state, TuriEnv *env);

#endif
