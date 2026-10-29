# Every `defmacro*` fails beside a sibling spice with a `^used` defn

**Status: RESOLVED 2026-10-28** (see [Resolution](#resolution)).

**Severity: high (blocks a feature).** In a workspace where any search-path
module holds a `^used` defn, no `defmacro*` compiles: its definition, the
macro env's stdlib preload and every expansion fail with
`<eval>:0:0: error: module '<that module>' not found`, followed by a cascade
of unrelated stdlib errors. turmeric-spices is such a workspace -- the
`frame` spice's `frame/interop` has `^used` defns -- so nothing there could
use `defmacro*`. Found 2026-10-28 starting follow-up A of
[xml-html-followups-plan](xml-html-followups-plan.md).

## Repro

```sh
mkdir -p /tmp/r/lib/app /tmp/r/main
cat > /tmp/r/lib/app/a.tur <<'EOF'
(defmodule app/a
  (defn ^used __helper [x : int] : int
    ```c
    return x + 1;
    ```))
EOF
cat > /tmp/r/main/main.tur <<'EOF'
(defmodule m
  (defmacro* slen [s] (int->syntax (cstr-len (syntax->str s))))
  (defn main [] : int (+ 37 (slen "hello"))))
EOF
tur build /tmp/r/main/main.tur -I /tmp/r/lib -o /tmp/r/bin
# error: module 'app/a' not found
```

## Root cause

`cmd_build_once` (and `compile_link`) scan the search path for `^used`
modules (used-attr-whole-program) and publish them in a process-global
`UsedModulesCtx` for the duration of `compile_to_c`.
`elaborate_program_session` force-loads that list after its main pass.

The macro-time interpreter elaborates code *during* that compile -- its
stdlib preload, each `defmacro*` definition, `:for-macros` imports -- through
`turi_eval_impl`, which calls `elaborate_program_session` too. That nested
elaboration saw the driver's list and tried to load the modules on it, but
with the interpreter env's search path, which does not hold them.

## Resolution

`turi_eval_impl` (and its turn-replay path) hide the list while their own
elaboration runs (`src/turi/eval.c`): the force-load list belongs to the
outer compile alone. Regression: `build-file-macro-env-ignores-used-force-loads`
in `tests/run-build-project.sh`.
