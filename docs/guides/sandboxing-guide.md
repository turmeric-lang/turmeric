---
title: Sandboxing Guide
category: Interoperability
description: Restricting Turmeric code inside a C host with turi_env_new_sandboxed, capability flags, the native classification table, and resource limits -- and the memory-safety gap that keeps it from being a boundary against hostile code
---

# Sandboxing Guide

The libturi embedding API provides a sandboxed evaluation environment for
Turmeric code -- REPL widgets, plug-in scripts, user-supplied formulas --
inside a C host process with I/O, FFI, and unsafe memory operations denied.

> **What the sandbox enforces.** Every capability is enforced -- each native
> function has a row in one [classification table](#capability-classification)
> and the native dispatch refuses a call the environment has no capability for
> -- so a sandboxed script cannot open a file, spawn a process, or read the
> environment, however it spells the call. Memory safety is enforced too: a
> native takes a collection / string / iterator / continuation handle as a bare
> integer, and in a restricted env a per-env handle-provenance registry refuses
> any handle that was not minted as the kind it is used as -- whether it reaches
> a native or one of the interpreter's own re-tags (an erasing ascription to
> `cstr` or a struct, a field read, a call). So `(vec-get 4096 0)`,
> `(:: 4096 cstr)`, a kind-confused replay and a use-after-free are refused
> instead of reading or writing an arbitrary address, while a genuinely built
> collection still round-trips. This was S-5 in the
> [security audit plan](https://github.com/turmeric-lang/turmeric/blob/main/docs/upcoming/security-audit-plan.md),
> resolved 2026-10-07; the [Security Guide](security-guide.md#t3-the-sandboxed-interpreter)
> states the promise and its limits. It is an in-process boundary: for code from
> someone you do not trust, also run the embedding in a separate, unprivileged
> process.

See [eval-api.md](eval-api.md) for the full C embedding API reference.

---

## Quick Start

```c
#include <turi/eval.h>

int main(void) {
    turi_init(false);
    TuriEnv *env = turi_env_new_sandboxed();

    TuriValue v = turi_eval(env, "(+ 1 2)");
    /* v.tag == TURI_INT, v.as_int == 3 */

    /* I/O is blocked */
    TuriValue bad = turi_eval(env, "(println 42)");
    /* bad.tag == TURI_ERROR */

    turi_env_free(env);
    return 0;
}
```

`turi_env_new_sandboxed()` returns an environment identical to `turi_env_new()`
except that the operations listed below are disabled.  Arithmetic, closures,
recursion, structs, algebraic effects, and pure stdlib functions all work
without restriction.

---

## What the Sandbox Blocks

### I/O, filesystem, processes, and the environment

The `println` family returns `TURI_ERROR` in a sandboxed environment, and so
does every native function whose row in the
[classification table](#capability-classification) names a capability the
environment does not hold: `process/spawn-raw` (under `process/spawn`), `io-fopen-write`, `r7rs-unlink__`,
`r7rs-getenv__`, the raw-descriptor `read-async`/`write-async`, and the rest.
The check is made once, where every native call is dispatched, so a native
reached by name, through `turi_call`, or from a higher-order native is refused
the same way:

```
eval: 'process/spawn-raw' requires capability proc, which this environment does not hold
```

`(load "path")` is refused alongside `(import ...)`.

### FFI (dynamic loading)

`dlopen`, `dlsym`, and `dlclose` are blocked.  A sandboxed script cannot load
native shared libraries.  `extern-c` declarations need `TURI_CAP_FFI` too,
including the ones the interpreter implements itself (`printf`, `puts`,
`getenv`, `exit`, `strlen`, `free`), which additionally need the capability of
what they do.

### Inline-C expressions

Any expression of the form `` (` ``c ... `` `) `` is rejected at evaluation
time with `TURI_ERROR`.

### Async forms

`(async ...)` is blocked.  Sandboxed code cannot spawn fibers or use the
cooperative scheduler.

---

## Resource Limits

A sandboxed environment caps total evaluation work so an infinite loop cannot
hang the host.

### Step fuel

The step-fuel counter decrements once per AST node evaluated.  When it reaches
zero, `turi_eval` returns `TURI_ERROR` immediately.

```c
/* Default sandboxed fuel: TURI_DEFAULT_SANDBOX_FUEL (10,000,000 steps).
 * Override after creating the environment: */
turi_env_set_fuel(env, 500000);   /* tighter limit for short formulas */
turi_env_set_fuel(env, 0);        /* 0 = unlimited (not recommended) */
```

Fuel checking is skipped entirely in unrestricted environments
(`step_fuel_limit == 0`), so there is no performance cost to non-sandboxed code.

### Recursion depth

There is no recursion-depth guard: interpreter recursion is heap-bounded, so
deep recursion cannot overflow the host's C stack. `turi_env_set_max_depth`
is retained as a **no-op** for API/ABI compatibility -- bound total work with
`turi_env_set_fuel` instead.

`TURI_DEFAULT_SANDBOX_FUEL` is `#define`d in `eval.h` and can be overridden at
compile time.

---

## Capability Allow-List

When you need a mix of restrictions -- for example, async is safe for your use
case but filesystem I/O is not -- use the capability API instead of the boolean
`sandboxed` flag.

```c
typedef uint32_t TuriCaps;

#define TURI_CAP_IO        (1u << 0)  /* stdout/stdin, raw-fd read/write, pipes */
#define TURI_CAP_FFI       (1u << 1)  /* dlopen/dlsym/dlclose, extern-c */
#define TURI_CAP_INLINE_C  (1u << 2)  /* inline-C expressions */
#define TURI_CAP_ASYNC     (1u << 3)  /* (async ...) and the scheduler natives */
#define TURI_CAP_UNSAFE    (1u << 4)  /* raw-malloc, ptr-deref, ... */
#define TURI_CAP_IMPORT    (1u << 5)  /* (import ...) and (load ...) */
#define TURI_CAP_FS        (1u << 6)  /* open/create/remove/stat by path */
#define TURI_CAP_PROC      (1u << 7)  /* spawn, wait, exit the host process */
#define TURI_CAP_ENV       (1u << 8)  /* getenv / environ */
#define TURI_CAP_EVERY     /* every bit above */
#define TURI_CAP_ALL       (~(TuriCaps)0)
#define TURI_CAP_NONE      ((TuriCaps)0)
```

`turi_env_new()` grants `TURI_CAP_ALL`.  `turi_env_new_sandboxed()` starts with
`TURI_CAP_NONE`.  Use `turi_env_allow` and `turi_env_deny` to adjust:

```c
/* Sandbox that permits async but not I/O or FFI. */
TuriEnv *env = turi_env_new_sandboxed();
turi_env_allow(env, TURI_CAP_ASYNC);

TuriValue ok  = turi_eval(env, "(async (fn [] :int 42))"); /* allowed */
TuriValue bad = turi_eval(env, "(println 1)");             /* TURI_ERROR */
```

```c
/* Unrestricted env with the outside world revoked -- useful inside a plugin
 * host that wants the script to print but touch nothing else. */
TuriEnv *env = turi_env_new();
turi_env_deny(env, TURI_CAP_FS | TURI_CAP_PROC | TURI_CAP_ENV | TURI_CAP_FFI);
```

A native that needs more than one capability runs only when the environment
holds all of them.

Query whether a capability is active (`turi_env_has_cap` answers true when
*any* of the bits passed is held, so query one bit at a time):

```c
if (!turi_env_has_cap(env, TURI_CAP_IO)) {
    /* safe to expose to user input */
}
```

### API summary

```c
/* Grant a capability to a sandboxed environment. */
void turi_env_allow(TuriEnv *env, TuriCaps cap);

/* Revoke a capability from any environment. */
void turi_env_deny(TuriEnv *env, TuriCaps cap);

/* Query whether a capability is currently granted. */
bool turi_env_has_cap(TuriEnv *env, TuriCaps cap);

/* Adjust the step-fuel limit. */
void turi_env_set_fuel(TuriEnv *env, uint64_t steps);

/* Retained as a no-op for API/ABI compatibility (the recursion-depth
 * guard is retired; interpreter recursion is heap-bounded). */
void turi_env_set_max_depth(TuriEnv *env, uint32_t depth);
```

---

## Blocking `(import ...)` and Pre-Loading Stdlib

`(import ...)` triggers filesystem reads.  In a sandboxed environment the
`TURI_CAP_IMPORT` bit is unset, so any `(import ...)` expression returns
`TURI_ERROR` without touching the filesystem.

If sandboxed scripts need stdlib functions, pre-load them in an unrestricted
environment and copy the bindings via `turi_env_set` before sandboxing:

```c
TuriEnv *boot = turi_env_new();
turi_eval_file(boot, "stdlib/list.tur");
TuriValue list_map = turi_env_get(boot, "list/map");

TuriEnv *sandbox = turi_env_new_sandboxed();
turi_env_set(sandbox, "list/map", list_map);   /* inject binding */

/* Sandboxed code can now call list/map without importing anything. */
turi_eval(sandbox, "(list/map (fn [x :int] :int (* x 2)) ...)");

turi_env_free(boot);
```

Do not grant `TURI_CAP_IMPORT` in a sandboxed environment unless you fully
control the filesystem paths reachable from the script.

---

## Native Functions in Sandboxed Environments

Sandboxed environments accept `turi_env_register_native` calls.  Your native
functions run with the same privileges as the host process; the sandbox only
constrains what *Turmeric code* can call.  Validate arguments carefully:

```c
static TuriValue safe_sqrt(TuriEnv *env, TuriValue *args,
                            uint32_t n, void *ud) {
    (void)ud;
    if (n != 1 || args[0].tag != TURI_FLOAT)
        return turi_error("sqrt: expected one float");
    double x = args[0].as_float;
    if (x < 0.0) return turi_error("sqrt: negative argument");
    return turi_float(sqrt(x));
}

turi_env_register_native(sandbox, "safe-sqrt", safe_sqrt, NULL);
```

A native registered under a name of your own carries no capability
requirement: you exposed it, so it runs. A native registered under a name the
[classification table](#capability-classification) already lists takes that
row's requirement, so re-registering `io-fopen-read` does not quietly give a
sandbox file access. When you want to say which it is, use the explicit form:

```c
/* Gate your own native behind a capability ... */
turi_env_register_native_caps(env, "host/read-config", read_config, NULL,
                              TURI_CAP_FS);

/* ... or deliberately expose one under a classified builtin name. */
turi_env_register_native_caps(env, "r7rs-getenv__", fake_getenv, NULL,
                              TURI_CAP_NONE);
```

Only expose native functions you are willing to let untrusted code call.

If your native hands Turmeric a pointer as a bare integer and takes it back
later, check it on the way back in: the handle table only knows the built-in
natives, so sandboxed code can pass yours any integer. The registry the
built-ins use is public (`src/turi/eval.h`) -- record the pointer when you hand
it out and check it when it returns; both calls do nothing in an unrestricted
env:

```c
turi_prov_register(env, TURI_HK_GENERIC, conn);              /* handing it out */

if (!turi_prov_check(env, TURI_HK_GENERIC, (void *)(intptr_t)args[0].as_int))
    return turi_error("conn: not a live connection");      /* taking it back */
```

---

## Error Handling

Every blocked operation returns a `TURI_ERROR` value rather than aborting the
process.  Always check the return value:

```c
TuriValue v = turi_eval(env, user_input);
if (turi_is_error(v)) {
    fprintf(stderr, "sandbox error: %s\n", turi_error_message(v));
    /* recover, report, or discard */
}
```

Step-fuel exhaustion also surfaces as `TURI_ERROR`:

```
sandbox error: eval: step fuel exhausted
```

So does a panic. An environment without `TURI_CAP_PROC` may not end your
process, so a panic nothing in the script catches -- `(panic ...)`, an
out-of-bounds `vec-get`, a failed contract -- stops the script and comes back
from `turi_eval` or `turi_call` as an error, and the environment stays
usable:

```
sandbox error: panic: vec index out of bounds
```

A `catch-unwind` inside the script still catches first. An environment that
holds `TURI_CAP_PROC` keeps the compiled program's behaviour: print the panic
and exit.

---

## Full Example -- Sandboxed Formula Evaluator

```c
#include <stdio.h>
#include <turi/eval.h>

int main(void) {
    turi_init(false);

    TuriEnv *env = turi_env_new_sandboxed();
    turi_env_set_fuel(env, 1000000);

    /* Expose a safe native. */
    turi_env_register_native(env, "safe-sqrt", safe_sqrt, NULL);

    /* User-supplied expressions evaluated in isolation. */
    const char *formulas[] = {
        "(defn hyp [a :float b :float] :float"
        "  (safe-sqrt (+ (* a a) (* b b))))",
        "(hyp 3.0 4.0)",
        "(hyp 5.0 12.0)",
        "(println-float 1.0)",   /* blocked -- no IO cap */
    };

    for (int i = 0; i < 4; i++) {
        TuriValue v = turi_eval(env, formulas[i]);
        if (turi_is_error(v)) {
            fprintf(stderr, "[error] %s\n", turi_error_message(v));
        } else {
            char buf[64];
            turi_value_repr(buf, sizeof(buf), v);
            printf("%s\n", buf);
        }
    }

    turi_env_free(env);
    return 0;
}
```

Expected output:

```
#<fn hyp>
5.0
13.0
[error] eval: 'println-float' requires capability io, which this environment does not hold
```

---

## Capability Reference Table

| Capability | `TURI_CAP_*` bit | Blocked by default | What it covers |
|---|---|---|---|
| I/O | `TURI_CAP_IO` | yes | `println`, stdout writers, `read-async`/`write-async` on raw descriptors, pipes, the reactor, the stdio ports |
| Filesystem | `TURI_CAP_FS` | yes | opening, creating, removing and testing files by path |
| Process | `TURI_CAP_PROC` | yes | `process/spawn-raw`, `process/wait-raw` (under `process/spawn` / `process/wait` / `process/run`), exiting the host process |
| Environment | `TURI_CAP_ENV` | yes | `getenv`, `environ` |
| FFI | `TURI_CAP_FFI` | yes | `dlopen`, `dlsym`, `dlclose`, every `extern-c`, spice reload |
| Inline-C | `TURI_CAP_INLINE_C` | yes | `` (` ``c ... `` `) `` expressions |
| Async | `TURI_CAP_ASYNC` | yes | `(async ...)` forms and the scheduler natives |
| Unsafe memory | `TURI_CAP_UNSAFE` | yes | `raw-malloc`, `ptr-deref`, `ptr-write`, `raw-memset`, the raw-address natives |
| Import | `TURI_CAP_IMPORT` | yes | `(import ...)` and `(load ...)` |

All nine capabilities are denied when you call `turi_env_new_sandboxed()`.
Use `turi_env_allow` to selectively re-enable any subset.

## Capability Classification

Every native function a new environment holds has one row in
`src/turi/native_caps.c` naming the capabilities a caller must hold. The table
is the source of truth; the sandbox test (`tests/turi/sandbox-eval.c`) fails if
a native a fresh environment holds has no row, or if any row with a
requirement can be called from a sandboxed environment.

A native is classified by what its *caller* can make it do. One that writes
`stderr` only on its own panic path is still pure; one that takes a raw
descriptor is I/O even though it opens nothing, because descriptors 0, 1 and 2
need no open. About six hundred natives are pure; these are the rest:

| Class | Natives |
|---|---|
| `proc` | `process/spawn-raw`, `process/wait-raw`, `process/child-of-raw`, `r7rs-exit__` |
| `fs` | `fs/tmpfile`, `fs/tmpfile-path`, `fs/tmpfile-fd`, `fs/tmpfile-free`, `io-fopen-read`, `io-fopen-write`, `io-fread-chunk`, `io-fwrite-chunk`, `io-fclose`, `io-remove`, `write-temp-file`, `json/decode-file!`, `r7rs-io-open__`, `r7rs-file-exists-c__`, `r7rs-unlink__` |
| `fs`, `io` | `random-access-bench` |
| `env` | `r7rs-getenv__`, `r7rs-getenv-set?__`, `r7rs-environ-count__`, `r7rs-environ-name__`, `r7rs-environ-value__` |
| `io` | `println-float`, `show-string-fputs`, `bt-print`, `doc-print`, `run-ring`, `run-nbody`, `read-async`, `write-async`, `async-pipe-init`, `r7rs-io-std__`, `reactor-new`, `tur_reactor_new`, `tur_reactor_poll`, `break` |
| `async` | `sleep-async`, `with-timeout`, `async-all2`, `await-val`, `async-race`, `cancel-task`, `task-cancelled?` |
| `ffi` | `reload` |
| `unsafe` | `box`, `unbox`, `io-alloc`, `io-free`, `io-buf-new`, `io-buf-free`, `int-val`, `alloc-int`, `alloc-key`, `alloc-str`, `flat-new`, `flat-get`, `flat-set`, `array-get`, `array-set` |
| every capability | `r7rs-eval-c-eval__`, `r7rs-eval-c-load__` -- they evaluate text in the process-global R7RS `eval` environment, which holds every capability -- and the rest of the `r7rs-eval-c-*__` family (`-apply__`, `-push-*__`, `-answer-*__`, `-frame-*__`, `-result-*__`), which act on that same environment |

`unsafe` is given to the natives whose only purpose is to allocate, free or
dereference a raw address with no typed wrapper. It is not given to the
collection and string natives, although they take handles as bare integers
too; a sandbox without vectors and maps would be useless. That gap is not a
capability's to close -- it is closed by the handle provenance registry (S-5):
in a restricted env each handle argument is checked against the handles minted
as the kind its position names, so a forged integer -- or a string literal, a
float or a struct where a vector is expected -- is refused while a real handle
round-trips. The per-native handle-kind column lives beside this table in
`src/turi/native_caps.c` (`k_handle_rows[]`), and the sandbox test sweeps every
capability-free native with forged arguments so a native nobody classified is
found. The
[S-5 report](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/turi-sandbox-handles-are-forgeable-integers.md)
has the model and the limits: `String`-keyed maps and sets are refused in a
restricted env, scratch promotion is off there, and re-entrant `call/cc` is
escape-only outside a top-level form.

The operations that are not native functions are checked where they are
evaluated: the `println` builtins and the raw-memory builtins in the builtin
dispatch, `dlopen` and friends and `extern-c` on the FFI path, inline C, the
`(async ...)` form, and `import`/`load` in the elaborator.

---

## See Also

- [security-guide.md](security-guide.md) -- what Turmeric promises at each trust boundary
- [eval-api.md](eval-api.md) -- full C embedding API reference
- [c-integration-guide.md](c-integration-guide.md) -- FFI and inline-C
- [compiler-flags-guide.md](compiler-flags-guide.md) -- `-X` feature flags
