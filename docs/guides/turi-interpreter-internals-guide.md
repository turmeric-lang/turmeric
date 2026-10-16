---
title: The turi Interpreter (internals)
category: Compiler Internals
description: How the tree-walking interpreter works -- its entry points, the shared front end it runs, value and frame representations, the explicit-stack driver, native overrides and inline-C, the two effect runtimes, what memory it gives back and what it keeps, process-global hazards, and how to add a form or native
---

# The turi Interpreter (internals)

This guide is for contributors changing `src/turi/`: the tree-walking
interpreter behind `tur interpret`, `tur repl`, `tur debug`/`dap`/`trace`,
`tur run --engine interp`, the WASM playground, the libturi embedding API and
macro-time evaluation. It describes the code as it stands.

Related guides, not repeated here:

- [turi-parity-guide](turi-parity-guide.md) -- the feature matrix and the
  rules for keeping the entry points in sync.
- [eval-api](eval-api.md) -- the public C embedding API and sandboxing.
- [repl](repl.md) -- the interactive prompt as a user sees it.
- [gc-guide](gc-guide.md) and [memory-usage-guide](memory-usage-guide.md) --
  memory from the program's side.
- [r7rs-implementation-guide](r7rs-implementation-guide.md) -- the Scheme
  prelude whose `call/cc` drives the continuation sweep below.
- [cps-dk-internals-guide](cps-dk-internals-guide.md) -- the compiled
  path's effect machine, which this one mirrors.

> **Trust the code over the header comments.** Several comments in `eval.c`
> predate the driver and the 2026-10 reclamation work. The file header still
> says every `turi_eval` "re-elaborates ALL accumulated source" (incremental
> elaboration has been the default since 2026-07-25). The frame section is
> titled "stack-allocated linked list" (frames come from the value pool and
> free lists). `eval_frame_free` and the `EX_CLOSURE` arm say frames are
> never freed (call frames are, since 2026-10-05). Where a comment and this
> guide disagree, read the code; where the code disagrees with this guide,
> fix the guide.

---

## The short version

1. **Same front end.** Every entry point ends in `turi_eval_impl`
   (`src/turi/eval.c`). It parses, runs the same elaborator as the compiled
   path (`elaborate_program_session`), then `effect_check_pass` and
   `lifetime_check_program`. It does **not** run `PASS_CPS`, any emitter, or
   a C compiler. The interpreter walks the elaborated `Expr` tree directly.
2. **Values** are a 16-byte tagged union, `TuriValue` (`src/turi/value.h`).
   Structs and ADT values are heap `TuriStruct`s. Most stdlib handles (Vec,
   Map, Set, HAMT, channels, cons cells) are raw C pointers in a
   `TURI_INT`.
3. **Environments.** Globals live in a hash table of `EnvBinding`s keyed by
   name. Locals live in a chain of `EvalFrame`s, and are looked up **by name
   string**, not by `Binding*`.
4. **Evaluation** is an explicit-stack CEK-style machine, `eval_drive_ex`:
   - `control` is the expression;
   - `cf` is the frame;
   - the work-stack `st` of `DriveCont` entries is the continuation.

   Calls, tail calls, `let`, `match`, `while`, handlers, `reset`, `call/cc`,
   `catch-unwind` and `atomically` all fold onto it. Anything else is
   evaluated as a black box by the recursive `eval_expr_impl`.
5. **Natives.** Stdlib functions whose bodies are inline C are replaced at
   runtime by C natives registered under the same name. User inline C runs
   only if a small pattern executor recognises the body, or under
   `--enable=repl-jit-inline-c`; otherwise it is a clean error.
6. **Effects** have two runtimes:
   - a capturable `handle` runs on the work-stack behind a `DK_PROMPT`, and
     `perform` captures a heap `TuriWsCont` (multishot, escaping, nested);
   - anything else falls back to a ucontext fiber, which is single-shot.
7. **Memory** is process-lifetime by default: arenas, a value pool, and
   `malloc`. Since 2026-10, call frames, their bindings and pins are handed
   back on return. In `#lang r7rs` sessions, a conservative sweep also hands
   back dropped closures, frames, structs and `call/cc` images.

---

## Source map

| File | Role |
|---|---|
| `src/turi/eval.c` (~18k lines) | Everything evaluative: `turi_eval_impl`, the driver `eval_drive_ex`, `eval_expr_impl`, application, effects, continuations, generators, STM, sessions, the debugger, scratch promotion, the continuation/pool sweep marker |
| `src/turi/eval.h` | Public API: `turi_eval*`, `turi_call`, native registration, caps, debugger control, the `turi_cont_*` sweep interface |
| `src/turi/value.h`, `value.c` | `TuriValue`, the value pool (`turi_val_*`), the pool's object map for the sweeps |
| `src/turi/env.h`, `env.c` | `TuriEnv` (every piece of interpreter state), globals hash table, `turi_env_new`/`free`, prelude pin/rewind |
| `src/turi/preload.c`, `.h` | The shared stdlib preload: `turi_env_preload_macros`, `_native_stubs`, `_collections`, `_typeclasses`, and the `prelude[]` module list |
| `src/turi/interpreter_natives.c`, `.h` (~7k lines) | `turi_env_register_interpreter_natives` and the `wk_register_*_natives` families; the R7RS natives, including `call/cc` capture and the sweep driver |
| `src/turi/collections_native.c` | Vec/Set/Map/HAMT natives (`turi_register_collection_natives`) |
| `src/turi/string_native.c` | The owned `String` natives |
| `src/turi/native_caps.c` | Per-native capability and handle-signature tables (sandboxing, provenance) |
| `src/turi/fiber.c`, `.h` | Async scheduler: `TuriFiber`, futures, timers, `poll(2)` I/O |
| `src/turi/repl.c` | `tur repl` (`turi_repl_run`) |
| `src/turi/macro_env.c` | The per-compile macro-time env (`elab_macro_env_get`) |
| `src/turi/trace.c`, `dap.c` | `tur trace` recorder and the `tur dap` server |
| `src/turi/inline_c_jit.c` | `repl-jit-inline-c`: compile a refused inline-C defn on first call |
| `src/turi/jit_ffi.c`, `jit_ffi_hook.c`, `ffi_thunk.c` | c2mir call thunks for `extern-c`, `call-ptr` and spice images |
| `src/turi/spice_loader.c` | Load a spice's AOT-compiled image into an env |
| `src/turi/r7rs_embed.c` | The single embedded env behind R7RS `(scheme eval)` |
| `src/turi/docstrings.c` | `doc` lookup natives |
| `src/main.c` | `cmd_eval_h` (the `interpret`/`debug`/`dap`/`trace` entry), `run_delegate_engine` (`--engine interp`), `tur eval`, the forked `tur worker` |

Two naming traps:

- The driver's `DriveKind` values are spelled `DK_DONE`, `DK_PROMPT`,
  `DK_CALL_RET`, ... They have nothing to do with the compiled path's `DK`
  struct and its `DKK_*` node kinds, though `DK_PROMPT` and
  `DK_NATIVE_RESUME` play the same roles as `DKK_HANDLER` and `DKK_FRAME`.
