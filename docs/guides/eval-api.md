---
title: libturi -- C Embedding API Reference
category: Interoperability
description: C embedding API for evaluating Turmeric expressions and calling Turmeric functions from within a C program using libturi.a
---

# libturi -- C Embedding API Reference

`libturi` is the Turmeric eval runtime packaged as a static library (`libturi.a`).
It lets C programs evaluate Turmeric expressions, call Turmeric functions, and
expose C functions to Turmeric code -- all without spawning a subprocess.

---

## Quick start

```c
#include <turi/eval.h>

int main(void) {
    turi_init(/*use_color=*/false);

    TuriEnv *env = turi_env_new();

    TuriValue v = turi_eval(env, "(+ 1 2)");
    /* v.tag == TURI_INT, v.as_int == 3 */

    turi_env_free(env);
    return 0;
}
```

Compile and link:

```sh
cc -o demo demo.c -I/usr/local/include/turi \
   -L/usr/local/lib -lturi -lpthread
```

---

## Headers

| Header | Purpose |
|--------|---------|
| `turi/eval.h`  | Main API -- eval, init, async helpers |
| `turi/env.h`   | `TuriEnv` type and environment operations |
| `turi/value.h` | `TuriValue` tagged union and constructors |

Include only `turi/eval.h`; it pulls in the others transitively.

---

## Initialisation

### `void turi_init(bool use_color)`

Initialises the diagnostics subsystem.  Call **once** before the first
`turi_eval`.  Pass `use_color = true` to enable ANSI colour in error messages
(suitable when stderr is a terminal).

---

## Environments

An environment (`TuriEnv`) holds all accumulated definitions and per-call
arenas.  Closures captured from an environment must not outlive it.

### `TuriEnv *turi_env_new(void)`

Creates an unrestricted environment.  All builtins -- including I/O -- are
available.

### `TuriEnv *turi_env_new_sandboxed(void)`

Creates a sandboxed environment.  I/O builtins (`println`, `read-async`,
`write-async`, ...) and inline-C expressions are disabled.  Suitable for
evaluating untrusted code.

### `void turi_env_free(TuriEnv *env)`

Frees all resources owned by `env`.  Any `TuriValue` containing a closure,
struct, or future that was produced by this environment becomes invalid after
this call.

### `TuriValue turi_env_get(TuriEnv *env, const char *name)`

Looks up a global binding by name.  Returns a `TURI_ERROR` value if the name
is not bound.

### `void turi_env_set(TuriEnv *env, const char *name, TuriValue value)`

Creates or replaces a global binding.  Useful for injecting values from C into
Turmeric code before evaluating expressions that reference them.

---

## Evaluation

### `TuriValue turi_eval(TuriEnv *env, const char *src)`

Evaluates a Turmeric source string in `env`.  Prior definitions in `env` remain
visible.  New top-level definitions (`defn`, `def`) are stored back into `env`
for subsequent calls.

On parse or elaboration error the diagnostic is emitted to stderr and the
returned value has tag `TURI_ERROR`.

The returned value is valid until `turi_env_free(env)`.

```c
/* Define a function, then call it in a later eval. */
turi_eval(env, "(defn double [x :int] :int (* x 2))");
TuriValue r = turi_eval(env, "(double 21)");
/* r.as_int == 42 */
```

### `TuriValue turi_eval_file(TuriEnv *env, const char *path)`

Reads `path` and evaluates its contents via `turi_eval`.  Returns a `TURI_ERROR`
if the file cannot be opened.

---

## Values -- `TuriValue`

`TuriValue` is a tagged union:

```c
typedef struct TuriValue {
    TuriTag tag;
    union {
        bool             as_bool;
        int64_t          as_int;
        double           as_float;
        const char      *as_cstr;    /* NUL-terminated string */
        TuriClosure     *as_closure;
        const char      *as_error;   /* error message */
        TuriEffectCont  *as_cont;
        TuriStruct      *as_struct;
        TuriThrow       *as_throw;
        TuriFuture      *as_future;
    };
} TuriValue;
```

