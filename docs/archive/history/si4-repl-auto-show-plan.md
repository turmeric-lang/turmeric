# SI4 -- REPL Auto-Show Implementation Plan

> **Status:** SI4-A complete (2026-05-22). SI4-B complete (free). SI4-C
> complete (2026-05-22). OQ2 complete (2026-05-22).
>
> **Prerequisites:** SI0--SI3 complete (all `Show` instances exist for stdlib
> types).
>
> **Last updated:** 2026-05-22

---

## Motivation

After SI0--SI3, evaluating `(pair-new 1 2)` in the REPL still prints a raw
integer (the heap pointer) rather than `"(1, 2)"`. The goal is to make
top-level REPL results display in human-readable form using the `Show` typeclass
conventions established in SI1--SI3.

---

## Architecture Reality Check

The original SI4 plan assumed type tags would need to be added to the compiler.
After reading the runtime source, the picture is better than expected in one area
and unchanged in another:

- **`TuriValue` already carries runtime type tags** (`TURI_INT`, `TURI_BOOL`,
  `TURI_FLOAT`, `TURI_CSTR`, `TURI_STRUCT`, etc.) -- no compiler changes needed
  for primitives and structs.
- **`TuriStruct` carries a `StructDef *def` pointer** that exposes field names
  (`def->fields[i].name`) and field count.  No structural changes to `TuriStruct`
  were needed.
- **Heap-allocated stdlib types** (`list`/`Cons`, `option`, `vec`, `pair`) are
  all returned as raw `int64_t` with tag `TURI_INT`. The runtime cannot
  distinguish a plain `42` from a list pointer without additional type
  information from the compiler. This remains the hard problem for SI4-C.

---

## Sub-tasks

### SI4-A -- Struct auto-show in the REPL

**Effort:** ~1 day (complete)
**Compiler changes:** None
**Files changed:** `src/turi/eval.c`, `src/turi/eval.h`, `src/turi/env.h`,
`src/turi/repl.c`, `src/web/wasm_glue.c`, `stdlib/pair.tur`

**Goal:** User-defined structs with Show instances (including `derive-show`
output) display via their Show typeclass method in the REPL; structs without
a Show instance fall back to `"TypeName { field = val, ... }"`.

#### Approach taken (differs from original plan)

Rather than adding `field_names` to `TuriStruct`, we used two mechanisms:

1. **`turi_value_repr` for `TURI_STRUCT`** already accesses `s->def->fields[i].name`
   (the StructDef is already reachable as `TuriStruct->def`, populated by
   `EX_MAKE_STRUCT` evaluation). This gives the `"TypeName { f = v }"` fallback
   format at no extra cost.

2. **`turi_eval_typed` + `turi_try_show`** call the actual registered `Show`
   typeclass instance for `TURI_STRUCT` values:
   - `turi_eval_typed` passes `out_tc_env` to `elaborate_program` and saves the
     populated `TypeClassEnv *` in `env->last_tc_env` (an opaque `void *` field
     added to `TuriEnv`).
   - `turi_try_show` walks `env->last_tc_env->instances`, finds the `Show`
     instance whose `type_args[0].as.struct_.def == val.as_struct->def`, creates a
     `TuriClosure` from `method_impls[show_mi]`, and calls it via `turi_call`.
   - Both the native REPL (`repl.c`) and WASM glue (`wasm_glue.c`) now call
     `turi_eval_typed` → `turi_try_show` → fallback to `turi_value_repr`.

#### Task list

- [x] **`turi_value_repr` for `TURI_STRUCT`** updated to `"TypeName { f = v }"` using `s->def`.
- [x] **`turi_eval_typed`** added to `eval.h`/`eval.c`:
  - Accepts `char *out_type_tag, size_t tag_cap`.
  - Allocates a `TypeClassEnv` in the per-call eval arena, passes it to
    `elaborate_program` as `out_tc_env`, and stores the pointer in
    `env->last_tc_env`.
  - Fills `out_type_tag` from the elaborated type of the last new expression
    via `extract_type_tag()` helper.
  - `turi_eval` is now a thin wrapper calling `turi_eval_impl(env, src, NULL, 0)`.
