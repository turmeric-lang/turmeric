# Debugger Phase 3 -- DAP Server over the Interpreter

Status: landed
Track: [debugger-plan.md](./debugger-plan.md), Phase 3

This is the Phase 3 deliverable from [debugger-plan.md](./debugger-plan.md): a
[Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
server over the tree-walking interpreter. `tur dap` speaks DAP (JSON-RPC 2.0
with `Content-Length` framing) on stdin/stdout, so any DAP client -- VS Code,
nvim-dap, etc. -- can launch a `.tur` program under the interpreter and get
breakpoints, stepping, a call stack, locals inspection, and `evaluate`.

It is a thin shell over the Phase 2 interpreter debugger
([debugger-interpreter-phase2.md](./debugger-interpreter-phase2.md)): the
`TuriDebugger` still does the real work; Phase 3 only adds a typed control API
to it and maps DAP requests onto that API.

## Using it

```sh
tur dap        # speaks DAP on stdin/stdout
```

You normally do not run this by hand -- an editor spawns it. The bundled VS Code
wiring lives in [vscode-syntax-ext/](../../vscode-syntax-ext/) (`extension.js`);
install that extension, open a `.tur` file, and press F5. A minimal
`launch.json`:

```jsonc
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "turmeric",
      "request": "launch",
      "name": "Debug current Turmeric file",
      "program": "${file}",
      "stopOnEntry": true
    }
  ]
}
```

## Supported requests / events

| DAP request | Behavior |
| --- | --- |
| `initialize` | reports capabilities, then sends the `initialized` event |
| `launch` | records `program`, `args`, `stopOnEntry` |
| `setBreakpoints` | line breakpoints (per source); supports a `condition` |
| `setExceptionBreakpoints` / `setFunctionBreakpoints` | accepted (no-op) |
| `configurationDone` | runs the program; pauses drive the stops |
| `threads` | a single thread (`id: 1`, "main") |
| `stackTrace` | the live activation stack, innermost first |
| `scopes` | one `Locals` scope per frame |
| `variables` | the frame's lexically-visible bindings, innermost first |
| `evaluate` | resolves a single name in the selected frame (hover / console) |
| `continue` / `next` / `stepIn` / `stepOut` | resume with the matching step mode |
| `pause` | acknowledged (the program is already paused at every stop) |
| `disconnect` / `terminate` | tears the session down |

Events emitted: `initialized`, `stopped` (reason `entry` / `breakpoint` /
`step` / `pause`), `output` (debuggee stdout), `exited`, `terminated`.

### Conditional breakpoints

The Phase 2 debugger never evaluated arbitrary expressions in a paused frame, so
Phase 3 supports the common conditional-breakpoint shape directly: a single
comparison `<name> <op> <literal>` with `op` in `== != < > <= >=`. The name is
resolved in the innermost frame and its rendered value compared to the literal
(string compare for `==`/`!=`, numeric for the ordered operators). A condition
that does not parse, or names a binding not in scope, falls back to "stop" so a
condition we cannot evaluate never silently swallows a breakpoint. Full
expression conditions wait on the in-frame expression evaluator, now tracked in
its own plan: [debugger-inframe-eval-plan.md](./debugger-inframe-eval-plan.md).

## How it works

### Control API (eval.h)

Phase 3 adds a typed control surface to the debugger
(`turi_debug_*` in `src/turi/eval.h`), all operating on the same
`env->debugger`:

- `turi_debug_set_pause_handler` -- install a callback invoked on each stop
  instead of the Phase 2 text REPL. The handler inspects state and resumes.
- `turi_debug_set_cond_handler` -- predicate consulted when a conditional
  breakpoint matches.
- `turi_debug_clear_breakpoints[_for_file]` / `turi_debug_add_breakpoint` --
  breakpoint-table management for `setBreakpoints`.
- `turi_debug_resume_{continue,step_in,step_over,step_out}` -- resume controls
  the pause handler calls to release the eval loop.
- `turi_debug_frame_count` / `turi_debug_frame_at` / `turi_debug_frame_locals`
  / `turi_debug_eval_name` -- stack / locals / evaluate introspection.

The per-node hook (`turi_dbg_before_node`) is unchanged on the hot path: when a
stop fires it now dispatches to `pause_fn` if one is set, else the Phase 2 REPL.
A stop reason (`TuriDbgStop`) is computed alongside the existing hit test and
passed through.

### The server (dap.c)

`src/turi/dap.c` is transport-only. It reuses the LSP module's JSON-RPC I/O
(`lsp_read_message` / `lsp_write_message`, the same `Content-Length` framing)
and flat-JSON readers (`lsp_json_*`), adding a small array iterator for the
`breakpoints` array. The server does not know how to build an interpreter
environment -- the embedder (`src/main.c`) supplies a launch callback that runs
the program via the existing `cmd_eval` debug path, wiring the DAP pause /
condition handlers in through a new `on_ready` hook (fired after the debugger is
enabled, before the program is armed).

### Configuration vs. run phases

`setBreakpoints` typically arrives before the program (hence the env) exists, so
the server stages those breakpoints in the `DapState` during configuration and
flushes them into the debugger in `dap_begin_session`, right before arming.
`setBreakpoints` received *while paused* is applied live instead.

### Program output

The debuggee runs in-process and its `stdout` is the same fd as the JSON-RPC
channel, so the server first `dup`s the real stdout for RPC, then redirects fd 1
to a pipe for the duration of the run. Captured bytes are drained (non-blocking)
at every stop and after exit and forwarded as `output` events. A program that
writes more than the pipe buffer (best-effort 1 MiB) between stops can block
until the next drain -- acceptable for interactive debugging; documented here as
the one caveat.

## Tests

- `tests/run-dap.sh` (ctest target `tur_dap`) drives a scripted JSON-RPC session
  via `tests/dap-driver.py` over `tests/fixtures/dap/input.tur` and asserts the
  handshake, breakpoint verification (plain + conditional), the entry stop,
  `next` / `stepOut`, the two-frame call stack, locals + `evaluate`, a
  conditional breakpoint that fires only at `i == 3`, the forwarded `output`
  event, and the exit code.
- The fixture carries `requires.dedicated-runner`, so `tests/run.sh` PASS-skips
  it (DAP, like `tur debug`, is an interpreter-only path). The driver is
  event-driven (it waits on responses / events, never sleeps), so the transcript
  is deterministic.

## Not in this phase

- Native (`emit-C`) debugging -- Phases 4-5 (gdb/lldb + pretty-printers).
- Setting a variable's value from the client (`setVariable`).
- Expandable structured variables -- structs / options / results render inline
  as a string rather than an expandable tree (their `variablesReference` is 0).
- Full-expression breakpoint conditions and `evaluate` of arbitrary expressions
  (only single-name resolution + simple comparison conditions today). Tracked in
  its own plan: [debugger-inframe-eval-plan.md](./debugger-inframe-eval-plan.md).
