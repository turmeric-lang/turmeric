# Struct Inspection -- Implementation Plan (SI0--SI4)

> **Status:** SI0 complete. SI1 complete. SI2 complete. SI3 complete. SI4 complete (framework).
>
> **Prerequisites:** Phase 15 (typeclasses: `Show`, `Display`, `Debug`), Phase
> R0 (`Display`/`Debug` typeclass definitions), Phase B1 (Clone infrastructure).
> No effect-row interaction is required.
>
> **Last updated:** 2026-05-22 (SI4 framework complete: turi_eval_typed, turi_try_show, REPL + WASM updated)

---

## Motivation

Today, making a struct "inspectable" requires writing boilerplate by hand for
every type:

```turmeric
(defstruct Point [x : int y : int])

;; Must be written manually today:
(definstance Show [Point]
  (show [p] : cstr
    ```c
    char *buf = malloc(64);
    snprintf(buf, 64, "Point { x = %ld, y = %ld }", p.x, p.y);
    return buf;
    ```))
```

There is no way to derive this automatically, no way to query a struct's field
names or count at the language level, and no macro that generates the boilerplate
for you. This makes debugging and REPL use friction-heavy.

The goal of this plan is to close that gap in four phases:

1. **SI0** -- Specification and fixtures.
2. **SI1** -- Real `show`/`display`/`debug` for primitive types (replace
   placeholders).
3. **SI2** -- `derive-show` macro that generates a `Show` instance for a struct
   given its field names.
4. **SI3** -- `derive-debug` and `derive-display` macros; `Show` instances for
   the standard collection types (`List`, `Option`, `Result`, `Pair`, `Vec`).
4. **SI4** -- REPL / web REPL integration: auto-apply `show` to top-level
   expression results.

---

## Current State

### Typeclasses defined (`stdlib/typeclass.tur`)

| Typeclass | Method | Instances today |
|-----------|--------|-----------------|
| `Show`    | `(show [x] :cstr)` | int, cstr, int8--32, uint8--32, uint64, float32 (all return placeholder strings) |
| `Display` | `(display [x] :cstr)` | int (placeholder), ptr\<void\> result (placeholder) |
| `Debug`   | `(debug [x] :cstr)` | int (placeholder), ptr\<void\> result (placeholder) |

### What is missing

- All primitive `Show` instances return literal placeholder strings (e.g.
  `"<int>"`) instead of the actual value.
- No `Show`, `Display`, or `Debug` instances exist for any struct type in stdlib.
- No macro exists to generate these instances automatically.
- No `Show` instances for `List`, `Option`, `Result`, `Pair`, or `Vec`.
- The REPL prints raw integer values; it does not call `show` on results.

---

## Architecture Overview

```
stdlib/typeclass.tur    -- fix primitive Show/Display/Debug instances (SI1)
stdlib/macros.tur       -- derive-show, derive-debug, derive-display macros (SI2--SI3)
stdlib/list.tur         -- Show [List] instance (SI3)
stdlib/option.tur       -- Show [Option] instance (SI3)
stdlib/result.tur       -- Show [Result] instance, fix Display/Debug (SI3)
stdlib/pair.tur         -- Show [Pair] instance (SI3)
stdlib/vec.tur          -- Show [Vec] instance (SI3)
src/wasm_glue.c         -- turi_show_result for REPL auto-show (SI4)
web/main.js             -- call show on top-level results (SI4)
tests/fixtures/         -- SI0--SI3 test fixtures
```

---

## Phase SI0 -- Specification and fixtures

**Goal:** Define expected output with runnable examples before touching any
implementation. All fixtures in this phase are expected to *fail* until SI1--SI3
are complete.

### Fixtures