### Tags

| Tag | Meaning |
|-----|---------|
| `TURI_NIL`    | Unit / void -- `()`, `nil` |
| `TURI_BOOL`   | Boolean -- `true` / `false` |
| `TURI_INT`    | 64-bit signed integer |
| `TURI_FLOAT`  | 64-bit float (double) |
| `TURI_CSTR`   | NUL-terminated C string |
| `TURI_CLOSURE`| First-class function |
| `TURI_ERROR`  | Runtime / parse error (`as_error` holds the message) |
| `TURI_EFFECT_CONT` | Live algebraic-effect continuation |
| `TURI_STRUCT` | Struct instance |
| `TURI_THROW`  | Vestigial -- no evaluation path produces it (the `throw`/`try`/`catch` surface was removed from the language) |
| `TURI_FUTURE` | Async future handle |
| `TURI_REF`    | Mutable borrow reference |
| `TURI_STRUCT_TYPE` | Struct type descriptor (`as_cstr` holds the name) |
| `TURI_GEN`    | Generator instance |
| `TURI_HANDLER`| First-class handler value |
| `TURI_REJECTION` | Async-task rejection (`as_error` holds the message). Distinct from `TURI_ERROR` so a rejected future value observed via `(error? r)` / `(error-message r)` does not short-circuit evaluation |
| `TURI_SYNTAX` | Syntax object (wraps a compiler `Form*`) -- produced by `read-string` and the `syntax-*` natives; the value vocabulary of `defmacro*` macro bodies (see [macros-guide.md](macros-guide.md)) |

### Constructors

```c
TuriValue turi_nil(void);
TuriValue turi_bool(bool b);
TuriValue turi_int(int64_t i);
TuriValue turi_float(double f);
TuriValue turi_cstr(const char *s);   /* borrows s -- caller keeps it alive */
TuriValue turi_error(const char *msg);
TuriValue turi_errorf(const char *fmt, ...);
```

### Predicates and accessors

```c
bool        turi_is_error(TuriValue v);
bool        turi_is_truthy(TuriValue v);
bool        turi_is_throw(TuriValue v);
bool        turi_as_bool(TuriValue v);
int64_t     turi_as_int(TuriValue v);
double      turi_as_float(TuriValue v);
const char *turi_as_cstr(TuriValue v);
const char *turi_error_message(TuriValue v);
```

### Printing

```c
void turi_value_repr(char *buf, size_t cap, TuriValue v);
```

Writes a human-readable representation into `buf` (at most `cap` bytes,
NUL-terminated).  Examples: `"42"`, `"\"hello\""`, `"#<fn double>"`,
`"true"`.

```c
void turi_print_value(FILE *out, TuriValue v);
```

Prints the same representation to `out`.

---

## Registering native C functions

### `void turi_env_register_native(TuriEnv *env, const char *name, TuriNativeFn fn, void *ud)`

Registers a C function so that Turmeric code can call it by `name`.

```c
typedef TuriValue (*TuriNativeFn)(TuriEnv *env, TuriValue *args,
                                   uint32_t n, void *ud);
```

- `args` -- array of evaluated arguments (length `n`).
- `ud` -- the `void *ud` passed to `turi_env_register_native`.
- Return `turi_nil()` for void functions; return an appropriate `TuriValue`
  for functions that produce results.
- Signal failure by returning `turi_error("message")` / `turi_errorf(...)`.

```c
static TuriValue native_add(TuriEnv *env, TuriValue *args,
                             uint32_t n, void *ud) {
    (void)env; (void)ud;
    if (n != 2) return turi_error("add: expected 2 args");
    return turi_int(args[0].as_int + args[1].as_int);
}

turi_env_register_native(env, "my-add", native_add, NULL);
TuriValue r = turi_eval(env, "(my-add 10 32)");
/* r.as_int == 42 */
```

---

## Per-embed-env peripherals

