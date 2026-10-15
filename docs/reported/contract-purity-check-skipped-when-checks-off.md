# TUR-E0375 (impure contract predicate) only fires where the check is emitted

**Severity:** low-medium (a static rejection that depends on the compiler's
build type, so the shipped Release `tur` accepts programs a Debug `tur`
rejects). Found 2026-10-15 running `tests/run-jit.sh` against a Release `tur`
during the performance audit's R-2 fix.

## Repro

```turmeric
(defn tick [] #fx{} : int
  ```c
  static int64_t n = 0;
  return n++;
  ```)

(defn f [n : int] : int
  :pre (>= (tick) 0)
  n)

(defn main [] : int
  (println (f 1))
  0)
```

`tur` v0.65.1 at `71b7abd68`, Ubuntu 24.04, gcc 13.3:

| compiler | `tur check repro.tur` |
| --- | --- |
| Debug `tur` | `7:1: error [TUR-E0375]: contract predicate has side effects; predicates must be pure`, exit 1 |
| Debug `tur --no-contracts` | no diagnostic, exit 0 |
| Release `tur` | no diagnostic, exit 0; `tur build` succeeds and the program prints `1` |

`tests/fixtures/errors/refine-impure-accessor-contract-positions` shows the
same thing, position by position. It expects six TUR-E0375s, at lines 31, 34,
38, 43, 47 and 51. A Debug `tur` gives all six. With `--no-contracts`, or from
a Release `tur`, only the `:invariant` one (`43:41`) is left. These five go
quiet:

- the parameter refinement;
- `:pre`;
- the return refinement;
- an impure call beside a `#reads` measure;
- an unannotated wrapper around one.

## Root cause

The purity check, `rt_diag_impure_pred` (`src/compiler/elab_fns.c:414`), is
called only from code that is building the runtime check:

- `rt_inject_param_checks` (`elab_fns.c:546`);
- `rt_wrap_return_check` (`elab_fns.c:615`);
- the `defn` path's `:pre` injection (`elab_fns.c:12042`).

Every one of those runs only when checks are emitted:

- **The `defn` path:** CT1 computes `should_check` under `#ifdef NDEBUG`
  (`elab_fns.c:11923-11930`).
- **The other sites** test `rt_contracts_emitted()` (`elab_fns.c:671`):
  - the return-contract wrap (`elab_fns.c:11323`);
  - a `fn`'s parameter checks (`elab_fns.c:13893`);
  - a typeclass method's parameter and result checks (`elab_typeclasses.c:5355`,
    `:5430`, `:5455`).

When checks are dropped (a Release `tur` without `--keep-contracts`, or
`--no-contracts` on any build), the predicate is never elaborated, so it is
never classified.

That contradicts the check's own rationale (`elab_fns.c:398-406`). Contracts
are compiled in or not depending on the build, so a predicate with side
effects makes behaviour depend on that choice: "That is a bug in the contract,
not in any of those flags."

The loop-invariant path already does the right thing. `elab_loop_invariant_pred`
classifies the predicate before the gate is consulted (`elab_forms.c:4708-4711`;
the gate is at `:4738`), which is why the `:invariant` error survives.

## How it surfaced, and what is not a bug

`run-jit.sh` with a Release `tur` failed 20 fixtures, and the same 20 failed on
`main`. This E0375 fixture is one of them.

The other 19 are fixtures that pin a runtime contract or refinement panic:
`contract-*-names-its-*`, `refine-*`, `loop-invariant-*-panics`,
`saffron-*refinement-violated`. A Release `tur` drops those checks on purpose,
unless `--keep-contracts` is given (`rt_contracts_emitted`,
`elab_fns.c:671-676`). Those fixtures assume a Debug `tur`, as `tests/run.sh`
does. `loop-invariant-body-panics` times out rather than failing, because its
loop only ends when the invariant check panics.

The engine is not the variable. The same 20 fixtures pass under `run-jit.sh`
with a Debug `tur`, and all 20 fail under `tests/run.sh` (the cc path) with a
Release `tur`.

## Fix directions

1. **Classify every contract predicate whether or not its check is emitted.**
   Follow the `:invariant` path: elaborate the predicate and call
   `rt_diag_impure_pred` ahead of the `should_check` / `rt_contracts_emitted()`
   gate, at each site above, and keep the gate for the injection only. When
   checks are off this adds one elaboration per predicate and emits nothing.
2. **Pin it with a test:** a negative fixture that runs the
   `refine-impure-accessor-contract-positions` program under `--no-contracts`
   and still expects all six TUR-E0375s. That catches it on every build, Debug
   included.
3. **Optional, for the harnesses:** `run.sh` and `run-jit.sh` could say when
   `TUR` is a Release build. Today the 19 elided-contract fixtures read as
   failures with no hint of the cause.
