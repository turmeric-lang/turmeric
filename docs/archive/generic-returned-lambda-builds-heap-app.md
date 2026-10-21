# A lambda RETURNED by a generic that builds a `:heap` parametric value reaches the dead-base trap

> **Resolved 2026-10-21.** Two gaps, both in `emit_abi_register_call`
> (`src/compiler/emit_module.c`):
>
> - **`mk-f`, the dead-base abort.** A lambda that captures nothing is lifted
>   to a global `__fn_N` and the body only names it, so
>   `returns_closure_fn_binding` was NULL and no clone was considered at all.
>   The register call now recognises a body that is a lifted lambda, and a
>   new `inner_app_heap` gate clones a returned lambda per spec whenever its
>   annotated result grounds to a parametric `:heap` application (or, for the
>   lifted case, a by-value one). The spec is matched on bindings, so the
>   `int` instantiation is no longer collapsed onto the base. The clone's
>   body is scanned under its own bindings, so its constructor call lands on
>   the monomorph, and `atom_var` already names a fn value's per-spec clone
>   (`emit_reresolve_fn_value`).
> - **`mk-k`, the garbage float.** Here the existing `inner_app_annotated`
>   gate did fire, but that clone deliberately shared the base env, so the
>   captured `x : A` stayed an `int64_t` slot. It now takes a suffixed env
>   when a capture changes C representation under the spec
>   (`emit_inner_captures_change_repr`).
>
> Pinned by `tests/fixtures/generic-returned-lambda-heap-app` (both lines of
> the repro at float, int and cstr, a by-value `(Pt A)`, and the `tcons-of`
> variant), which runs on both engines.

**Severity: medium.** A correct program aborts at runtime (`tur: internal
error: base constructor ... a call reached the dead base generic path`), or,
when the lambda captures a float it stores, prints garbage. The interpreter
runs both correctly.

**Discovered:** 2026-10-14, executing
[generic-zipper-plan](../archive/generic-zipper-plan.md) (ZP0). The zipper
does not hit it: `zip.tur` never returns a lambda directly, and a lambda
stored into a record field is cloned per specialization (that path, and the
word-shim it used to box, are fixed and pinned by
`generic-lambda-in-record-field-float`).

## Repro

```turmeric
(defstruct Box :heap :copy [A] (v A))

(defn mk-f [A] [x : A] : (fn [A] (Box A))
  (fn [y : A] : (Box A) (make-struct Box :v y)))

(defn mk-k [A] [x : A] : (fn [A] (Box A))
  (fn [y : A] : (Box A) (make-struct Box :v x)))

(defn main [] : int
  (println (.v ((mk-f 2.5) 7.1)))   ; aborts: ctor_Box_Box has no definition
  (println (.v ((mk-k 2.5) 7.1)))   ; (alone) prints 4.61281e+18, not 2.5
  0)
```

The same holds for a `(Cons A)` result built with `tcons-of`.

- **Compiled (gcc 13, clang 18):** the abort above, or `4.61281e+18`.
- **`tur interpret`:** `7.1`, `2.5`.

## Root cause

The lambda is lifted once, at the carrier ABI (`int64_t __fn_N(int64_t y)`),
and its body names the base constructor `ctor_Box_Box`, which a parametric
`:heap` type never defines. `emit_abi_register_call`
(`src/compiler/emit_module.c`, the `inner_float` / `inner_app` /
`inner_app_annotated` block) clones a RETURNED closure per spec only when its
result is a float, an unannotated application, or an application that grounds
to a by-value product. An ANNOTATED result that grounds to a `:heap`
application -- `(Box float)` -- matches none of them. Adding that case to
`inner_app_annotated` (tried) is not enough on its own: `mk-f` at `int` is not
specialized at all (it calls the base `mk_hyf`), and the float spec still
returns the shared `__fn_N`.

## Fix directions

1. Clone a returned lambda per spec whenever its body constructs a parametric
   `:heap` application that mentions the enclosing generic's variables, and
   make sure the enclosing generic itself is specialized at every such
   instantiation (an int instantiation included -- `int` and the carrier
   spell alike, which is why `mk-f` at `int` stays on the base today).
2. Pin both lines of the repro as a fixture.