These helpers exist for embedders that create **one `TuriEnv` per attached
script** (so that two scripts cannot clobber each other's globals -- e.g. each
exporting `_ready`). They keep the embed surface from becoming N copies of the
same boilerplate. See the report `libturi-per-embed-env-and-peripherals` for
the motivating Godot use case.

### Default natives -- seed once, install everywhere

```c
void turi_register_default_native(const char *name, TuriNativeFn fn, void *ud);
void turi_clear_default_natives(void);
TuriEnv *turi_env_new_with_natives(const TuriNativeSpec *specs, size_t n);
```

`turi_register_default_native` records a native that **every subsequent**
`turi_env_new` / `turi_env_new_sandboxed` / `turi_env_new_with_natives` call
installs automatically. An embedder shipping ten natives registers them once at
startup instead of re-registering on every per-script env (and cannot forget
one). The name is copied; re-registering a name replaces the prior entry. It
does not retroactively affect already-created envs.

`turi_env_new_with_natives` is `turi_env_new` plus an explicit table installed
on top of the default-natives registry:

```c
TuriNativeSpec specs[] = {
    { "godot-println", native_println, lang_singleton },
    { "godot-emit",    native_emit,    lang_singleton },
};
TuriEnv *env = turi_env_new_with_natives(specs, 2);
```

### `void turi_env_reset(TuriEnv *env)` -- reload without teardown

Resets an env to a from-scratch interpreter state: clears all globals that
`turi_eval` installed (user `defn`/`def`), the accumulated source, and the
deferred / handler / return / throw / abort / catch control state -- while
**keeping every registered native alive** (the builtins and the embedder's
own natives). This is what a script `_reload` wants: re-run the new source over
a clean slate without `turi_env_free` + re-registering natives.

Notes: non-native definitions a prelude installed (interpreted stdlib `defn`s
loaded via `turi_eval_file`) are dropped too -- re-eval the prelude after a
reset. Arena memory from prior evals is reclaimed at `turi_env_free`, not here.
Call between top-level eval cycles, not from inside an async/handler frame.

### `void turi_env_set_diag_sink(TuriEnv *env, TuriDiagSinkFn cb, void *ud)`

Routes this env's parse/elaboration diagnostics to `cb` instead of stderr for
the duration of each `turi_eval` call on the env. Lets an editor integration
attribute each script's compile errors to that script in its own Output panel.

```c
static void on_diag(TuriEnv *env, int level, const char *code,
                    const char *file, uint32_t line, uint32_t col_start,
                    uint32_t col_end, const char *message, void *ud) {
    /* level: 0=error 1=warning 2=note 3=help; code is "TUR-E0001" or "". */
    editor_output_push((Script *)ud, level, file, line, message);
}

turi_env_set_diag_sink(env, on_diag, script);
```

Pass `cb == NULL` to clear. LSP-collection and JSON diagnostic modes take
precedence over the sink when active.

### `void turi_env_set_module_base_dir(TuriEnv *env, const char *path)`

Sets the base directory used to resolve `(import ...)` paths. **Call this
before `turi_eval`** on source that imports modules; otherwise imports resolve
relative to the process cwd (for a packaged app, wherever the binary was
launched -- almost never what the user means). `path` is copied; pass `NULL` to
restore the default (`"."`).

```c
turi_env_set_module_base_dir(env, "res://scripts");
turi_eval(env, "(defmodule game/main (import std/list) ...)");
```

A bare top-level `(import ...)`, outside any `defmodule`, is an error unless
the env allows one -- see `turi_env_set_toplevel_imports` below.

### `int turi_env_set_search_path_for(TuriEnv *env, const char *path)`

Resolves `(import ...)` the way `tur run <path>` would for a program whose
source lives at `path` -- a notebook, a script, an editor buffer. The module
base dir becomes `path`'s directory, and the extra search dirs become what the
per-file commands find by walking up to the enclosing `build.tur`: the spice's
own `src/`, each `:spices` dep's `src/` that is on disk (`:path` deps
included), and the `src/` of every other member of a workspace that lists the
spice in `:members`. Outside any `build.tur` only the base dir is set.

