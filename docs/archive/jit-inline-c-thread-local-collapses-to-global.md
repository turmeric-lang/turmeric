# Under `tur jit`, inline C's `_Thread_local` silently becomes one global shared by every thread

**Severity:** medium (silent wrong value). The JIT guide promises the
opposite: `_Thread_local` / `__thread` "always take the fallback", because
"collapsing a thread-local to a global would trade a clean compile error for
silent cross-thread corruption under `spawn`" (`docs/guides/jit-guide.md:403`).
That holds for `__thread`, which c2mir does not know, so the compile fails and
falls back to cc (TUR-W0070). It does not hold for the C11 spelling
`_Thread_local`. c2mir accepts it, warns `Thread local is not implemented --
program might not work as assumed`, and compiles a plain global. The program
then runs under the JIT.

The emitted runtime is not affected: its thread-locals go through host
accessors under the JIT (`src/runtime/tur_tls.c`), and the r7rs prelude's two
`_Thread_local`s sit behind `__GNUC__`, which c2mir does not define. The
exposure is user and spice inline C. No stdlib or spice `.tur` file uses
`_Thread_local` today.

Filed 2026-10-24, found while checking a claim about r7rs-heap-frames-plan H6.

## Repro

```turmeric
(defn tls-probe [] : int
  ```c
  /* __tur_include__: #include <pthread.h> */
  /* __tur_include__: static _Thread_local long tl_counter; */
  /* __tur_include__: static void *tls_worker(void *a) { (void)a; tl_counter = 99; return 0; } */
  pthread_t t;
  tl_counter = 7;
  pthread_create(&t, 0, tls_worker, 0);
  pthread_join(t, 0);
  return (int)tl_counter;
  ```)

(defn main [] : int
  (println (tls-probe))
  0)
```

```sh
./build-release/tur run tls.tur   # 7
./build-release/tur jit tls.tur   # <tur-jit>:51:8: warning -- Thread local is not implemented ...
                                  # 99
```

With `__thread` in place of `_Thread_local`, `tur jit` prints
`conversion of non-scalar value requested`, then TUR-W0070, then `7`. That is
the documented fallback.

## Root cause

`external/mir/c2mir/c2mir.c:4730`: a `T_THREAD_LOCAL` storage class is an error
only under `pedantic_p`, and a warning otherwise. `jit_compile`
(`src/jit_engine.c:1081`) sets `message_file` and `ignore_warnings_p`, and
nothing turns that warning into a compile failure. Under
`tur_jit_set_quiet_warnings` the warning is not even printed.

## Fix directions

- Cheapest, in-tree: make the JIT prelude (`JIT_PRELUDE`,
  `src/jit_engine.c:152`) reject the keyword, for example
  `#define _Thread_local __tur_jit_no_thread_local` placed after any use the
  runtime still needs. The declaration then fails to parse, and the existing
  TUR-W0070 fallback to cc takes over. First confirm that no `TUR_THREAD_LOCAL`
  use still reaches c2mir in a full (unpruned) TU: the macro expands to
  `_Thread_local` under c2mir (`src/compiler/emit_module.c:15256`).
- Or, in the fork: a c2mir option (or `pedantic_p` for this one diagnostic) that
  makes `_Thread_local` an error. Batch it into the fork's one open PR.
- Pin it with the repro as a `tests/run-jit.sh` case: the expected output is `7`,
  with TUR-W0070 allowed.

## Resolution (2026-10-29)

Fixed in-tree, at the warning itself rather than with a prelude `#define`.
That route would also have poisoned the preamble's own `TUR_THREAD_LOCAL`,
which expands to `_Thread_local` under c2mir. `jit_compile`
(`src/jit_engine.c`) now gives c2mir an `open_memstream` as its
`message_file` and turns warnings on. After the compile it relays the
messages to stderr (still dropping warnings when quiet). If one of them is
"Thread local is not implemented", it reports the compile as
`TUR_JIT_ERR_COMPILE` with a one-line reason. cmd_jit's TUR-W0070 fallback
then runs the program with cc, as it already did for `__thread`. The warning
fires only for a declaration c2mir actually compiles, so a guarded one (the
r7rs prelude's, behind `__GNUC__`) does not trip it. Windows keeps the old
behaviour: it has no `open_memstream`.

The repro now prints `7` under `tur jit`, after TUR-W0070. Pinned by the
fixture `jit-inline-c-thread-local`, which `tests/run-jit.sh` allows to fall
back (`tests/jit-fallback-baseline.txt`). The JIT guide's "always take the
fallback" line now says how.