- [x] **`turi_try_show`** added to `eval.h`/`eval.c`:
  - Walks `TypeClassEnv->typeclasses` to find `Show`.
  - Finds `show` method index in `TypeClass->methods`.
  - Walks `TypeClassEnv->instances` to find the `Show [StructType]` instance
    matching `val.as_struct->def`.
  - Creates a temporary `TuriClosure` from `method_impls[show_mi]` and calls
    it via `turi_call`.
  - Returns a heap-allocated string (`strdup` of result) or `NULL`.
- [x] **`void *last_tc_env`** added to `TuriEnv` (opaque pointer; `TypeClassEnv *`
  in eval.c).
- [x] **Native REPL** (`repl.c`) updated: calls `turi_eval_typed`, tries
  `turi_try_show`, falls back to `repl_print_value`.
- [x] **WASM glue** (`wasm_glue.c`) updated: calls `turi_eval_typed`, tries
  `turi_try_show`, falls back to `turi_value_repr`.
- [x] **`stdlib/pair.tur` Show [Pair]** rewritten to use direct field access in
  the `snprintf` arguments (`p.first`, `p.second`) instead of intermediate
  local variables, making it work under the interpreter's inline-C pattern
  executor.

#### Known limitation of `turi_try_show`

The Show method body must be interpretable by `try_exec_simple_inline_c`.
This works for:
- Simple `malloc` + `snprintf(buf, N, "fmt", (long long)param.field, ...)` patterns
- Patterns that directly reference struct fields as snprintf arguments

It does NOT work for inline C that uses intermediate local variables
(`int64_t fst = p.first; snprintf(buf, ..., fst)`) because the interpreter's
pattern matcher cannot track assignments to local C variables.  The fix is to
inline field accesses directly into snprintf arguments.

#### Smoke tests

```
tur> (defstruct Point :copy [x :int y :int])
tur> (derive-show Point x y)
tur> (make-struct Point 3 4)
=> Point { x = 3, y = 4 }           ;; via turi_try_show → derive-show Show instance

tur> (defstruct Color :copy [r :int g :int b :int])
tur> (definstance Show [Color] (show [c] :cstr ```c
...  char *buf = malloc(16); snprintf(buf, 16, "#%02x%02x%02x", (int)c.r, (int)c.g, (int)c.b); return buf;
...  ```))
tur> (make-struct Color 255 128 0)
=> #ff8000                            ;; custom Show instance used, not turi_value_repr

;; Via stdlib show-pair fixture style:
tur> (make-struct Pair 1 2)          ;; (with local Show [Pair] defined)
=> (1, 2)
```

---

### SI4-B -- Primitive auto-show (bool, float, cstr)

**Effort:** Already done (free)
**Compiler changes:** None

`turi_value_repr` already handles these correctly:
- `TURI_BOOL` → `"true"` / `"false"`
- `TURI_INT` → `"%lld"` (correct for plain integers)
- `TURI_FLOAT` → `"%g"` (matches `Show [float32]` convention)
- `TURI_CSTR` → `"\"%s\""` (quotes around the string, fine for REPL display)

No work needed here.

---

### SI4-C -- Heap-allocated stdlib types (list, option, vec, pair)

**Effort:** 2--3 weeks
**Compiler changes:** Yes -- significant
**Files:** `src/compiler/elab_toplevel.c`, `src/turi/eval.c`, `src/turi/eval.h`,
`src/web/wasm_glue.c`

**Goal:** `(cons 1 (cons 2 (nil-value)))` in the REPL prints `"[1, 2]"` instead
of a raw pointer integer.

**The core problem:** All heap-allocated stdlib types (`Cons`, `option`, `vec`,
`Pair`) return `int64_t` from their constructor functions, which the evaluator
stores as `TURI_INT`. Even with `turi_eval_typed`, the elaborated type of
`(pair-new 1 2)` is `TY_INT` (because `pair-new` is declared `(defn pair-new [a b] #{Unsafe} :int ...)`)
-- so the type tag `"int"` provides no hint that the value is actually a Pair
pointer.