The env owns what this builds (`turi_env_free` frees it), a second call
replaces it, `NULL` clears both, and it survives `turi_env_reset`. Returns the
number of extra dirs found, or `-1` on allocation failure. The stdlib is still
found through `TUR_STDLIB_DIR` (or a cwd-relative `stdlib/`), as for
`turi_eval` everywhere.

```c
turi_env_set_search_path_for(env, "/work/my-spice/notes/walkthrough.tur.md");
/* now (import my-spice/core ...) and any :spices dep resolve */
```

`TURI_HAS_SEARCH_PATH_FOR` is defined beside the declaration, so an embedder
that must also build against an older libturi can `#ifdef` it and fall back to
`turi_env_set_module_base_dir`.

### `void turi_env_set_toplevel_imports(TuriEnv *env, bool on)`

Lets source evaluated on `env` write `(import ...)` at the top level, outside
any `defmodule` -- the interactive-session model, where one turn (a REPL line,
a notebook cell) imports and a later turn calls what it referred. `:refer`,
`:as` and `:for-macros` all work, and a referred name or an alias stays bound
for the rest of the session; re-running an import turn is harmless. Off by
default, because a program's imports belong to its `defmodule`, as they do when
the same file is compiled. `tur repl` turns it on; the notebook does too.

```c
turi_env_set_toplevel_imports(env, true);
turi_eval(env, "(import plot/core :refer [plot-write-png])");
turi_eval(env, "(plot-write-png ...)");
```

### `void turi_env_preload_stdlib(TuriEnv *env, const char *stdlib_root)`

Loads the standard library into a fresh env the way `tur --interpret` does for
a program: the core macros (`when`, `cond`, `for`, ...), the typed collections
(`Vec`, `Map`, `Set`, `Option`, `Result`, ...) and the interpreter's natives
for stdlib functions whose bodies are inline C. A bare `turi_env_new` env has
only the elaborator builtins, so a module that names `(Vec float)` does not
elaborate in it. Call it once, before the first `turi_eval`.

The stdlib directory is `$TUR_STDLIB_DIR` when set, else `stdlib_root`, else a
cwd-relative `stdlib`; when `TUR_STDLIB_DIR` is unset and `stdlib_root` is
given, it is exported as `TUR_STDLIB_DIR`, which a module import's stdlib
fallback reads. A program you `tur build` can bake in the stdlib it was built
against with the autolink hint `-DNAME=@TUR_STDLIB_ROOT@`, which `tur`
replaces with that root as a C string literal:

```c
/* __tur_autolink__: -lturi -Isrc -Isrc/compiler -Isrc/runtime -DMY_STDLIB=@TUR_STDLIB_ROOT@ */
#ifndef MY_STDLIB
#define MY_STDLIB ""
#endif
turi_env_preload_stdlib(env, MY_STDLIB);
```

It is not the REPL's preload: that adds the `Show` slice, which puts stdlib
`String` in the global scope, where a module that defines a `String` of its
own can no longer export it.

### `void turi_env_register_native_ex(...)` -- native ud that the env owns

```c
typedef void (*TuriNativeFreeFn)(void *ud);
void turi_env_register_native_ex(TuriEnv *env, const char *name,
                                 TuriNativeFn fn, void *ud,
                                 TuriNativeFreeFn free_ud);
```

Same as `turi_env_register_native`, but `free_ud` (when non-NULL) is invoked
with `ud` from `turi_env_free`. This lets a native registered with
`ud = a per-script object` tie that object's lifetime to the env it serves --
the common embedder pattern where a native calls back into the script for
property routing or signal emission. With one env per script, env lifetime ==
script lifetime, so the `ud` is reclaimed exactly when the script goes away.

Finalizers fire in LIFO order at teardown and are **not** fired by
`turi_env_reset` (natives survive a reset, so their `ud` must too). A native
whose `ud` outlives the env -- a process-global, or anything registered via
`turi_register_default_native` (shared across every env) -- must **not** take a
finalizer; use plain `turi_env_register_native` for those. `TuriNativeSpec` also
gained a `free_ud` field, honoured by `turi_env_new_with_natives`.

