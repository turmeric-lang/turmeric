# `(scheme eval)` registered a host procedure under a stack-buffer name

**RESOLVED 2026-10-07.** Filed and fixed the same day; it had never been
reported, but it failed every `JIT engine (ubuntu-latest)` run on `main` that
was checked (37412067483, 37484233418, 37514742502) as `FAIL r7rs-eval --
stdout mismatch`. The leg is `continue-on-error`, so nothing turned red.

**Severity was:** medium -- a read of a dead stack frame on every program that
hands a program procedure to evaluated code. Release builds printed the right
answer only because the stale bytes happened to survive.

## Repro

On a Debug (ASan) build with the JIT:

```sh
cd tests/fixtures/r7rs-eval
ASAN_OPTIONS=detect_leaks=0 ../../../build/tur jit input.tur
```

```
ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 1 ... thread T2
    #0 ht_find src/turi/env.c:91
    #1 turi_env_set src/turi/env.c:748
    #2 eval_expr_impl src/turi/eval.c:11975
    ...
    #11 embed_eval_text src/turi/r7rs_embed.c:127
    #13 turi_r7rs_embed_eval src/turi/r7rs_embed.c:479
Address ... is located in stack of thread T2 at offset 4432 in frame
    turi_eval_impl ... 'saved_files' <== Memory access ... overflows this variable
```

ASan's "may be a false positive with a custom stack unwind" hint was a red
herring: the address was a real dangling pointer.

## Root cause

`host_value` (`src/turi/r7rs_embed.c`) wraps a program procedure for the
evaluated environment by registering a native named
`r7rs-host-procedure-<id>__`. It formatted that name into `char name[64]` on
its own stack and passed it to `turi_env_register_native`. A new global binding
keeps the NAME POINTER (`turi_env_set`, `src/turi/env.c:755`) -- every
`EnvBinding->name` is expected to live in the env's `sym_arena`. Once
`host_value` returned, the binding's key pointed into dead stack. A later
`turi_env_set` whose hash probe passed that slot compared against it
(`b->name[0] == name[0]`), reading whatever frame had reused the memory -- here
`turi_eval_impl`'s redzone.

All three back ends reach `host_value`. Compiled fixtures run unsanitized, so
`run.sh` could not see it. The interpreter (`native_r7rs_eval_c_eval`) runs it
inside the sanitized `tur` too, but ASan reports only a read of POISONED
bytes: under `--interpret` the stale key happened to land in addressable stack
of a later frame, so the read went unreported. Only the JIT's frame layout put
it on a redzone.

## Fix

`host_value` copies the formatted name into `g_env->sym_arena` before
registering it -- the same pattern `turi_env_register_native_caps` and the
private-defn qualified key in `eval.c` already used. The lifetime rule is now
written on `turi_env_register_native` (`src/turi/eval.h`) and `turi_env_set`
(`src/turi/env.h`); no other caller passed a transient name.

Verified: the fixture prints `expected.stdout` with no ASan report under the
Debug JIT, and all nine `(scheme eval)` / `(scheme load)` fixtures pass on
`run-jit.sh` and `run.sh`, plus `tests/run-r7rs-import.sh`.