#### Current state after SI4-A work

`turi_eval_typed` is now live and already collects the elaborated type from the
last top-level expression. The infrastructure is in place:

```c
char type_tag[64];
TuriValue result = turi_eval_typed(env, src, type_tag, sizeof(type_tag));
// type_tag is now e.g. "int", "bool", "Point", "ptr<void>"
```

For `:copy` structs created with `(make-struct ...)`, the type tag is the struct
name (e.g. `"Point"`), and `turi_try_show` dispatches correctly.

For `pair-new`, `cons`, `some`, `vec-new` etc., the type tag is `"int"` because
their Turmeric return annotations are `:int`. This is the remaining gap.

#### Option A -- Return-type annotations on stdlib heap constructors (recommended)

Change stdlib constructor functions to declare a more specific return type:

```turmeric
;; Before:
(defn pair-new [a b] #{Unsafe} : int ...)

;; After (using ptr<Pair> or a named opaque type alias):
(defn pair-new [a b] #{Unsafe} : ptr<Pair> ...)
```

If `ptr<Pair>` is not a first-class type in Turmeric's type system today, an
alternative is to introduce a lightweight **opaque newtype** mechanism:

```turmeric
(defopaque PairPtr :int)
(defn pair-new [a b] #{Unsafe} : PairPtr ...)
```

With this, `turi_eval_typed` would produce type tag `"PairPtr"` (or `"Pair"`),
and the `turi_show_result` dispatch table would interpret the `int64_t` as a
`struct { int64_t first; int64_t second; } *` and format it.

**Tasks for Option A:**

- [x] **Investigate `ptr<T>` support** -- determined not needed; went with
  `defopaque` instead (see below).

- [x] **Implement opaque newtypes** (`defopaque Name :base-type`) as a
  zero-overhead type alias that carries a distinct name through elaboration.
  Implementation: added `bool is_opaque` to `StructDef`, `type_c_name` returns
  "int64_t" for opaque structs, `elab_defopaque` in `elab_structs.c`, forward
  pre-pass in `elab_toplevel.c`, dispatch in `elab_call.c`.

- [x] **Update stdlib constructors** to use the new type annotation:
  - `pair-new` → returns `":PairPtr"` (defopaque PairPtr :int in pair.tur)
  - `cons` / `nil-value` → returns `":ConsPtr"` (defopaque ConsPtr :int in list.tur)
  - `some` / `none` → left as `:int` (Option support deferred)
  - `vec-new` → left as `:int` (Vec support deferred)

- [x] **Implement `turi_show_result(TuriValue val, const char *type_tag)` in
  `eval.c`** -- dispatches on "PairPtr"/"Pair" and "ConsPtr"/"Cons".

- [x] **Wire `turi_show_result` into the REPL** (`repl.c`) and WASM glue
  (`wasm_glue.c`) as a third-tier fallback after `turi_try_show` -- done.

- [x] **Smoke tests (verified in REPL):**
  - `(pair-new 5 10)` → `(5, 10)` ✓
  - `(cons 1 (cons 2 (nil-value)))` → `[1, 2]` ✓
  - `(+ 1 2)` → `3` (plain int, no regression) ✓
  - Deferred: `some`/`none`, `vec-new` (require defopaque ConsPtr/VecPtr)

#### Option B -- Tagged value struct in generated C (alternative)

Instead of passing type tags at the eval boundary, change the C ABI so all
values are wrapped in `{ int64_t value; const char *type_tag; }` rather than
bare `int64_t`. This is a larger breaking change to the code generator but
eliminates the need for a separate type-tag channel.

This is the more architecturally sound long-term approach but would require
updating all generated C code and all stdlib inline C blocks. Not recommended
as an immediate next step.

---

## Prior art: how other languages handle type tags

Understanding how other languages solve this problem informs which option to
choose for SI4-C.