- `eval_frame_free` is a no-op kept for history. Frames are released by
  `frame_release`.

---

## Entry points

| Entry | Path into the interpreter |
|---|---|
| `tur interpret f.tur` (also `--interpret`) | `main.c:cmd_eval_inc` -> `cmd_eval_h` |
| `tur run --engine interp` (`TUR_ENGINE=interp`, `:engine "interp"`) | `run_delegate_engine` -> `cmd_eval_inc` |
| `tur debug`, `tur dap`, `tur trace` | `cmd_eval_h` with `debug = true` and `EvalHooks` |
| `tur repl` | `repl.c:turi_repl_run`, `repl_preload_stdlib_and_natives` |
| WASM playground | `src/web/wasm_glue.c` calling `turi_eval` |
| libturi embedders | `turi_env_new` (or `_sandboxed`, `_with_natives`) then `turi_eval`/`turi_call` |
| Macro time | `macro_env.c:elab_macro_env_get`, for `defmacro*` bodies |
| R7RS `(scheme eval)` | `r7rs_embed.c`: one embedded env per process |
| `tur worker` | `main.c:wk_eval_fixture`, a forked child per fixture, with its own narrower native set |

### What `cmd_eval_h` does

`cmd_eval_h` is the reference sequence. The REPL and the macro env follow
the same order; read it before adding a step.

1. Set `g_interpret_mode = true`, `turi_init`, `turi_env_new`.
2. Make the env's reader-macro registry strict (a duplicate registration is
   an error, as compiled; the REPL keeps it lax).
3. Pre-read the file's `#lang` line (or `.scm` extension) and seed
   `env->reader_type`, `env->lang` and `g_lang_prelude`, so the stdlib
   prelude is parsed under the same reader as the user file. Otherwise the
   user file's directive would reset the accumulated source and drop the
   prelude.
4. `turi_env_preload_macros`: `macros.tur`, then `contract.tur`, each in its
   own `turi_eval` so each gets a `file_id` and the M7 macro promotion sees
   them first.
5. `turi_env_preload_native_stubs`: typed `(defn ...)` stubs that let the
   elaborator type calls to natives that have no stdlib definition
   (`println-float`, `int->float`, `none?`, `doc-print`, the benchmark
   helpers).
6. `turi_env_preload_collections`: one batched `(load ...)` blob of the
   `prelude[]` list in `preload.c` (typeclasses, hamt/set/map, vec, option,
   result, list, `sym.tur`, `trail.tur`, `region.tur`, ...). Run under
   `g_turi_stdlib_preload = true`, which stamps definitions as stdlib-owned.
