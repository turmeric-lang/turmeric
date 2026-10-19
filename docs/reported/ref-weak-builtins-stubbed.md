# `ref?` and `weak?` are stubs, `upgrade` returns an untyped pointer, and the interpreter's `rc->ptr` returns the payload

**Severity: low-medium.** Four of the builtin reference forms do not do what
their names say. Nothing in the stdlib calls them -- `stdlib/weak.tur` wraps
the runtime directly -- so this bites a program that reaches for the
intrinsics. Found 2026-10-19 checking the examples for their API reference
(`stdlib/forms/references.tur`).

| Expression | Compiled | Interpreted | Expected |
| --- | --- | --- | --- |
| `(weak? 3)` | `true` | `true` | `false` -- 3 is not a `weak<T>` |
| `(ref? 7)` | `true` (and cc: `comparison between pointer and integer`) | `true` | `false` |
| `(ref? 0)` | `false` | `true` | `false` |
| `(let [r (rc/of 5)] (unsafe (ptr-deref (rc->ptr r))))` | `5` | **`tur` segfaults** (ASan: SEGV at address 0x5, `eval.c:5478`) | 5 |
| `(some? (upgrade w))` | TUR-E0001: expected `(Option A)`, got `ptr<void>` | same | an `(Option rc<T>)` to test |

## Root cause

- **`weak?`.** Compiled: `bool t = true; // weak? not fully implemented in
  Phase 9` (`src/compiler/emit_expr.c`, `case EX_WEAK_PRED`). Interpreted:
  `return turi_bool(true)` (`src/turi/eval.c:14039`).
- **`ref?`.** Compiled: `(x != NULL)` on whatever the argument is
  (`emit_expr.c`, `case EX_REF_PRED`), so it tests the value, not its type.
  Interpreted: `tag != TURI_NIL` (`eval.c:14012`). Both accept any argument
  (`elab_ref_pred`, `src/compiler/elab_memory.c:531`).
- **`rc->ptr`, interpreted.** `case EX_RC_PTR` returns the rc's payload
  field itself (`eval.c:13961`), not an address, so a read through it
  dereferences the value.
- **`upgrade`.** `elab_weak_upgrade` types the result `ptr<void>`, "an
  option<rc<T>> ... Callers use some?/option-unwrap" (`elab_memory.c:482`), but
  no `Option` function accepts a `ptr<void>`; and the strong handle it mints on
  success can then never be `rc/drop`ped. `weak/upgrade` (`stdlib/weak.tur`) is
  the working spelling.

## Also: `lref/new`'s cell is never freed

`(let [p (lref/new 99)] (deref p))` -- the shape structs-guide and
`tests/fixtures/linear-lref-propagation` use -- compiles to a
`malloc(sizeof(int64_t))` that nothing frees: `deref` consumes the `lref<T>`
for the linearity check, but no free is emitted for it, while a `ref<T>`
binding of the same shape gets a scope-exit `drop!` (compare
`./build/tur emit-c` of the two). Each `lref/new` leaks 8 bytes.

## Fix directions

1. `ref?` and `weak?`: the argument's type is static, so answer from it at
   elaboration (`true` iff `TY_REF` / `TY_WEAK`) -- or retire both, since a
   program never needs to ask.
2. `upgrade`: type the result `(Option rc<T>)` and build it with
   `tur_some_ptr` / `tur_none`, as `weak/upgrade` does; or retire it in favour
   of `weak/upgrade`.
3. Interpreted `rc->ptr`: return the address of the payload cell.
4. Free an `lref<T>`'s cell when `deref` consumes it.
5. A fixture for each on both engines (leak-checked for the `lref` case).