### GHC Haskell -- dictionary passing (most relevant)

GHC never embeds a runtime type tag in a value. Instead, the typeclass
dictionary (a record of function pointers, one per method) is passed as an
implicit extra argument at every polymorphic call site. The compiler statically
resolves which `Show` dictionary to use before evaluation; the value itself
carries no type information.

```haskell
-- show x  (x :: Int)  compiles roughly to:
showDict_Int.show x
```

For GHCi's REPL, the type inferencer resolves the type of the top-level
expression *before* evaluation, selects the right dictionary, and threads it
in. The result value is still an untagged machine word at runtime.

**Implication for Turmeric:** SI4 Option A (pass the static type tag alongside
the `int64_t` result at the REPL boundary) is the GHC-equivalent approach.
The compiler already knows the static type; it just needs to surface it once
at the eval boundary rather than embed it in every value.

### OCaml -- low-bit tag + GC block headers

OCaml uses a hybrid:
- **Immediates** (int, bool, char): the LSB is `1`. Value = `(n << 1) | 1`.
  One bit of every integer is permanently borrowed for the tag.
- **Heap pointers**: LSB is `0`. The pointed-to block begins with a header
  word: `[ size (22 bits) | GC color (2 bits) | tag (8 bits) ]`.

Structural types (tuples, records, variants) use tag byte `0`; strings `252`;
floats `253`; closures `246`. The GC reads these tags to know how to trace.

**Cost:** 63-bit integers on 64-bit hardware; one header word per heap object.

### Lua 5.x -- explicit tagged union

Lua uses a structure nearly identical to Turmeric's `TuriValue`:

```c
typedef struct TValue {
    union { /* value */ } value_;
    lu_byte tt_;   /* type tag: LUA_TNIL, LUA_TBOOLEAN, LUA_TNUMBER, ... */
} TValue;
```

No bits are stolen from values; the tag is a separate byte. LuaJIT replaces
this with NaN boxing for performance (see below).

### NaN boxing -- LuaJIT, JavaScriptCore

A 64-bit IEEE double has ~2^52 spare NaN bit-patterns. These are used to
encode non-double values:

```
Normal double:  any non-NaN 64-bit pattern
NaN-boxed value: [ 1 | 11111111111 | 1 | tag (3 bits) | payload (48 bits) ]
```

48 bits is enough for any pointer on current x86-64 hardware. Tags
distinguish: `object`, `string`, `boolean`, `null`, `undefined`, `int32`.

**Pros:** Every value fits in one 64-bit register with no separate tag word.
**Cons:** Real doubles require a round-trip check; only ~7 distinct tags fit
without another indirection level.

### Ruby / SBCL / Scheme -- pointer tagging (low 3 bits)

These languages use the low 2--3 bits of a `uintptr_t` to encode the type of
immediates, relying on heap objects being aligned (so their low bits are always
`000`):

```
Ruby (low 3 bits):
  001  fixnum (int shifted left by 1)
  000  heap pointer -> RObject with flags word
  010  float
  000/special  false, true, nil, undef
```

SBCL (Common Lisp) uses 3 low bits similarly, with a second-level **widetag**
byte in the block header for heap objects needing finer discrimination (bignum,
simple-vector, hash-table, etc.).

### Summary table

| Language | Approach | Value overhead |
|---|---|---|
| GHC Haskell | Dictionary at call site; no tag in value | None |
| OCaml | LSB=1 for int; block header tag for heap | 1 bit/int; 1 word/heap obj |
| Lua 5.x | Explicit tagged union (like `TuriValue`) | 1 extra byte per value |
| LuaJIT | NaN boxing | None (fits in 64 bits) |
| Ruby / SBCL | Low bits of pointer + object header | 1 bit/int; 1 word/heap obj |

### Recommendation for Turmeric

The **GHC approach** (SI4-C Option A) fits Turmeric's architecture best:
- No changes to value representation or generated C.
- No overhead on ordinary (non-REPL) code paths.
- The static type is already known by the compiler at every call site.
- Only the REPL boundary needs the type tag, not every value in the program.
- `turi_eval_typed` already provides the scaffolding; SI4-C Option A is a
  matter of making the type annotations richer (newtypes or `ptr<T>`) rather
  than architectural change.

