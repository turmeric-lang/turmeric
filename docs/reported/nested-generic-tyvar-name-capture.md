# A generic calling a generic with the same type-variable names gets its caller's bindings

**Severity: high (silent wrong value).** `(list-map floats (fn [f : float] :
int 7))` answers `4619679907765970534` instead of `7` on the compiled path,
with no diagnostic. The interpreter answers `7`. Found 2026-10-17 while fixing
[typed-list-byvalue-adt-element-unboxed](../archive/typed-list-byvalue-adt-element-unboxed.md);
reproduces on `main` at `8c539640` and with that fix on top of `69ecdea2`.

## Repro

```turmeric
(load "stdlib/list-typed.tur")
(defn main [] : int
  (let [fs (list-of 7.1 2.25)
        is (list-map fs (fn [f : float] : int 7))]
    (println (list-first is))   ; 4619679907765970534, want 7
    0))
```

`4619679907765970534` is `0x401C666666666666`, the bits of `7.1`. With a
result type that is a by-value `defdata` (`(fn [f : float] : Shape ...)`) the
same program segfaults.

## What the emitted C shows

`list-map [A B]` calls `(list-map-onto__ xs f (list-nil))`, and
`list-map-onto__` is declared `[A B]` too (`stdlib/list-typed.tur:205`). The
outer clone is specialized `A := float, B := int`, but the inner clone is
interned with `A := float, B := float` and calls `f` as returning a double:

```c
double __ps_509 = ((*( tur_thunk_double_double_t *)((void *)(intptr_t)(f)))(
                      (void *)(intptr_t)(f), __ps_508));
```

The closure returns `7` in `rax`; the caller reads `xmm0`, which still holds
the argument `7.1`.

Renaming `list-map-onto__`'s parameters to `[C D]` (and nothing else) gives
`C := float, D := int` and prints `7`. So the inner call's bindings are being
resolved against the caller's type variables by NAME: the inner `B` picks up
something bound to the outer `A`.

## Where to look

- `emit_abi_intern_spec` (`src/compiler/emit_module.c`, ~4729) interns a
  clone from `AbiTypeBinding { name, type }` pairs.
- A call inside a spec body gets its bindings through
  `emit_abi_register_call` (~6514) and `emit_type_through_spec`, which
  substitute the enclosing spec's bindings into the callee's types. When the
  callee's own type variables share names with the caller's, a substitution
  that walks the callee's declared types with the caller's bindings (or the
  reverse) cannot tell them apart.
- Printing the bindings in `emit_abi_intern_spec` with `type_print` shows the
  wrong `B := float` directly.

## Fix directions

- Alpha-rename a callee's type variables (or key bindings by their owning
  forall/binding, not by name) before composing them with the caller's.
- Until then, a stdlib generic that calls another with the same variable
  names is exposed: `list-map` -> `list-map-onto__`, `list-reverse` ->
  `list-reverse-onto__`, and likely others. Renaming the inner function's
  variables works around a single case but is not a fix.
- The unminimized `list-foldl` warning in the archived report above (a spec
  passing `const char *` where it declared `tur_adt_Html *`) may be this bug.
