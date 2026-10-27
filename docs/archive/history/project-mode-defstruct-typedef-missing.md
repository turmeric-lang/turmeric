# Project-mode codegen: `defstruct` typedef is missing from generated header/impl

**Severity:** hard error -- any project-mode spice with a `defstruct` fails to
link in cc. Single-file `emit-c` is fine. The defect is in the per-module
emit path used by `tur build <project-dir>`, not the codegen of the struct
fields themselves.

## Repro

Minimal project-mode tree:

```
/tmp/structtest/
├── build.tur
└── src/foo/box.tur
```

`build.tur`:
```turmeric
(defpackage tur-structtest
  :name "tur-structtest"
  :version "0.1.0"
  :description "min struct repro"
  :license "MIT"
  :exports #{ "foo/box" ["Box" "make-box"] })
```

`src/foo/box.tur`:
```turmeric
(defmodule foo/box
  (export Box make-box)
  (defstruct Box [x : float y : float])
  (defn make-box [a : float b : float] : int
    ```c
    typedef struct { double x; double y; } Box_;
    Box_ *p = malloc(sizeof(*p));
    p->x = a; p->y = b;
    return (int64_t)(intptr_t)p;
    ```))
```

Running `tur build .` from `/tmp/structtest` fails:

```
foo__box.c:6:1: error: unknown type name 'Box'
    6 | Box Box_2;
      | ^
1 error generated.
tur: cc invocation failed (status 256)
```

The generated `foo__box.c` contains a bare `Box Box_2;` variable declaration
referring to a type that was never declared. The generated `foo__box.h`
contains no `Box` reference at all.

## Expected behavior

Single-file `tur emit-c src/foo/box.tur` correctly emits at file scope:

```c
typedef struct Box {
    double x;
    double y;
} Box;
```

and **does not** emit a `Box Box_N;` global variable declaration. The
project-mode emit should mirror this: emit the typedef (in the header so
importers see it, or at least in the implementation file) and skip the
spurious variable declaration.

## Observed behavior

In project-mode:

- `emit_header` (src/compiler/emit_module.c:5248) skips struct typedefs.
  Its forward-declaration loop (around line 5330) handles
  `EX_DEF && !e->as.def_.struct_def` -- the negation explicitly excludes
  struct defs. No earlier pass emits them either.
- `emit_implementation` (src/compiler/emit_module.c:5516) treats every
  `EX_DEF` as a value binding. At line 5792-5806 it unconditionally emits
  `%s %s;` (type + name) for any `EX_DEF`, including struct defs, producing
  the bogus `Box Box_2;` (or `ADSRParams ADSRParams_8;`) variable
  declaration.

The single-file path (`emit_program`) has a dedicated Pass 0 at
src/compiler/emit_module.c:1704-1755 that emits
`typedef struct Name { fields... } Name;` plus drop_glue/walk_glue if any
field is RC/weak/ref. That pass has no project-mode equivalent.

## Affected real-world code

`spices/signal/src/signal/envelope.tur:14` defines
`(defstruct ADSRParams [attack :float decay :float sustain :float release :float])`
and exports it. Full `tur build .` from `spices/signal` fails at the cc
step in `signal__envelope.c:33` with `unknown type name 'ADSRParams'`. The
test `tests/signal/test_core.tur` only imports `signal/core` and therefore
avoids envelope.tur in its include path; it builds fine. The bug only
fires when the whole spice is compiled together.

## Proposed fix

In `emit_header` (project-mode header), add a struct-typedef emission pass
**before** the function forward-declarations loop (around line 5301).
Mirror the loop body at lines 1704-1755 of `emit_program`, but only emit
the typedef (not drop_glue/walk_glue, which are `static` and must remain
file-local). Skip `def->is_opaque`.

In `emit_implementation`, modify the `EX_DEF` arm at line 5792 to early-out
when `e->as.def_.struct_def` is non-NULL -- the struct typedef is in the
header, and the binding does not represent a runtime value to declare. The
drop_glue/walk_glue functions should still be emitted in the implementation
file (since they're `static` and called from local code).

Validation:

1. The minimal repro above should compile with `tur build .` and link
   cleanly.
2. `tur build .` in `spices/signal` (with the test-spice on the spice
   search path) should succeed all the way through `signal__envelope.c`.
3. A new fixture under `tests/fixtures/project-mode-defstruct/` should
   exercise the path end-to-end.
4. Existing single-file fixtures must not regress.

## Resolution

The typedef gap is **fixed**. `emit_header`
(src/compiler/emit_module.c:5375-5428) now emits `typedef struct Name {
fields... } Name;` into the project-mode header for every non-opaque struct
def, and the `emit_implementation` `EX_DEF` arm
(src/compiler/emit_module.c:5936-5941) early-outs on struct defs so the
spurious `Name Name_N;` variable declaration is no longer emitted.

Validation status:

1. Minimal repro -- covered by `build-project-defstruct-typedef` in
   `tests/run-build-project.sh` (asserts `tur build` compiles, links, and the
   binary returns 21). Passing.
2. `spices/signal` (`ADSRParams`, float-only fields) -- `tur build .` succeeds
   end-to-end through `signal__envelope.c`. Verified.
3. End-to-end coverage lives in `tests/run-build-project.sh` rather than a
   `tests/fixtures/project-mode-defstruct/` directory, matching the convention
   that project-mode (`build.tur`) cases are exercised by the dedicated
   build-project runner, not the single-file `emit-c` fixture suite.
4. Full `tests/run.sh` suite passes with no regressions.

### Follow-up split out

RC support in project mode is still entirely missing -- the RC/frame runtime
preamble (`RcControlBlock`, `rc_cb_alloc`, `tur_frame`, ...) is not emitted in
separate compilation, and per-struct `drop_glue_<Name>`/`walk_glue_<Name>` are
never emitted in the implementation file (despite the `emit_header` comment
claiming they "stay `static` in the implementation file"). This is broader than
the typedef fix here and is tracked in
[project-mode-rc-runtime-preamble-missing.md](project-mode-rc-runtime-preamble-missing.md).