NaN boxing or low-bit tagging would require changing Turmeric's entire value
representation and all generated C code -- worthwhile only if Turmeric later
adopts a GC that needs to trace heap objects, at which point a block-header
approach (like OCaml's) would be more natural than NaN boxing anyway.

---

## Recommended execution order

1. **SI4-A** (struct show) -- complete. Custom Show instances and derive-show
   structs display via `turi_try_show` in both REPLs.
2. **SI4-B** -- complete (primitives already work via `turi_value_repr`).
3. **SI4-C Option A -- investigation:**
   - First check if `ptr<Pair>` is already a parseable type (it may be, given
     `TY_PTR_VOID` exists). If so, changing `pair-new`'s return annotation may
     be all that's needed.
   - If not, design and implement opaque newtypes (`defopaque`) as a lightweight
     first step -- this is useful beyond SI4-C.
   - Implement `turi_show_result` as the third-tier dispatch (after `turi_try_show`).
   - Update all five stdlib constructor families.

---

## Open questions

1. **Should `turi_value_repr` for `TURI_CSTR` drop the surrounding quotes?**
   **Decision: keep quotes.** The REPL prints `"hello"` (with quotes), matching
   GHCi, Python, and Clojure conventions. This makes the type visible at a glance
   and distinguishes `"42"` (a string) from `42` (an int). `Show [cstr]` returning
   the bare string is correct for user-facing output; the REPL repr is a separate
   concern.

2. **Depth limit for recursive struct repr?**
   **Decision: hard limit of 4, truncate with `#<struct TypeName>`.** -- **DONE (OQ2).**
   `turi_value_repr` now calls `turi_value_repr_d(buf, cap, v, 4)` internally.
   When depth reaches 0, emits `#<struct TypeName>` instead of expanding fields.

3. **Memory ownership of `turi_show_result` return values (SI4-C only).**
   **Decision: per-evaluation scratch arena.**
   A bump arena is allocated at the start of each top-level REPL evaluation and
   reset (not freed) after the result string is printed. All `show_*_ptr` helpers
   allocate into this arena via an `arena_alloc(arena, size)` helper rather than
   calling `malloc` directly. No per-call `free` is needed; nested calls (e.g.
   `show_cons_ptr` calling `show_int` on each element) compose naturally without
   coordination.

   The arena must be implemented before SI4-C begins. Suggested interface:

   ```c
   typedef struct ShowArena {
       char  *buf;
       size_t cap;
       size_t used;
   } ShowArena;

   ShowArena  show_arena_new(size_t cap);      /* malloc's backing buffer */
   void      *show_arena_alloc(ShowArena *a, size_t size);
   void       show_arena_reset(ShowArena *a);  /* resets used=0, keeps buffer */
   void       show_arena_free(ShowArena *a);   /* frees backing buffer */
   ```

   A single `ShowArena` lives in `TuriEnv` (or is passed as a parameter through
   the eval call stack). `turi_wasm_eval` resets the arena before each call and
   reads the result string before the next reset.

   Note: SI4-A (`turi_try_show`) currently uses `strdup` for its return value
   and requires the caller to `free` it. This is acceptable for REPL use where
   one show call happens per expression; the arena becomes necessary for SI4-C
   where `show_cons_ptr` may allocate O(n) strings for a long list.

4. **`turi_try_show` inline-C limitation.**
   Show instances whose `show` method body uses intermediate C local variables
   (e.g. `int64_t fst = p.first; snprintf(buf, ..., fst)`) do not work through
   the interpreter's pattern matcher. The fix is to write `snprintf` arguments as
   direct field accesses (`(long long)p.first`). All stdlib Show instances in
   `pair.tur`, `option.tur`, `list.tur`, `vec.tur`, `typeclass.tur` should be
   audited against this rule when SI4-C is implemented.
