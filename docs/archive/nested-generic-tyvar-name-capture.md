# A generic calling a generic with the same type-variable names gets its caller's bindings -- RESOLVED

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

## Resolution

Resolved 2026-10-17. The name collision was real, but not where this report
looked: `list-map` and `list-map-onto__` sharing `[A B]` was harmless on its
own. It took a nullary generic argument, `(list-nil)`, whose own variable is
also `A`, and a second defect showed up once that was fixed.

**1. Elaboration** (`call_collect_type_bindings`, `src/compiler/elab_call.c`).
Binding `(list-map-onto__ xs f (list-nil))` inside `list-map [A B]`:

- `xs` binds onto__'s `A := A`;
- `f : (fn [A] B)` binds `B := B`, i.e. onto__'s `B` to list-map's `B`;
- `(list-nil)` is `(List A)` with list-nil's `A` marked an open slot (it
  fixes nothing; declared-tyvars-are-not-rigid).

The branch for generic-map-assoc-rejects-sig-tyvar-value treats a binding
spelled `B := B` as an unfixed self-binding (`(map-new)`'s `V := V`) and lets
a later signature variable take its place. Here the spelling was a
coincidence, and the open `A` replaced it: `B := A`, which list-map's spec
resolves to `float`. An open slot now never replaces a binding in that branch,
as the first branch's rule already said. Renaming onto__'s variables to `[C
D]` worked only because `D := B` does not look like a self-binding. This also
fixes the int -> float direction, which the old compiler got wrong too
(`(list-map ints (fn [n : int] : float ...))` printed `0`).

**2. Emission** (the cross-spec fallback in `emit_call_name`,
`src/compiler/emit_core.c`, and `find_matched_abi_spec`, `emit_expr.c`). With
both directions in one program there are two `list-map-onto__` specs, and
their clone names collide (every parameter is the `int64_t` carrier), so the
second is the `__h1` twin. Inside the int -> float twin, `(list-first xs)`
needs no clone (`A := int` rides the carrier), so nothing was recorded for it
there. Both lookups then fell back to the entry recorded under the float ->
int sibling, `list_first__spec__double_int64_t`, whose parameter C types match
because `(List int)` and `(List float)` are both the carrier. Each int element
was read as a double. `cross_spec_clone_bindings_fit_call` now requires the
call's own type bindings, resolved under the active spec, to give the same C
types as the clone's. Both sides name the callee's variables, so comparing by
name is sound. A dictionary-dispatched method call is exempt:
`emit_call_name` re-resolves it to the active spec's instance, and
`find_matched_abi_spec` borrows only its result shape
(`class-var-applied-result-in-constrained-generic` depends on that).

**Pinned by** `generic-forward-nullary-arg-tyvar-name`:

- `list-map` float -> int, int -> float (with its argument used),
  float -> float and float -> a by-value `defdata`, all in one program;
- a user-defined `outer [A B]` forwarding to `inner [A B]` next to
  `(list-nil)`.

It passes on both engines. The old compiler segfaults on it.

The unminimized `list-foldl` warning mentioned in the by-value list report was
not reproduced; whether it was this bug is unknown.