7. Load `json.tur` and `schema.tur` (the `#json` readers' runtime).
8. `turi_env_pin_prelude`, so a later `#lang` switch rewinds to here instead
   of to empty.
9. `turi_env_register_interpreter_natives`, then
   `turi_register_collection_natives` **again**. Natives must be registered
   after the preload, because the preload's `defn`s overwrite globals of the
   same name (see [Override order](#override-order)).
10. Bind `*args*` (a malloc'd cons list of `argv` strings) and `*argv0*`;
    set `module_base_dir` (through the setter) and the include path (`-I`,
    then the spice walk-up, `auto_append_spice_includes`).
11. Register `(break)`; under `debug`, enable the debugger and run the
    `on_ready` hook.
12. Evaluate `(load "<path>")`, not the file's text, so the user file keeps
    its own `file_id` and real line numbers.
13. If the file defined `main`, `turi_call` it. An `int` result is the exit
    code; an error value is printed as `tur: <msg>`.
14. `turi_run_pending_defers`, `on_done`, `turi_env_free`, free `*args*`.

---

## One turn: `turi_eval_impl`

Every `turi_eval*` call is a *turn*. `turi_eval_with_sink` wraps it: it
installs the env's diagnostic sink and snapshots `g_interpret_mode` from
`env->interpret_mode` for the call. `eval_toplevel_guarded` adds a host-exit
landing pad for envs without `TURI_CAP_PROC` (see `host_guarded_run`).

A turn:

1. Strips a shebang line; detects `#lang` (rejecting unknown bases with
   TUR-E0331 and trailing tokens with TUR-E0330, as compiled). A reader
   change calls `turi_env_reset_to_prelude`.
2. Builds the combined source, `src_acc + "\n" + new`, in the reused
   `env->src_combined` buffer, and allocates a fresh eval arena
   (`ArenaNode`, linked on `env->eval_arenas`).
3. Saves the diagnostic file registry, `diag_reset`s, registers the blob as
   `file_id` 0, and restores the saved registry. Reused `Form`s from earlier
   turns still carry their `(load ...)`ed files' ids.
4. Parses. With `incremental_elab` (default on), only the new tail is read
   and earlier turns' `Form`s are reused from `env->acc_forms`. Sweet-exp
   readers and some other turns fall back to a whole-blob parse.
5. Elaborates with `elaborate_program_session`. With a live
   `env->elab_session`, it hands over only this turn's forms; earlier
   definitions resolve from the session's accumulated scope. A session
   discarded after a failed turn is rebuilt by `elab_session_replay`, which
   walks `env->acc_turns` turn by turn.
6. Runs `effect_check_pass` with a **session-scoped** `EffectEnv`
   (`env->effect_env`), so effects declared in earlier turns resolve.
7. Runs `lifetime_check_program`.
8. Publishes the turn's `TypeClassEnv` to `env->last_tc_env` **before**
   evaluating; runtime dictionary dispatch reads it.
9. Evaluates program items: first the file-scope defs (`[0, n_fsd)`,
   imported modules), then the new items. On the whole-program path the
   already-run prefix is skipped by `prior_prog_items`, a count of
   *program items*, not parsed forms.
10. On success, appends the source to `src_acc` and commits the counters and
    `acc_turns`. On failure, uncommits this turn's forms and discards the
    elaboration session.
11. `turi_promote_escaping` (a no-op unless scratch promotion is on).

### What the interpreter shares, and what it skips

| Compiled stage (`main.c:core_passes[]`) | Under the interpreter |
|---|---|
| Reader, macro expander, `(load ...)` splicing | Same code |
| `PASS_ELABORATE` | Same elaborator, session variant |
| `PASS_KIND_CHECK` (`kind_check_pass`) | Not run as a pass. The kind-error fixtures still fail under `--interpret`, because the elaborator raises them too |
| `PASS_EFFECT_LOWER` | Only its `EffectEnv` setup; `effect_lower` is a no-op anyway |
| `PASS_EFFECT_ROW_INFER` | `effect_check_pass` |
| `PASS_CPS` | Not run |
| `PASS_BORROW_CHECK` | Only `lifetime_check_program`. `borrow_check_program` is disabled process-wide (`borrow_check_enabled` is never set), so skipping it costs no parity. `borrow_check_effect_handler_captures` is **not** run, so a handler case that reads an outer borrow is rejected compiled but runs here: `tests/fixtures/errors/effect-handler-borrow` prints 42 under `--interpret` instead of its diagnostic |
| Emission, cc, link | Not run |

The elaborator itself branches on `g_interpret_mode` in a few places. That
list is in [Process-global state](#process-global-state).

---

## Values

```c
typedef struct TuriValue { TuriTag tag; union { ... }; } TuriValue;
```

| Tag | Payload | Notes |
|---|---|---|
| `TURI_NIL`, `TURI_BOOL`, `TURI_INT`, `TURI_FLOAT` | immediate | `TURI_INT` is also the **carrier** for every pointer-shaped value the stdlib passes as `int64_t` |
| `TURI_CSTR` | `const char *` | Borrowed or malloc'd; never freed by the interpreter |
| `TURI_CLOSURE` | `TuriClosure *` | Turmeric closures **and** natives |
| `TURI_STRUCT` | `TuriStruct *` | Structs, ADT values, `rc` wrappers, `any` boxes |
| `TURI_STRUCT_TYPE` | name in `as_cstr` | A struct type descriptor |
| `TURI_EFFECT_CONT` | `TuriEffectCont *` | A handler's `k`. `ws != NULL` means a work-stack continuation |
| `TURI_HANDLER` | `TuriHandlerVal *` | First-class handler value |
| `TURI_GEN` | `TuriGen *` | Generator |
| `TURI_FUTURE` | `TuriFuture *` | Async future |
| `TURI_REF` | `EvalBinding *` | A `&mut` borrow of a local |
| `TURI_SYNTAX` | `Form *` | A syntax object (macro-time) |
| `TURI_ERROR` | message | Propagates: almost every form returns it unchanged |
| `TURI_REJECTION` | message | A rejected future. Deliberately **not** `turi_is_error`, so it does not short-circuit |
| `TURI_THROW` | `TuriThrow *` | Retired with `throw`; no path makes one |

Errors are values, not `longjmp`s. A form that gets a `TURI_ERROR` from a
child returns it, and the error unwinds up the driver like a signal (see
[Signals](#signals)).

### Structs and ADTs

```c
struct TuriStruct {
    const char *name;  uint32_t n_fields;  uint32_t epoch;
    TuriValue  *fields;  bool is_any_box;  const CtorDef *ctor;
};
```

- **Constructors are natives.** `EX_DEFDATA`/`EX_DEFGADT` register every
  constructor under its name as `adt_ctor_native`, with the `CtorDef` as the
  native's `ud`. Calling it builds a `TuriStruct` named after the
  constructor, with `ctor` set. Constructor names share the flat global
  namespace.
- **Matching compares names.** `eval_match_resolve_with` matches a
  constructor pattern by `strcmp(val.as_struct->name, ctor->name)`.
- **`defstruct`** values come from `EX_MAKE_STRUCT` (`DK_MAKE_STRUCT`), or
  through a single-variant record ADT when the struct was lowered to one;
  `ctor` recovers field names either way.
- **`rc<T>`** is a struct named `"__rc"` whose field 0 is a pointer to the
  count. `rc/clone`, `rc/drop`, `rc/strong-count` and the `EX_CLOSURE`
  capture all recognise it by name.
- **`any`** widening wraps a carrier in a struct with `is_any_box`, so
  `type-of`/`is?`/`cast` can name a type the bare word cannot.
- **By-value parameters are copied.** `turi_copy_byvalue_struct_arg` copies
  a struct argument as it is bound, recursing through by-value fields and
  stopping at `__rc`, `:heap` and non-struct values. It skips the copy when
  no write can be observed: the type's `AdtDef.field_written` is unset
  ([turi-immutable-struct-args-copied-per-call](../archive/turi-immutable-struct-args-copied-per-call.md)).

### Closures and natives

```c
struct TuriClosure {
    FnDef *fn;  EvalFrame *captured;      /* Turmeric closure */
    TuriNativeFn native;  void *native_ud; /* native when non-NULL */
    TuriCaps native_caps;  const char *native_cap_name;
    const TuriNativeHandleRow *native_handle;
    bool skip_env_param;  uint32_t epoch;  const char *module;
    TuriEnv *origin_env;                   /* !NDEBUG only */
};
```

- A `defn` is a closure with `captured == NULL` and `module` set to its
  defining module.
- `fn` and `EX_CLOSURE` capture the **current frame** whole and mark it
  escaped (`frame_escape`). The elaborator's capture list is ignored except
  for one thing: an `EX_CLOSURE` bumps the count of every captured `__rc`,
  to match the compiled closure env's retain.
- `EX_CLOSURE`'s `FnDef` has a synthetic `__env_p` first parameter for
  codegen; `skip_env_param` drops it.
- A native has `fn == NULL`. Registration stamps its capability row and
  handle signature from `native_caps.c`.
- Debug builds stamp `origin_env` on first application and abort if the
  closure is applied under another env. Closures point into their env's
  arenas and must never be cached across envs.

### Collections and other handles

Vec, Set, Map and HAMT are C objects the natives allocate, handed around as
`TURI_INT` pointer words, exactly as compiled. Consequences:

- **Display** needs the elaborated type: `turi_try_show_by_tag` finds the
  `Show` instance from `env->last_result_type`. That is the third of the
  four display tiers in the parity guide.
- **Element types.** A `(Vec any)` needs per-element tags, which live in a
  process-global side table keyed by the vec's address
  (`collections_native.c:vec_tag_set`).
- **Freeing.** Natives that allocate register the box with
  `turi_env_track_collection`, so `turi_env_free` frees what the program
  did not. An explicit `vec-free` tombstones its node.
- **Forgery.** A sandboxed or macro env turns on handle provenance
  (`turi_prov_*`): a native whose signature names a handle argument refuses
  a word that no constructor minted in this env
  ([turi-sandbox-handles-are-forgeable-integers](../archive/turi-sandbox-handles-are-forgeable-integers.md)).

---

## Environments and frames

### Globals

`turi_env_get`/`turi_env_set` (`env.c`) work on a linked list of
`EnvBinding`s, indexed by an open-addressing table, `globals_ht`.

- `turi_env_set` **keeps the name pointer**. Pass a literal or a string in
  `env->sym_arena`, never a stack buffer.
- A miss returns a `TURI_ERROR` "unbound variable".
- A `defn` inside a `defmodule` that the module does not export is
  registered as `"<module>/<name>"` (allocated in `sym_arena`), plus under
  the bare name if that is free. `eval_lookup` probes
  `"<current_module>/<name>"` before the bare name. `env->current_module`
  is set from the running closure's `module` and restored by `DK_CALL_RET`.

### Frames

```c
struct EvalBinding { const char *name; TuriValue value; EvalBinding *next; };
struct EvalFrame {
    EvalBinding *bindings;  EvalFrame *parent;
    TyvarBind *tyvars;  DictBind *dicts;
    bool escaped, reclaimable, deferred;  uint32_t epoch;
    EvalFrame *owned, *owned_next;
};
```

- **Lookup is by name.** `eval_lookup` walks bindings frame by frame, then
  globals. `frame_name_eq` compares pointers first (names are interned
  nearly everywhere) and then the first byte, to keep `strcmp` off the hot
  path. Re-elaboration therefore never invalidates a lookup, but shadowing
  is purely lexical-chain order.
- **`tyvars`/`dicts`** are the interpreter's stand-in for monomorphization
  and dictionary passing. A generic call pins its type variables on the
  callee frame (`frame_record_abi`, `frame_pin_*_tyvars_from_args`); a
  constrained call binds one `DictBind` per constraint (and per superclass)
  with `frame_bind_constraint_dicts`. Method dispatch inside the body then
  re-resolves the instance the elaborator baked to the carrier
  representative ([turi-generic-dict-dispatch-bakes-representative-instance](../archive/turi-generic-dict-dispatch-bakes-representative-instance.md),
  [turi-dict-passing-plan](../archive/turi-dict-passing-plan.md)). The
  [typeclass internals guide](typeclass-internals-guide.md) covers the
  elaborator side.
- **Three kinds of frame:**

  | Made by | Kind | Released |
  |---|---|---|
  | `eval_frame_new_call` | an activation's call frame (`reclaimable`) | by its `DK_CALL_RET`, via `frame_release` |
  | `eval_frame_new_owned` | a `let` or match-arm frame | with its nearest call-frame owner (on its `owned` list) |
  | `eval_frame_new` | anything else (top level, clones, captures) | never, except by the r7rs pool sweep |

---

## Evaluation: the driver

Three functions:

- **`eval_expr`** checks the debugger hook, the in-flight signals and step
  fuel, then calls `eval_expr_impl`.
- **`eval_expr_impl`** is a big `switch` on `ExprKind`. Literals, variables,
  definitions and most special forms are evaluated here, recursively.
  `EX_LET`, `EX_IF`, `EX_DO`, `EX_PROGRAM`, `EX_BUILTIN`, `EX_CALL` and
  `EX_MATCH` are not: they call `eval_drive`.
- **`eval_drive_ex(env, frame, e, seed)`** is the machine.

### The machine

The driver's state is a CEK triple plus a direction bit:

| Driver local | CEK role |
|---|---|
| `control` (`const Expr *`) | C: the expression being evaluated |
| `cf` (`EvalFrame *`) | E: its environment |
| `st[0..len)` (`DriveCont`) | K: the continuation, a work-stack. It starts in an 8-entry inline array (`DRIVE_INLINE`) and moves to the heap when it grows |
| `cur` (`TuriValue`) | the value being returned |
| `descending` | true: evaluate `control`; false: hand `cur` to `st[len-1]` |
| `tail` | whether `control` is in tail position of the activation |

Each loop iteration either descends (a `switch` on `control->kind` that
pushes a `DriveCont` and moves `control` to a child, or produces `cur`
directly) or returns (a `switch` on the top entry's `kind` that consumes
`cur` and either pops or re-descends). `DK_DONE` at the bottom ends the
loop.

| `DriveKind` | Pushed for | `aux` / notes |
|---|---|---|
| `DK_DONE` | bottom of every drive | -- |
| `DK_IF_BRANCH`, `DK_DO_SEQ` | `if`, `do`/program | a sequence with a direct `defer` pushes a scope marker |
| `DK_LET_BIND`, `DK_LET_BODY` | `let`/`letrec` | owned frame; defer mark |
| `DK_MAKE_STRUCT`, `DK_BUILTIN_ARG`, `DK_CALL_ARG` | field / argument accumulation | malloc'd `TuriValue[]` accumulator |
| `DK_CALL_RET` | a Turmeric callee's body | the activation: defer mark, saved module, `returning`, `no_unwind` |
| `DK_MATCH_SCRUT`, `DK_MATCH_BODY` | `match` | the scrutinee is driven only when it may perform |
| `DK_GET_FIELD`, `DK_UNARY` | `(.f x)`, and cast/ascribe/return/`set!` | the operand is driven; `eval_unary_post` applies the form |
| `DK_WHILE` | `while` | `index` = phase (cond/body) |
| `DK_PROMPT` | a capturable `handle`/`with-handler` | `HandleExpr *` |
| `DK_PERFORM_ARG`, `DK_RESUME_K`, `DK_RESUME` | `perform` args, `resume` | so a perform inside them lands in the driver |
| `DK_RESET`, `DK_ESCAPE` | `reset`, `call/cc` | heap boundary records; no `setjmp` |
| `DK_CONT_FOLD` | resuming a serial/cloneable continuation | `ContFoldState *` |
| `DK_CATCH_UNWIND`, `DK_ATOMICALLY`, `DK_STM_SEQ` | `catch-unwind`, `atomically` | heap boundary records |
| `DK_NATIVE_RESUME` | a special form that must apply a closure: `tvar/modify`, an abortive `shift`'s receiver, a serial receiver | `NativeResume { resume, state }` |

The `default` descend arm is the **black box**: it calls `eval_expr` on the
node, which may itself start a nested `eval_drive_ex` on a new C frame.

### Calls and tail calls

`EX_CALL` (and `EX_DYN_CALL`) pushes `DK_CALL_ARG`, which evaluates the
callee and then each argument into its accumulator. When the last argument
returns:

- a **native or inline-C callee** is a *leaf*: `eval_apply` runs it
  synchronously and `cur` is its result;
- a **Turmeric callee** is *folded*: a call frame is made, the parameters
  bound (with the by-value struct copy), and `DK_CALL_ARG` is replaced by
  `DK_CALL_RET`. `control` becomes the body, with `tail = true`;
- a **call in tail position** whose next frame down is the enclosing
  activation's `DK_CALL_RET` **reuses** it: the old call frame is released
  and the slot re-armed. That is the TCO. A tail-recursive loop runs in
  constant work-stack depth and, since 2026-10-05, constant memory.

`DK_CALL_RET`'s return step is the function epilogue:

1. restore `in_no_unwind`;
2. fire this activation's defers (by scope on an early exit, head-first
   otherwise);
3. take `return_value` if `returning` is set;
4. restore `current_module`;
5. `frame_release`.

The C-side entry is `eval_apply`/`eval_apply_driven`, used by `turi_call`,
the leaf path and thunks. Its dispatch order is the inline-C ladder:

1. a native closure: `call_native_checked` (the capability check every
   native passes through);
2. an inline-C body with a native registered under the defn's name: that
   native;
3. an inline-C body the pattern executor recognises
   (`try_exec_simple_inline_c`);
4. a user inline-C body under `repl-jit-inline-c`:
   `turi_inline_c_jit_try`;
5. otherwise a Turmeric body: build the frame, then
   `eval_drive_ex(..., &seed)`. The `DriveSeed` makes the drive start with
   the activation's `DK_CALL_RET`.

If none applies, the body reaches `EX_INLINE_C` in `eval_expr_impl` and the
call fails with "inline-C not supported in interpreter mode".

### Signals

Non-local control is env state, checked on the way up, not `longjmp`:

| Signal | Set by | Consumed by |
|---|---|---|
| a `TURI_ERROR` value | any failing form | the entry point (printed) |
| `env->returning` / `return_value` | `return` (`eval_unary_post`) | the activation's `DK_CALL_RET` |
| `env->aborting` / `abort_value` / `abort_prompt_kind` / `abort_target` | an abortive `shift`; `(k v)` on a `call/cc` escape | the matching `DK_RESET`, or the `DK_ESCAPE` whose boundary `abort_target` names |
| `env->panicking` | `panic` | `DK_CATCH_UNWIND`; at top level, `exit` (or the host-exit pad) |
| `env->throwing` | nothing (retired) | -- |

Each return-side arm checks `signaled` and runs its own cleanup: a let frame
fires its defers, an accumulator is freed. A new arm must too.

### What still recurses on the C stack

The driver bounds recursion through **driven** positions. Three paths still
nest C frames:

- **Black-box forms.** Each `eval_expr` call from the default arm. It is
  bounded by the program's nesting depth, not by its recursion.
- **C-scoped forms run outside the driver**: `async`, a fiber-fallback
  `handle`, a generator body (each on its own ucontext stack).
- **Natives that call back**: about 30 call sites in
  `interpreter_natives.c` and `collections_native.c` call `turi_call`
  (option and result combinators, seq bridges, comparators, the R7RS
  natives). Each
  callback is a fresh `eval_drive_ex` on top of the native's C frame.
  [turi-cek-stackless-reentry-plan](../archive/turi-cek-stackless-reentry-plan.md)
  says this re-entry is gone because `option-map` and friends became pure
  Turmeric. But `cmd_eval_h` re-registers `native_option_map` (and the
  other natives) after the preload, so it is the native that runs.
  Recursion through `option-map` 100,000 deep overflows the C stack under a
  Debug `--interpret`; the compiled program completes.

`turi_env_set_max_depth` is a no-op. **Step fuel** (`turi_env_set_fuel`;
`TURI_DEFAULT_SANDBOX_FUEL`, 10M steps, for sandboxed envs and the macro
env) is the only resource limit. It is charged per `eval_expr` and per
activation.

---

## Natives and inline C

### Registering

| API | Use |
|---|---|
| `turi_env_register_native(env, name, fn, ud)` | The common case. Capabilities come from `native_caps.c`'s table by name |
| `turi_env_register_native_caps` | Explicit capability mask |
| `turi_env_register_native_typed` | Declares a result tag the elaborator can use |
| `turi_env_register_native_ex` | With a `ud` finalizer, run LIFO at `turi_env_free` |
| `turi_register_default_native` | Process-wide seed, installed last into every later env |

A native is `TuriValue fn(TuriEnv *, TuriValue *args, uint32_t n, void *ud)`.
It receives already-evaluated arguments, which are often carriers
(`TURI_INT` holding a pointer), and must return a fresh value.

The families live in `tur_core`, never in `main.c`, so embedders and the
WASM build get them (see the parity guide's embedding rules):

- `turi_env_new` registers async builtins, eval builtins
  (`turi_eval_register_builtins`), the collection and string natives, and
  `turi_env_register_interpreter_natives`;
- `turi_env_register_interpreter_natives` calls `wk_register_stdlib_natives`
  and the per-module `wk_register_*_natives` (safe, typeclass, comonad,
  mutex, future, bytes, taskgroup, chan, backtrack, trail, proc-fs, serial,
  sym, seq, json, schema, syntax), plus the contract and `doc` natives.

### Override order

A preloaded `defn` and a native fight over one global name:

- `EX_FN_DEF` **keeps** an existing native when the new defn's body is
  inline C. For `__inst_*` instance methods it keeps it only when the impl
  is defined in a `stdlib/` file, so a user `Show` with an inline-C body
  does not inherit stdlib's native
  ([turi-show-instance-with-inline-c-body-prints-a-pointer](../archive/turi-show-instance-with-inline-c-body-prints-a-pointer.md)).
- A defn with a **Turmeric** body overwrites the native.
- Natives registered **after** the preload overwrite everything, including
  Turmeric bodies. That is why every entry point calls
  `turi_env_register_interpreter_natives` and
  `turi_register_collection_natives` again after `turi_env_preload_*`
  ([turi-vec-new-filled-native-override-lost](../archive/turi-vec-new-filled-native-override-lost.md)).
  It is also why a stale native for a function stdlib has since rewritten
  in Turmeric keeps running.

So: a native for an inline-C stdlib body works whenever it is registered. A
native that is meant to replace a Turmeric body needs the post-preload
registration. A native that should no longer replace a Turmeric body must be
deleted.

### Inline C

There is no C compiler on this path. An inline-C body runs in one of four
ways:

1. **A native under the same name** (above). This is how the stdlib works.
2. **`try_exec_simple_inline_c`**, a string-pattern executor over the
   body's text. It recognises a void no-op, a bare `free`, a string
   `switch`, a fat-string compare, a `snprintf` formatter, a
   malloc-and-assign constructor, a field accessor, a linked-list print
   loop, and a single straight-line `return <expr>`. It declines anything
   with loops, several returns, `printf`, function-pointer calls or
   `__TUR_CNAME_` splices rather than guessing
   ([history/turi-inline-c-silent-miscompiles](../archive/history/turi-inline-c-silent-miscompiles.md)).
   `TUR_IC_TRACE=1` logs which matcher claimed each body. An `int` result is
   re-tagged to `TURI_BOOL` for a `: bool` return, and to a `TuriStruct *`
   for a non-opaque ADT return.
3. **`--enable=repl-jit-inline-c`** (beta; needs a `TUR_JIT` build). The
   defn's own source goes through the real emitter and MIR on its first
   call, and is called through its `__ffi` shim (`inline_c_jit.c`, hook
   installed from `main.c`). User bindings only; never stdlib.
4. **`EX_INLINE_C` intercepts** in `eval_expr_impl`: the exact
   `gc_force();`/`gc_enable();`/`gc_disable();` one-liners, the session
   channel templates (`eval_session_intercept`), and a top-level block with
   no frame and no captures (file-scope C declarations, which evaluate to
   nil).

Everything else is the clean error. `tests/run-turi.sh` skips fixtures with
user inline C for that reason.

`extern-c` declarations bind through `register_extern_c_binding`: a known
libc name (`exit`, `free`, `strlen`, `getenv`, `printf`, `puts`) gets a
hand-written native; with a JIT FFI provider, a c2mir thunk resolved against
the running process (aggregate signatures through `register_extern_c_agg`);
otherwise a nil stub.

---

## Effects, continuations and coroutines

### Two effect runtimes

When the driver meets `EX_HANDLE` (or `EX_WITH_HANDLER`), it asks
`ws_capturable` whether the body and every clause reach each `perform` only
through driver-transparent forms.

**Yes: the work-stack runtime.**

1. Push `DK_PROMPT` and descend the body.
2. `perform` finds the nearest matching prompt on `st` and copies the slice
   above it into a heap `TuriWsCont`, with the handler, its frame and the
   env state at the perform.
3. The clause runs with `k` bound to a `TURI_EFFECT_CONT` whose `ws` is set.
4. `resume` (`DK_RESUME`) pushes a **clone** of the slice (`clone_ws_slice`:
   owned frames deep-copied, accumulators duplicated) and re-installs the
   prompt for a deep handler.

This gives multishot, escaping and nested resume, the analogue of the
compiled DK chain. When a clause is exactly `(resume k <inert value>)` and
`k` is not `^multishot`, `ws_case_is_oneshot_resume` takes a fast path: the
slice is pushed back in place, not cloned, and freed
([turi-effect-perform-keeps-its-continuation](../archive/turi-effect-perform-keeps-its-continuation.md)).

**No: the fiber fallback.** `eval_handle` runs the body on a 256 KB
ucontext stack (`EFFECT_CONT_STACK_SIZE`, `mmap`'d) and `swapcontext`s on
each `perform`. It is **single-shot**: a second `resume` fails with "this
continuation has already been resumed and its body has finished".

`ws_capturable` is conservative. It recurses into direct calls of Turmeric
functions (with a cycle guard, so recursive handlers stay on the work-stack)
and accepts `if`/`do`/`let`/`while`/`match`/`set!`/casts. It rejects:

- a call that passes a value which may perform, such as a lambda whose body
  performs. A callee could apply it from a black box, so this holds even
  when the callee is a plain Turmeric HOF;
- match guards, and resume/value positions it cannot model;
- `catch-unwind`, `reset` and `atomically` boundaries;
- anything past its depth or function-count budget.

`TURI_TRACE_FIBER_FALLBACK=1` names the rejected form:

```
turi: note: handle at 12:12 falls back to the single-shot fiber effect
runtime: the form at 4:21 (EX kind 16) is not work-stack capturable
```

(kind 16 is `EX_CALL`). In practice, a handler whose `perform` is reached
through a callback, such as
`(handle (my-map xs (fn [x] (perform (Pick)))) (Pick [] ^multishot k) ...)`,
prints 23 compiled and errors on the second `resume` interpreted.
[turi-multishot-resume-in-while-aborts](../archive/turi-multishot-resume-in-while-aborts.md)
and [turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md)
record earlier gaps of this shape.

### Delimited control and boundaries

All of these live on the work-stack with heap boundary records, so nesting
costs no C stack ([history/turi-c-scoped-forms-heap-bounding-plan](../archive/history/turi-c-scoped-forms-heap-bounding-plan.md)):

- **`reset`/`shift`/`shift0`.** `DK_RESET` marks the prompt. An abortive
  `shift` sets `env->aborting`, and its receiver runs through
  `DK_NATIVE_RESUME`.
- **`call/cc`/`escape`** (one-shot, upward). `DK_ESCAPE` is the landing.
  `(k v)` raises `aborting` with `abort_target` set. A `k` whose boundary
  has already returned is a named error, checked against
  `env->escape_live`.
- **Serial and cloneable reset/shift** reify their context as a
  `TuriCont`. Resuming one folds its captured frames through
  `DK_CONT_FOLD`.
- **`catch-unwind`** is `DK_CATCH_UNWIND`. A panic sets `env->panicking`
  and unwinds to it as `(err payload)`. `eval_apply` refuses to start any
  callee while a panic is in flight.
- **`atomically`** is `DK_ATOMICALLY` and `DK_STM_SEQ`. STM is
  single-threaded: a `retry` with no progress is an error, not a block.

### Generators, async, sessions

- **Generators** (`EX_GEN`) run their body on a 256 KB ucontext stack
  (`GEN_STACK_SIZE`); `yield` swaps back. The stack is tracked with
  `turi_env_track_coro_stack` and freed with the env.
- **`async`** pre-evaluates its operand. A closure becomes a `TuriFiber`
  with a 512 KB stack (`TURI_ASYNC_STACK_SIZE`) on the ready queue. A
  non-closure (the elaborator does not always wrap the body) is already
  evaluated, so it settles the future at once. `await` from the main
  context pumps the scheduler (`turi_await_future`, `turi_sched_step`); in
  a fiber it parks. A finished fiber's stack is unmapped early.
- **Sessions** are a cooperative one-slot rendezvous over the scheduler
  (`TuriChan`, `session_send_on`/`session_recv_on`). A blocked operation
  with nothing runnable is a "deadlocked" error that names the protocol,
  something the compiled pthread runtime cannot detect.

### `#lang r7rs` re-entrant `call/cc`

The R7RS prelude's `call/cc` (`native_r7rs_cont_capture`) copies the **C
stack** between the current top-level form's prompt and the capture, plus
the driver state that is off that stack. The state a re-entry must find as
it was:

- every live drive registers itself on `g_drive_regs` (`DriveReg`). A
  capture snapshots the heap-grown work-stacks; a re-entry hands each drive
  a fresh copy;
- `TuriContState` (`turi_cont_state_capture`) saves the env's
  dynamic-extent fields and the thread-local boundary stacks;
- `g_turi_cont_epoch` counts captures. A frame, closure, struct or
  work-stack temporary stamped with an older epoch may sit in an image, so
  it is not released at its normal point; it goes to
  `cont_defer_frame`/`cont_defer_temp` instead.

Nothing outside `#lang r7rs` captures, so for other programs every stamp
matches and these paths are inert.

---

## Memory

### Where things live

| Store | Holds | Freed |
|---|---|---|
| `env->sym_arena` | symbol text, global names, module-private keys | `turi_env_free` |
| `env->eval_arenas` (one per turn) | `Form`s, the elaborated `Expr` tree, `TypeClassEnv` | `turi_env_free` (closures point into them) |
| `env->value_scratch` / `value_perm` | escaping payloads: frames, bindings, closures, structs, pins, fibers | `turi_env_free`, or a scratch rewind |
| free lists on `TuriEnv` (`frame_free`, `binding_free`, `tyvar_free`, `dict_free`, `closure_free`, `struct_free[n]`, `*_swept`) | pool objects handed back | reused |
| `malloc` | driver accumulators, heap work-stacks, `TuriEffectCont`, collection boxes, `TURI_CSTR` results | individually, or tracked and freed with the env |
| `mmap` | fiber, generator and fallback-handler stacks | tracked, or `munmap`ed at the end of the handle |

### What is reclaimed

The interpreter's baseline policy is **process-lifetime**: anything that
might be captured stays until `turi_env_free`. These are the exceptions:

| Since | What | Mechanism |
|---|---|---|
| 2026-10-05 | an activation's call frame, its `let`/match-arm frames and bindings, when nothing captured them | `frame_release` from `DK_CALL_RET` ([turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md)) |
| 2026-10 | a released frame's tyvar/dict pins; a `defer` snapshot | free lists; the rest is open in [turi-call-pins-and-side-frames-not-reclaimed](../reported/turi-call-pins-and-side-frames-not-reclaimed.md) |
| 2026-10-07 | the by-value struct copy, skipped for types nothing writes | `turi_struct_arg_may_be_written` |
| `#lang r7rs` | `call/cc` images nothing references | the continuation sweep ([r7rs-callcc-memory-never-freed](../archive/r7rs-callcc-memory-never-freed.md)) |
| 2026-10-14, `#lang r7rs` | dropped closures, frames, bindings and structs (up to 8 fields) | the pool sweep ([turi-dropped-closures-kept-until-teardown](../archive/turi-dropped-closures-kept-until-teardown.md)) |
| REPL only | per-turn scratch, when the turn's results can be relocated | scratch promotion (`turi_promote_escaping`, on in `repl_configure_env`) and the TR3 collection sweep after a rewind |
| always | collection boxes the program never freed | `turi_env_track_collection`, at `turi_env_free` |

**Frame release rules.** `frame_release` does nothing when:

- the frame is not a call frame, or it escaped (`frame_escape` marks the
  frame and all its ancestors when a closure, generator, continuation or
  captured slice can hold it);
- a debugger is attached;
- `TUR_TURI_FRAME_RECLAIM=0` is set.

A frame older than the last capture epoch is deferred to the sweep instead.

**The sweeps** (r7rs only; `turi_cont_gc_*` in `eval.c`, driven from
`interpreter_natives.c`) are conservative mark passes:

1. Roots are globals, the env's in-flight values, every registered drive
   (read typed), the C stack and registers, and each reachable image.
2. Any word that points into the value pool is resolved to its object
   through the pool's object map (`turi_val_starts_*`, enabled only for an
   r7rs env that started from turn 0).
3. A **doubt** cancels the sweep: fibers, generators or futures, an imported
   Turmeric module, a debugger, another env's continuation, and similar.

The pool sweep runs at a driver safe point (`pool_sweep_safe_point`) once
`turi_pool_count` passes the budget: 4 MB, or more after a big sweep.

**Measured** (Debug `tur`, run with `quarantine_size_mb=0` so ASan's free
quarantine does not count):

| Loop | 100k steps | 400k steps | 1.6M steps |
|---|---|---|---|
| tail-recursive loop calling a helper | 79 MB | 79 MB | 79 MB |
| same loop making one capturing `fn` per step | 116 MB | 228 MB | 674 MB |

The plain loop is flat. The capturing loop keeps about 370 B per step: its
frames escaped, and nothing sweeps a Turmeric program. With ASan's default
256 MB quarantine the plain loop's peak also rises (114 MB to 204 MB),
because the driver's freed accumulators are quarantined. Check RSS before
blaming a leak.

### Why the harnesses set `detect_leaks=0`

`run-turi.sh`, `run-flags.sh` and their ctest targets export
`ASAN_OPTIONS=detect_leaks=0` because, by design:

- many natives return a malloc'd `TURI_CSTR` that nothing frees;
- registered natives and `defn` closures live until exit;
- once an r7rs capture has pinned them, driver temporaries are kept.

`turi_env_free` reclaims the arenas, the pool and tracked buffers, so an
embedder that frees its env is leak-clean for those. The compiler/codegen
path stays leak-checked. Override with
`ASAN_OPTIONS=detect_leaks=1 bash tests/run-turi.sh`.

---

## Process-global state

The interpreter is mostly per-env, but these globals are shared by every env
in the process. Each has bitten someone.

- **`g_interpret_mode`** (`src/runtime/globals.c`) is read by the
  **elaborator**:
  - `#?(:tur ... :turi ...)` picks the `:turi` arm
    (`elab_toplevel.c`);
  - an unknown call head becomes a TUR-W0040 runtime dispatch instead of an
    error (`elab_call.c`, UCH1);
  - the top-level statement fold is skipped (`elab_toplevel.c`);
  - an ADT under an integer return is a bridge rather than a by-value
    mismatch (`elab_core.c`);
  - an endpoint-capturing `async` stays on the spawning thread
    (`elab_concurrent.c`).

  Who writes it:
  - `turi_env_new` sets it to `true`, unconditionally;
  - `turi_eval_with_sink` sets it from `env->interpret_mode` for one call
    and restores it;
  - code that compiles **while an interpreter env exists** must bracket it:
    save, set `false`, compile, restore. `main.c` does this around
    `compile_to_c` for the JIT REPL and the inline-C JIT build, and
    `macro_env.c` does it around `turi_env_new`.
- **`g_turi_stdlib_preload`** stamps definitions as stdlib-owned while it is
  set. Set it only around a preload. `acc_turns` records it, so a session
  replay re-elaborates each turn under the same value.
- **Static and thread-local driver state in `eval.c`**: `g_turi_cont_pinned`
  and `g_turi_cont_epoch`; `g_drive_regs` and `g_drive_boundary`;
  `g_reset_stack`, `g_catch_stack` and `g_stm_tx`; `g_wscap_fns` and
  `g_wscap_reject`; `g_pending_cont` (the `makecontext` side channel);
  `g_panic_site` and `g_fwd_site`. A second env on the same thread shares
  them, which is one reason the r7rs sweep refuses to run across envs.
- **Process-wide tables and hooks**: the Vec element-tag table, the
  default-native list (`turi_register_default_native`), the pool-sweep hook
  (`turi_set_pool_sweep_hook`, installed by the R7RS natives), the inline-C
  JIT hook, the JIT FFI provider, and the diagnostic file registry (saved
  and restored around every nested evaluation).

### The macro-time env

`elab_macro_env_get` creates one interpreter env per `ElabSession`, lazily,
at the first `defmacro*`. The compile is mid-elaboration, so the env is
built inside a `MacroEnvBracket`:

1. Save and replace the diagnostic file registry and `diag_had_error`.
2. Silence the `tur expand` trace (`g_dump_expansion`).
3. Re-stamp the builtin table's symbol pointers on exit
   (`builtins_init` runs at every elaborate entry).

The env then gets the REPL's preload (plus `cstr.tur` and `str-build.tur`)
and the post-preload natives. After that, **every capability is denied**
and step fuel is set. `--macro-caps=io` re-grants only `TURI_CAP_IO |
TURI_CAP_FS`. Macro bodies run with `interpret_mode = true`, so they see
`:turi` reader arms. The env is disposed with the session. `macro_env.c`'s
header still says nothing calls into it beyond creation; in fact
`elab_macro_env_define_proc` and `elab_macro_env_call_proc` run every
`defmacro*`. The [macros guide](macros-guide.md) covers the language side.

---

## Debugging and tracing

- **`tur debug f.tur`**: `turi_debug_enable` attaches a `TuriDebugger`.
  `eval_expr` and the driver's descend step call `turi_dbg_before_node` at
  each node. Debugger frames are pushed and popped at `DK_CALL_RET`. The
  debugger is armed before `main` (or around the whole load for a
  top-level program).
- **`tur dap`** (`dap.c`): the same debugger behind the pause-handler API in
  `eval.h` (`turi_debug_set_pause_handler`, `turi_debug_frame_at`, ...).
  `main.c:dap_launch_cb` builds the env through `cmd_eval_h`.
- **`tur trace`** (`trace.c`): a recorder built on the pause handler and
  `resume_step_node`. It records per-node binding deltas into a byte buffer.
- **`(break)`** is a builtin: a no-op without a debugger.
- An attached debugger disables frame release.

| Variable | Effect |
|---|---|
| `TURI_TRACE_FIBER_FALLBACK=1` | Name the form that forced a `handle` onto the single-shot fiber |
| `TUR_IC_TRACE=1` | Log which inline-C pattern matcher claimed each body |
| `TUR_TURI_FRAME_RECLAIM=0` | Turn frame release off (bisect a suspected use-after-release; the free lists hide one from ASan) |
| `TUR_TURI_CONT_GC=0`, `_TRACE=1`, `_STRESS=1` | r7rs continuation sweep: off, report each sweep, sweep at every capture |
| `TUR_TURI_POOL_GC=0`, `_STRESS=<bytes>`, `_QUARANTINE=1` | r7rs pool sweep: off, sweep every N bytes, quarantine instead of reusing (finds a missed root) |
| `TUR_NO_INCREMENTAL_ELAB=1` | Whole-program re-parse and re-elaboration each turn |
| `TUR_NO_SCRATCH_PROMOTION=1` | REPL: no scratch rewind |
| `TUR_STDLIB_DIR`, `TUR_NO_AUTO_SPICE`, `TUR_BIN` | Stdlib root; skip spice discovery; the `tur` used to build spice images |

---

## Adding a form or a native

**A new `ExprKind`:**

1. Add an arm to `eval_expr_impl`. Return any error or signal from a child
   unchanged (`if (turi_is_error(v) || env_signaled(env)) return v;`).
2. If the form's operands can recurse deeply or can `perform`, model it in
   the driver instead: a descend arm that pushes a `DriveCont`, and a
   return arm that handles `signaled` and frees what it owns. A
   single-operand form can join `unary_operand`/`eval_unary_post`.
3. Teach `ws_has_perform` and `ws_capturable_rec` about it, or every
   `handle` containing it drops to the single-shot fiber. Run a multishot
   probe with `TURI_TRACE_FIBER_FALLBACK=1`.
4. If the form allocates an `aux`, add it to `clone_ws_slice`, the r7rs
   drive scan (`gc_drive`) and the promotion walk (`promo_*`).
5. If it makes a closure or keeps a frame, call `frame_escape`.
6. Run `python3 tools/check_turi_parity.py`. It must report 0 gaps; an
   intentional carve-out goes in `docs/artifacts/turi-carve-out.txt`.

**A native for an inline-C stdlib function:**

1. Write it in `src/turi/interpreter_natives.c` (or `collections_native.c`
   or `string_native.c`). Never in `main.c`.
2. Register it in the right `wk_register_*_natives`, under the exact
   Turmeric name. An instance method uses its mangled `__inst_...` name.
3. Add a row to `native_caps.c` for its capability and, if it takes or
   mints handles, its handle signature.
4. If the stdlib body is Turmeric rather than inline C, decide whether the
   native should win. It will win, because of the post-preload
   re-registration. A native that calls back through `turi_call` brings
   back C recursion and makes the callback opaque to `ws_capturable`.
5. A native whose return is not an `int` needs a typed stub in
   `turi_env_preload_native_stubs`, or the elaborator types its calls as
   `:int` (or warns TUR-W0040).
6. A native that stores a caller's value into memory that outlives a region
   bracket needs a region note (CLAUDE.md "Region Store Hooks"; see the
   [regions internals guide](regions-internals-guide.md)).

### How it is tested

- `bash tests/run-turi.sh` (ctest `turi_fixture_tests`) runs every fixture
  under `tur --interpret`, and the `errors/` fixtures for their diagnostics.
  It PASS-skips:
  - fixtures marked `requires.compiled`, `requires.tur-only`,
    `requires.dedicated-runner`, `requires.spices` (when the sibling
    checkout is absent), `requires.tsan` or `requires.stress`;
  - fixtures with user inline C, except a list it runs with
    `--enable=repl-jit-inline-c` on JIT builds.

  `requires.interp-only` fixtures run **only** here. `stderr.compiled-only`
  limits the check to stdout and exit. Use `TURI_FILTER='<regex>'` to
  narrow a run. The full rules are in the
  [running-tests guide](running-tests-guide.md) and
  [test-suite-portability guide 7d](test-suite-portability-guide.md#7d-what-run-turish-does-not-run----and-how-it-says-so).
- ctest targets cover the parts fixtures cannot:
  `tur_eval_*` (the C API, effects, STM, async, sandboxing, TCO),
  `tur_incremental_elab_diff` (incremental against whole-program
  elaboration), `tur_turi_frame_reclaim`, `tur_turi_callcc_reclaim` and
  `tur_turi_pool_sweep` (the RSS bounds above), `tur_macro_env_nested`,
  `tur_dap`, `tur_trace` and `tur_repl_*`.
- Parity checkers: `tools/check_turi_parity.py` (`EX_*` coverage) and
  `tools/check_turi_native_parity.py` (preload module coverage; it reads
  `prelude[]` from `preload.c`).

---

## Reading the archive

The interpreter was built in phases, each archived with a status banner:

1. [turi-eval-trampoline-plan](../archive/turi-eval-trampoline-plan.md) and
   [turi-cek-frame-reuse-tco-plan](../archive/turi-cek-frame-reuse-tco-plan.md):
   the driver and its tail calls (T1-T3, F1-F5).
2. [turi-interpreter-delimited-control-plan](../archive/turi-interpreter-delimited-control-plan.md):
   `DK_PROMPT` and `TuriWsCont`.
3. [turi-cek-stackless-reentry-plan](../archive/turi-cek-stackless-reentry-plan.md)
   and [history/turi-c-scoped-forms-heap-bounding-plan](../archive/history/turi-c-scoped-forms-heap-bounding-plan.md):
   `DK_NATIVE_RESUME`, `DK_RESET`, `DK_ESCAPE`, `DK_CATCH_UNWIND`,
   `DK_ATOMICALLY`, and the retirement of the depth guard.
4. [turi-incremental-elaboration-design](../archive/turi-incremental-elaboration-design.md):
   `acc_forms`, `elab_session`.
5. [history/turi-env-owned-value-arena-pool-plan](../archive/history/turi-env-owned-value-arena-pool-plan.md)
   and [turi-interp-incremental-reclamation-plan](../archive/turi-interp-incremental-reclamation-plan.md):
   the value pool, scratch promotion, the TR3 collection sweep.
6. [turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md),
   [r7rs-callcc-memory-never-freed](../archive/r7rs-callcc-memory-never-freed.md)
   and [turi-dropped-closures-kept-until-teardown](../archive/turi-dropped-closures-kept-until-teardown.md):
   the 2026-10 reclamation work.
7. [turi-parity-post-v1-plan](../archive/turi-parity-post-v1-plan.md) and
   [turi-interp-stdlib-natives-libturi-plan](../archive/turi-interp-stdlib-natives-libturi-plan.md):
   the preload and natives moving into `tur_core`.

---

## Maintenance

Update this guide in the same PR when you:

- add a `TuriTag`, a `DriveKind`, or a field family on `TuriEnv`;
- change the `cmd_eval_h` sequence, the preload order or the override rules;
- move a form between `eval_expr_impl` and the driver, or change what
  `ws_capturable` accepts;
- change what is reclaimed, or add a sweep, a doubt or an environment
  variable;
- add an elaborator branch on `g_interpret_mode`.

Re-run the memory table's two probes when frame or closure reclamation
changes. Cite functions, not line numbers.