```c
turi_env_register_native_ex(env, "godot-emit", native_emit,
                            script, (TuriNativeFreeFn)script_release);
```

### `void turi_env_set_interpret_mode(TuriEnv *env, bool interpret)`

Sets a per-env bit that is snapshotted into the process-global elaborator mode
flag for the duration of each `turi_eval` on the env, then restored. Every
`turi_env_new` defaults it to `true` (interpret mode -- registered natives
resolve at runtime). An embedder driving compile-mode elaboration through the
same process flips it to `false`.

The point is co-residency: before this, the *last* `turi_env_new` to run set the
global for everyone, so two libturi embedders in one process (e.g. a Godot
binding plus an MCP worker) could clobber each other's mode. Now each env
re-installs its own bit on every eval. (The elaborator still reads a
process-global *within* a single eval; threading the mode all the way through
the elaboration context per-env remains future work, but the cross-eval
clobber is gone.)

### `void turi_env_set_shared_spice_image(TuriEnv *env, struct TurSpiceImage *image)`

Points an env at another (prototype) env's already-loaded spice image instead of
auto-discovering and owning its own copy. The borrowing env will **not** free the
image in `turi_env_free` -- the prototype owns it and must outlive every
borrower. Pass the prototype's `spice_image` field; pass `NULL` to detach.

```c
TuriEnv *proto = turi_env_new();          /* auto-discovers the project image  */
turi_eval(proto, "...");
TuriEnv *script = turi_env_new();
turi_env_set_shared_spice_image(script, proto->spice_image);  /* borrow, no copy */
```

