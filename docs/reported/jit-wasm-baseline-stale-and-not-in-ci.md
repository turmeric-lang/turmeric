# `tests/jit-wasm-baseline.txt` has drifted: 11 fixtures are worse than their baseline, and no CI job runs the harness

**Severity:** low (test infrastructure). `tests/run-jit-wasm.sh` exits 1 on a
clean checkout of `main`, so it cannot tell a new browser-JIT regression from
the eleven it already reports. Nothing in `.github/`, `CMakeLists.txt` or the
Justfile runs it, and the baseline was last rewritten on 2026-10-30.

Filed 2026-11-23, found while checking turmeric-lang/mir#21
([c2mir-wasm-vararg-buffer-allocas-per-call](../archive/c2mir-wasm-vararg-buffer-allocas-per-call.md))
against the corpus.

## Repro (measured, emcc 6.0.10, node 22, `main` at e9372656)

```sh
cmake -S . -B build-wasm -DTUR_WASM=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build-wasm -j --target tur_wasm_jit
bash tests/run-jit-wasm.sh     # 3309 fixtures; needs sharding to fit 12 minutes
```

`2602 compiled, 191 interpreted, 227 failed, 249 skipped`, and "worse than the
baseline" for these, each with baseline `compiled`:

| Fixture | Outcome now |
| --- | --- |
| `await-in-generic-capturing-lambda-struct` | the module threw: unreachable |
| `await-through-fn-value` | the module threw: unreachable |
| `cps-effectful-callback-wide-aggregate` | the module threw: unreachable |
| `cps-effectful-capturing-callback-narrow-result` | the module threw: unreachable |
| `cps-effectful-capturing-callback-ptr-void-arg` | the module threw: unreachable |
| `generic-hof-unannotated-fat-param-mixed` | the module threw: unreachable |
| `scheduler-spawn-detached-frees-done` | the module threw: unreachable |
| `cps-multishot-cell-per-run` | the module threw: memory access out of bounds |
| `cps-multishot-cell-per-run-narrow-and-struct` | stdout mismatch on the compiled path |
| `cps-set-value-performs` | stdout mismatch on the compiled path |
| `stdlib-c-fn-callbacks` | interpreted path: the program starts threads (`tur_timer_wheel_new`) |

All eleven give the same outcome with the vendored MIR before and after mir#21,
so that change did not cause them.

## Root cause

Not diagnosed per fixture. Some of these fixtures may be newer than the
baseline, in which case they were never `compiled` and only lack a row; the
two `stdout mismatch on the compiled path` rows are wrong output from the
engine and deserve a look first. The structural cause is that no job runs the
harness, so nothing notices when a row goes stale.

## Fix directions

- Triage the eleven: add baseline rows for the ones the page cannot run
  (threads), file the wrong-output and trap cases.
- Run `tests/run-jit-wasm.sh` in CI, sharded (a full run took about 30 minutes
  on 4 cores here), in the job that already builds `tur_wasm_jit`.