- [x] `tests/fixtures/show-int/` -- `(show 42)` returns `"42"`.
- [x] `tests/fixtures/show-cstr/` -- `(show "hello")` returns `"hello"`.
- [x] `tests/fixtures/show-float/` -- `(show 3.14)` returns `"3.14"` (`%g` -- decided in Open Questions #2).
- [x] `tests/fixtures/show-pair/` -- `(show (pair-new 1 2))` returns `"(1, 2)"`.
- [x] `tests/fixtures/show-option/` -- `(show (some 99))` returns `"some(99)"` and `(show (none))` returns `"none"`.
- [x] `tests/fixtures/show-list/` -- `(show (cons 1 (nil-value)))` returns `"[1]"`.
- [x] `tests/fixtures/derive-show-struct/` -- a user-defined struct uses `derive-show` and `show` returns `"Point { x = 3, y = 4 }"`.
- [x] `tests/fixtures/derive-show-nested/` -- a struct whose fields are themselves `Show`-able; `show` recurses correctly.

Note: fixtures live flat under `tests/fixtures/` (not in a `show/` subdirectory) to match the existing test runner convention.

### String format conventions (decided here, implemented in SI1+)

| Type | `show` output | `debug` output |
|------|--------------|----------------|
| `int` | `"42"`, `"-7"` | `"int(42)"` |
| `float32` | `"3.14"`, `"1e-05"` (`%g`) | `"float32(3.14)"` |
| `cstr` | `"hello"` (no surrounding quotes) | `"cstr(\"hello\")"` |
| `bool` | `"true"` / `"false"` | `"bool(true)"` |
| Struct | `"TypeName { field1 = v1, field2 = v2 }"` | `"(TypeName (field1 v1) (field2 v2))"` |
| Pair | `"(a, b)"` | `"(Pair (first a) (second b))"` |
| Option some | `"some(v)"` | `"Option::some(v)"` |
| Option none | `"none"` | `"Option::none"` |
| List | `"[a, b, c]"` (`"[]"` for empty) | `"(List a b c)"` |
| Vec | `"[a, b, c]"` (same as List) | `"(Vec a b c)"` |
| Result ok | `"ok(v)"` | `"Result::ok(v)"` |
| Result err | `"err(e)"` | `"Result::err(e)"` |

**Exit criterion:** All fixtures exist and are documented; all currently fail
with either a type error or placeholder output.

---

## Phase SI1 -- Real primitive Show/Display/Debug instances

**Goal:** Replace the placeholder `"<int>"`, `"<float32>"`, etc. strings with
actual formatted values. This is a prerequisite for all derived instances.

### Changes to `stdlib/typeclass.tur`

Replace each placeholder `Show` instance with a C inline block that formats the
value:

```turmeric
(definstance Show [int]
  (show [x] : cstr
    ```c
    char *buf = malloc(32);
    snprintf(buf, 32, "%ld", (long)x);
    return buf;
    ```))

(definstance Show [float32]
  (show [x] : cstr
    ```c
    char *buf = malloc(32);
    snprintf(buf, 32, "%f", (float)x);
    return buf;
    ```))

(definstance Show [bool]
  (show [x] : cstr
    (if x "true" "false")))
```

Apply the same pattern to `int8`, `int16`, `int32`, `uint8`, `uint16`, `uint32`,
`uint64` with the appropriate `printf` format specifier.

Fix `Display [int]` and `Debug [int]` in the same way (they currently both
return `"<int>"`).

### Memory note

These instances `malloc` a small buffer. Callers that use `show` for logging and
discard the result should be aware these are heap-allocated. A future `str.tur`
arena allocator could eliminate the overhead, but that is out of scope here.

### Tasks

- [x] Replace `Show [int]` placeholder with formatted C inline.
- [x] Replace `Show [float32]` placeholder.
- [x] Replace `Show [int8]`, `Show [int16]`, `Show [int32]`.
- [x] Replace `Show [uint8]`, `Show [uint16]`, `Show [uint32]`, `Show [uint64]`.
- [x] Add `Show [bool]`.
- [x] Fix `Display [int]` and `Debug [int]` to call the same formatting logic.
- [x] Fix `Display [ptr<void>]` and `Debug [ptr<void>]` (result) to emit
  `"ok(...)"` / `"err(...)"` with the inner value's representation.
- [x] All `show-int.tur`, `show-cstr.tur`, `show-float.tur` fixtures pass.
- [x] New fixtures: `show-bool/`, `display-int/`, `debug-int/`.

**Exit criterion:** Every primitive `Show` instance returns the actual value as
a string. Existing tests continue to pass.

---

## Phase SI2 -- `derive-show` macro for structs

**Goal:** Provide a macro that generates a `Show` instance for a struct from an
explicit list of field accessors. No compiler reflection is required -- the macro
is purely syntactic.

### Design

Because Turmeric does not expose struct field metadata at runtime, `derive-show`
takes the struct name followed by field descriptors. Each descriptor is either:

- A bare symbol `x` -- label is `"x"`, accessor is `(.x s)`.
- A pair `[label accessor]` -- uses the given label string and accessor
  expression, for aliasing or non-standard accessors.

```turmeric
(defstruct Point :copy [x : int y : int])

;;; derive-show -- generate a Show instance for a struct.
;;;
;;; Parameters:
;;;   TypeName -- the struct type name (symbol)
;;;   fields   -- bare symbols or [label accessor] pairs
;;;
;;; Returns:
;;;   A definstance Show form for TypeName.
;;;
;;; Example:
;;;   (derive-show Point x y)
;;;   ; (show (make-struct Point 3 4)) => "Point { x = 3, y = 4 }"
;;;
;;;   (derive-show MyStruct name [display-name .internal-label] count)
;;;   ; uses (.name s), (.internal-label s) as "display-name", (.count s)
;;;
;;; Since: SI2
(defmacro derive-show [TypeName & fields]
  ...)
```

The macro expands to a `definstance Show [TypeName]` whose body:

1. Calls `show` on each field value (requires the field type to itself have a
   `Show` instance).
2. Concatenates the results with `str-concat` (from `stdlib/str.tur`).

### Expansion example

```turmeric
;; (derive-show Point x y)
;; expands to:
(definstance Show [Point]
  (show [p] : cstr
    (str-concat "Point { x = " (show (.x p))
      (str-concat ", y = " (show (.y p))
        " }"))))

;; (derive-show MyStruct name [display-name .internal-label])
;; expands to:
(definstance Show [MyStruct]
  (show [p] : cstr
    (str-concat "MyStruct { name = " (show (.name p))
      (str-concat ", display-name = " (show (.internal-label p))
        " }"))))
```

### Dependency: `str-concat`

`stdlib/str.tur` must expose a `str-concat` function (two `:cstr` arguments,
returns a new heap-allocated `:cstr`). If it does not already exist, add it as
part of this phase.

### Tasks

- [x] Verify or add `str-concat` to `stdlib/str.tur` -- already present since Phase B1.
- [x] Implement `derive-show` macro in `stdlib/macros.tur`.
- [x] Add docstring to `derive-show`.
- [x] `derive-show-struct.tur` fixture updated to use `(derive-show Point x y)`.
- [x] `derive-show-nested.tur` fixture updated to use `(derive-show Triple a b c)`.
- [x] Bug fix: CT builtins `vec` and `list` receive raw form nodes; changed `'__p` to `__p` and `show` to `.show` in macro bodies so unbound symbols resolve correctly and method dispatch uses the `.` prefix.
- [ ] Run `just docs` to confirm docstring appears in generated HTML.

Note: three new CT built-ins were added to `src/compiler/elab_macros.c` to support
the macro: `symbol-name` (symbol -> string literal), `dot-sym` (symbol -> .symbol),
`str-append` (compile-time string concatenation), and `vec?` (vector predicate).

**Exit criterion:** `derive-show` generates a correct `Show` instance; both
struct fixtures pass.

---

## Phase SI3 -- Show instances for stdlib types

**Goal:** Add `Show` instances for `List`, `Option`, `Result`, `Pair`, and `Vec`
using the conventions from SI0. Use `derive-show` where applicable; write
manual C inline blocks where the recursive/pointer-chasing logic requires it.

### `Show [Pair]` (`stdlib/pair.tur`)

```turmeric
;;; Show [Pair] -- show a Pair as "(a, b)".
(definstance Show [Pair]
  (show [p] : cstr
    (str-concat "(" (show (pair-first p))
      (str-concat ", " (show (pair-second p))
        ")"))))
```

### `Show [Option]` (`stdlib/option.tur`)

```turmeric
;;; Show [Option] -- show an Option value.
(definstance Show [Option]
  (show [opt] : cstr
    (if (none? opt)
      "none"
      (str-concat "some(" (show (unwrap opt)) ")"))))
```

### `Show [List]` (`stdlib/list.tur`)

A manual recursive C inline block (or a Turmeric loop) that iterates the linked
list, calling `show` on each element, and builds `"[a, b, c]"`.

### `Show [Vec]` (`stdlib/vec.tur`)

Similar to `Show [List]` but indexes by integer.

### `Show [Result]` / fix `Display`+`Debug` (`stdlib/result.tur`)

Fix `Display [ptr<void>]` and `Debug [ptr<void>]` in `typeclass.tur` to call
`show` on the inner ok/err value (requires the value type to be `Show`-able).
Add a `Show [ptr<void>]` instance following the same pattern.

### Tasks

- [x] `Show [Pair]` in `stdlib/pair.tur`; `show-pair.tur` passes.
- [x] `Show [Option]` in `stdlib/option.tur`; `show-option.tur` passes.
- [x] `Show [List]` in `stdlib/list.tur`; `show-list.tur` passes.
- [x] `Show [Vec]` in `stdlib/vec.tur`.
- [x] Fix `Display`/`Debug`/add `Show` for `Result` in `stdlib/typeclass.tur`.
- [x] Add `derive-debug` macro (same shape as `derive-show`, uses `debug`).
- [x] Add `derive-display` macro (same shape, uses `display`).
- [x] Run `just test` -- all existing tests pass (20/20 CTest).

**Exit criterion:** All `show-*.tur` fixtures pass; `derive-debug` and
`derive-display` macros exist and are documented.

---

## Phase SI4 -- REPL and web REPL auto-show

**Goal:** When a top-level expression is evaluated in the REPL or web REPL, if
the result type has a `Show` instance, print the string returned by `show`
instead of a raw integer.

### Changes to `src/wasm_glue.c`

Export a new function `turi_show_result(int64_t val, const char *type_tag)`
that dispatches to the correct `show` implementation based on `type_tag` (a
compiler-emitted string identifying the result type). For types without a `Show`
instance, fall back to `"<value>"`.

### Changes to `web/main.js`

After evaluating a top-level form, call `turi_show_result` on the return value
before printing to the output panel.

### Changes to `src/repl.c` (native REPL)

After evaluating a top-level expression, call the `show` method if available,
and print the result.

### Limitations

- The dispatch in `turi_show_result` is necessarily a switch/if-chain over
  known type tags. Truly generic dispatch (through the typeclass vtable) requires
  runtime type information that Turmeric does not yet carry. This is a known
  limitation; a follow-up plan can address it when runtime type tags are added.

### Tasks

- [x] Define `type_tag` conventions: `turi_eval_typed` in `src/turi/eval.h`/`eval.c` extracts the elaborated type tag of the last expression (`"int"`, `"bool"`, `"Pair"`, etc.).
- [x] Implement `turi_try_show` in `src/turi/eval.c`: finds the Show typeclass instance for a `TURI_STRUCT` value from `env->last_tc_env`, creates a closure from the method impl, calls it via `turi_call`, and returns a heap-allocated string.
- [x] Update native REPL (`src/turi/repl.c`): calls `turi_eval_typed`, then tries `turi_try_show`; falls back to `repl_print_value` if no Show instance registered.
- [x] Update WASM glue (`src/web/wasm_glue.c`): calls `turi_eval_typed`, then tries `turi_try_show`; falls back to `turi_value_repr` if no Show instance.
- [x] Update native REPL and web REPL: improved `turi_value_repr` in `src/turi/eval.c` to show `"TypeName { field = val, ... }"` for TURI_STRUCT values with a StructDef; both REPLs benefit automatically since both call `turi_value_repr`.
- [x] Fixed Show [Pair] inline C in `stdlib/pair.tur` to use direct field access in snprintf args (interpreter-compatible).
- [ ] Manual smoke test: `(pair-new 1 2)` shows `"(1, 2)"` -- blocked on type-tag tracking for heap-pointer structs (`pair-new` returns `:int`, not `:Pair`).
- [x] `(make-struct Pair 1 2)` shows `"(1, 2)"` in the REPL via `turi_try_show` dispatch.

**Exit criterion achieved (partial):** `(make-struct Pair 1 2)` shows `"(1, 2)"` in both native and WASM REPLs. The `pair-new` case remains blocked on heap-pointer type tracking (static type is `:int`). User-defined structs with custom Show instances now display correctly.

**Exit criterion blocked:** `pair-new` returns `:int` at the type level, so typeclass dispatch on its result goes to `Show [int]`, not `Show [Pair]`.

---

## Open Questions

1. **Memory ownership of `show` return values** -- **Decision: per-evaluation
   scratch arena (option C).** A scratch arena is allocated at the start of each
   top-level evaluation and reset (not freed) after the result is printed. All
   `show` implementations allocate into this arena via an `arena_alloc` helper
   rather than calling `malloc` directly. No per-call `free` is needed; nested
   `show` calls (e.g. `Show [List]` calling `show` on each element) compose
   naturally. The arena infrastructure must land before or alongside SI1. Until
   the arena exists, SI1 primitives may use `malloc` as a temporary measure
   with a `// TODO: use arena` comment; the switch-over is a single mechanical
   replacement once the arena is available.

2. **Format of `float32`** -- **Decision: `%g`.** Drops trailing zeros and
   switches to scientific notation only when the exponent falls outside
   `[-4, 6]`. `3.14` prints as `"3.14"`, `1.0` as `"1"`, `0.00001` as
   `"1e-05"`. More readable than `%f` for typical values; the type system
   already distinguishes floats from ints so the lack of a `.0` suffix is not
   ambiguous.

3. **`derive-show` field accessor syntax** -- **Decision: bare names with optional
   `[]` override.** Each argument to `derive-show` is either a bare symbol or a
   `[label accessor]` pair:
   - Bare symbol `x` expands to field label `"x"` and accessor `(.x s)`.
   - Pair `[display-name .internal-field]` uses the given label string and
     accessor expression, allowing aliasing or non-standard accessors.
   Example:
   ```turmeric
   (derive-show Point x y)
   (derive-show MyStruct name [display-name .internal-label] count)
   ```
   The macro detects whether each argument is a list or a symbol at expansion
   time and generates the appropriate accessor form.

4. **`Debug` vs `Show` distinction** -- **Decision: different output (option B).**
   `show` produces clean, human-readable output. `debug` adds explicit type
   annotation for diagnostic use:

   | Expression | `show` | `debug` |
   |---|---|---|
   | `(make-struct Point 3 4)` | `"Point { x = 3, y = 4 }"` | `"(Point (x 3) (y 4))"` |
   | `(some 99)` | `"some(99)"` | `"Option::some(99)"` |
   | `(none)` | `"none"` | `"Option::none"` |
   | `42` | `"42"` | `"int(42)"` |
   | `"hello"` | `"hello"` | `"cstr(\"hello\")"` |

   `derive-show` generates a `Show` instance with the `"TypeName { k = v }"` format.
   `derive-debug` generates a `Debug` instance with the `"(TypeName (k v))"` format.
   Both macros share the same field descriptor syntax (bare symbols + `[]` overrides).