This is a **measure-first** hook: the per-env image cost is only worth
optimizing once a real project shows many envs duplicating a nontrivial image,
and it is safe only when no borrower triggers `(reload)` on the shared image (a
reload would swap the prototype's image out from under the borrowers). It
deliberately avoids copy-on-write arena machinery until measurement justifies
it.

> **Debug-only cross-env guard.** In debug builds (`assert`/ASan, i.e. not
> `NDEBUG`) each `TuriClosure` carries an origin-env tag claimed on first
> application and checked on every later one. Caching a closure value across
> per-script envs -- forbidden, because closures pin their originating env's
> arenas -- aborts with a clear message instead of a heap use-after-free.
> Release builds compile the field and check out entirely (zero cost).

---

## Async API

The async scheduler is cooperative and single-threaded.  `(async ...)` spawns
a fiber; `(await ...)` suspends the current fiber until a future resolves.
From C, call `turi_run_event_loop` after evaluating async code to drive
completion.

### `void turi_run_event_loop(TuriEnv *env)`

Runs the cooperative scheduler until all pending fibers, timers, and I/O
callbacks complete.  Call after any `turi_eval` that starts async tasks.

```c
turi_eval(env, "(defn work [] :int (await (async (fn [] :int 42))))");
TuriValue r = turi_eval(env, "(work)");
/* r.as_int == 42  -- (await ...) in main context drives the loop inline */
```

### `TuriValue turi_task_spawn(TuriEnv *env, const char *src)`

Evaluates `src` as a zero-argument closure and spawns it as an async task.
Returns a `TURI_FUTURE` value.

### `void turi_task_cancel(TuriEnv *env, TuriFuture *f)`

Cancels the fiber owning `f`.  The future is rejected; any `(await f)` yields
a `TURI_REJECTION` value, observed with `(error? r)` / `(error-message r)`.

### `TuriValue turi_future_poll_val(TuriFuture *f)`

Non-blocking poll.  Returns the resolved value, a `TURI_ERROR` if rejected,
or `TURI_NIL` if still pending.

### `TuriValue turi_sleep_async(TuriEnv *env, uint64_t ms)`

Returns a `TURI_FUTURE` that resolves after `ms` milliseconds.  The current
fiber (if any) should `(await)` this future to suspend.

---

## Thread safety

`TuriEnv` is **not** thread-safe.  Use one environment per thread, or protect
access with an external lock.

---

## Error handling pattern

```c
TuriValue v = turi_eval(env, user_input);
if (turi_is_error(v)) {
    fprintf(stderr, "error: %s\n", turi_error_message(v));
}
```

Note: `TURI_ERROR` is a value-level error (parse failure, unbound variable).
Async-task rejection surfaces as a distinct `TURI_REJECTION` value; check with
`turi_is_rejection(v)` and read the message with `turi_error_message(v)`.

---

## Interpreter value semantics: `::` is value-preserving, not bit-preserving

The tree-walking interpreter (`turi`, behind `tur interpret`, `tur repl`,
sandbox `turi_eval`, and the WASM REPL) holds values as **tagged** `TuriValue`s
(`TURI_INT` carries a 64-bit word, `TURI_FLOAT` carries a `double`), whereas the
compiled path carries every scalar as a raw `int64_t`.  This difference used to
be observable in one spot: a `::` ascription that crossed the **float/int
representation boundary**.

`(:: expr T)` between two **same-size but distinct** scalar kinds lowers to a
bit-level reinterpret (`EX_REINTERPRET`).  Whether that reinterpret is
observable depends on the pair:

| `::` between kinds                                          | Bits change meaning? | turi vs compiled |
| ---------------------------------------------------------- | -------------------- | ---------------- |
| `int`<->`cstr`, `int`<->`ptr<void>`, `int`<->opaque newtype, `int8`<->`uint8`, `sym`<->`ptr` | no                   | **identical**    |
| `float`<->`int` (8-byte), `float32`<->`int32` (4-byte)         | **yes**              | **rejected at compile time** |

For the first group -- which is the overwhelming majority of real `::` use
(unwrapping `defopaque`/refined newtypes like `Fd`, `Pid`, `EventSourceId`,
`NonEmpty`; pointer and `cstr` carriers) -- the underlying bits are identical,
so the interpreter's tag-preserving passthrough produces exactly the compiled
result.

**The float/int pair no longer diverges, because `::` no longer spells it.**
That ascription is a hard error on both paths: the operator cannot tell a
genuine integer being *converted* from a carrier slot holding float bits being
*reinterpreted*, and the two readings disagree about the answer, so the author
writes which one they meant.

```turmeric
(:: 7.1 :int)     ; error: `::` between an integer and a float kind is ambiguous
(float->int 7.1)  ; 7                    -- the NUMBER   (stdlib/math.tur)
(float->bits 7.1) ; 4619679907765970534  -- the IEEE-754 BITS (stdlib/bits.tur)
```
```sweet-exp
(:: 7.1 :int)     ; error: `::` between an integer and a float kind is ambiguous
float->int(7.1)  ; 7                    -- the NUMBER   (stdlib/math.tur)
float->bits(7.1) ; 4619679907765970534  -- the IEEE-754 BITS (stdlib/bits.tur)
```

(An integer *literal* ascribed to a float still converts -- `(:: 7 :float)` is
`7.0` -- since a written constant has no carried bit pattern to mean.  See
[docs/archive/ascribe-int-to-float-expression-ambiguity.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/ascribe-int-to-float-expression-ambiguity.md).)

All four spellings behave **identically under turi and compiled**.  The
interpreter registers `float->bits` / `bits->float` as natives, exactly as it
already did for `int->float` / `float->int`: once the author has said which
reading they meant, the tagged model can answer precisely -- hand back the other
tag over the same 64 bits -- so both observing a bit pattern and round-tripping
a float through an `:int` carrier agree with the compiled path.

**Why `::` could not simply pick one.** A `TURI_INT` cannot be distinguished as
"a genuine integer" from "a carrier holding float bits", so a bit-reinterpreting
`::` breaks the round-trips that rely on tag preservation (it would corrupt a
`Cons[float]`'s `.head` to its integer bit pattern), while a converting `::`
breaks the carrier read-backs that typed slots, variadic rest collection and the
cons/HAMT carriers depend on.  Value-preserving was the only self-consistent
choice for the tagged model as long as one operator had to serve both; splitting
the spelling removed the need to choose.  See
[docs/archive/history/turi-map-nonint-value-carrier-ascription.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/turi-map-nonint-value-carrier-ascription.md).

**Practical guidance.** Bit-level float work (hashing, representation-exact
serialization, manual IEEE/NaN-boxing) goes through `float->bits` /
`bits->float` and runs on either path.  Note the stdlib's own float
`Hash`/`MapKey` reinterprets go through inline-C `union`s that the interpreter
overrides with **natives**, not through `::`, so float-keyed maps and sets hash
correctly under the interpreter.

---

## Not interpreted: carve-outs

A handful of language features run only on the compiled path (`tur build` /
`tur run`).  Under the tree-walking interpreter (`tur interpret`, `tur repl`,
sandbox `turi_eval`, the WASM REPL) they raise a clean, value-level
`TURI_ERROR` -- never a wrong answer, and never the generic "unhandled
expression kind" default.  Check for them with `turi_is_error` as usual.

| Feature | Interpreter behaviour | Why |
| --- | --- | --- |
| **User inline-C** (` ```c ... ``` ` bodies) | `inline-C not supported in interpreter mode` | The interpreter has no C compiler.  Stdlib inline-C is covered by registered **natives** (see `turi_env_register_native`) and the simple-shape evaluator; arbitrary user inline-C is not. |
| **Channels / `select`** | `select is not supported in interpreter mode (channels require native primitives; use the compiled path)` | Turmeric channels are inline-C `pthread` mutex/condvar ring buffers with no native representation in `turi`, so `select` has nothing to select over.  Tracked in [docs/archive/history/turi-select-needs-channel-primitives.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/turi-select-needs-channel-primitives.md). |
| **WASM async** | scheduler-dependent error | The WASM build drives async through the host event loop; the standalone interpreter's cooperative scheduler does not cover the WASM async path.  See `src/turi/fiber.c`. |
| **`call/cc`, context-capturing `serial-shift` / `cloneable-shift`** | clean error | Need genuine continuation capture; deferred to the CPS-transform work.  See `docs/turi-carve-out.txt`. |
| **Multi-shot `resume` under a non-capturable handle** | `resume: this continuation has already been resumed ...` | A `handle` runs on the work-stack driver -- where a continuation is a heap slice cloned per resume, so multi-shot works -- only when every `perform` it can reach is reached through a form the driver descends: plain control flow (`if`, `do`, `let`, `while`, `match`), `set!`/`return`/ascription operands, field receivers, perform args, resume's `k`, and direct calls.  Reaching one through a native higher-order call, a `catch-unwind`/`reset`/`atomically` boundary, or a match *guard* falls back to a ucontext fiber, which *is* the continuation and so is single-shot.  The second resume reports rather than aborting, and `TURI_TRACE_FIBER_FALLBACK=1` names the form that forced the fallback.  See [docs/archive/turi-ws-capturable-stale-black-box-arms.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-ws-capturable-stale-black-box-arms.md). |

These are intentional and enforced: `tools/check_turi_parity.py` (wired into
`tests/run.sh`) checks every `EX_*` kind against `docs/turi-carve-out.txt`, so a
carve-out can neither silently regress to the generic default nor go stale.

---

## Full example -- sandboxed calculator

```c
#include <stdio.h>
#include <turi/eval.h>

int main(void) {
    turi_init(false);
    TuriEnv *env = turi_env_new_sandboxed();

    const char *exprs[] = {
        "(defn fact [n :int] :int (if (<= n 1) 1 (* n (fact (- n 1)))))",
        "(fact 10)",
        "(fact 20)",
    };

    for (int i = 0; i < 3; i++) {
        TuriValue v = turi_eval(env, exprs[i]);
        if (turi_is_error(v)) {
            fprintf(stderr, "error: %s\n", turi_error_message(v));
        } else {
            char buf[64];
            turi_value_repr(buf, sizeof(buf), v);
            printf("%s => %s\n", exprs[i], buf);
        }
    }

    turi_env_free(env);
    return 0;
}
```
