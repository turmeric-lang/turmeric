# `ref?` and `weak?` are stubs, `upgrade` returns an untyped pointer, and the interpreter's `rc->ptr` returns the payload

**RESOLVED 2026-10-21** -- see [Resolution](#resolution-2026-10-21). Pinned by
`ref-weak-builtins` (both engines, leak-checked) and
`errors/weak-upgrade-builtin-retired`.

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

## Resolution (2026-10-21)

All five items, on both engines.

1. **`ref?` / `weak?`** answer from the argument's static type (fix direction
   1): `true` iff it is a `ref<T>` / a `weak<T>`. `emit_expr.c` evaluates the
   argument and yields the constant; `eval.c` reads the same elaborated type,
   so the engines agree. A linear variable passed to either is read, not
   moved (`elab_pred_read_is_not_a_move`, mirroring `elab_deref`), so
   `(ref? r)` followed by `(deref r)` compiles.
2. **`upgrade` is retired** for `weak/upgrade` (the second half of fix
   direction 2). Typing the node `(Option rc<T>)` would have put a box the
   compiler cannot see the owner of into the carrier-sum-option machinery
   ([carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md)),
   for a spelling that `weak/upgrade` (tur/weak) already provides correctly.
   `(upgrade w)` is now an error naming `weak/upgrade`, `weak/unwrap` and
   `rc/drop`; `EX_WEAK_UPGRADE` is gone, and with it the `let`-ownership
   special case (`binding_owns_upgrade_box`) that freed its box. The five
   fixtures that used it -- `gc-cycle-freed`, `gc-deterministic`, `gc-mixed`,
   `weak-dangling`, `weak-upgrade-option` -- use `weak/upgrade` and `some?`
   now instead of an inline-C tag peek, and `weak-upgrade-option` also
   releases the strong handle the upgrade mints.
3. **Interpreted `rc->ptr`** returns the address of the payload word in the
   `__rc` record (fix direction 3), so `(ptr-deref (rc->ptr r))` reads 5, as
   compiled.
4. **`lref/new`'s cell** is freed by the `deref` that consumes it (fix
   direction 4): `EX_DEREF` frees an `lref<T>` operand it owns -- a fresh
   `(lref/new ...)` or an owning variable (`is_linear`, not
   `is_nonowning_ref`) -- after reading the value. A field read is left
   alone. The `linear-lref-*` fixtures are leak-clean.
5. `ref-weak-builtins` covers 1, 3 and 4 on both engines under
   `requires.leak-check`; `errors/weak-upgrade-builtin-retired` pins 2.

The reference page (`stdlib/forms/references.tur`) documents the new answers
and the retirement, with doctested examples, and c-integration-guide's weak
section names `weak/upgrade`.
