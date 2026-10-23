# Plan: a generator type `(Generator T)` and a typed `gen-unwrap`

> **Status:** DONE 2026-10-23 (archived). Decision taken (generator-forms-gaps
> item 1): add a parameter type and type `gen-unwrap` by it. All five design
> items landed; see "Outcome" at the end for where the implementation differs
> from the design below.

## Problem

A generator can be consumed only where it is made, or through the `gen-for-each` /
`gen-collect` / `gen-nth` macros:

- `(defn g [x : generator] : int (gen-unwrap (gen-next x)))` is `gen-next expects a
  generator value, got tyvar`; un-annotated, the parameter defaults to `int` and
  fails the same way. `(Generator int)` is a kind error (TUR-E0012).
- A struct yield is unusable: `(.a (gen-unwrap (gen-next g)))` is `no typeclass
  method found for 'a'`, because the element is read as an `int`.

`TY_GENERATOR` carries only `element_kind` and, inside a generic, `element_tyvar`
(`types.h`); `gen-unwrap` reads the 64-bit slot and re-types it by that kind
(`elab_gen_unwrap`, `EX_GEN_UNWRAP` in `emit_expr.c` and `eval.c`).

## Design

1. **The annotation.** `(Generator T)` in a type position resolves to
   `TY_GENERATOR` with the element type `T` (`type_expr_from_form`). Store the
   full element type (`const Type *element`), not just the kind, beside the
   existing fields; `element_kind` stays for the scalar fast paths. Type equality
   compares elements. The bare spelling `generator` keeps meaning "a generator of
   unknown element" and stays readable only by the macros, or becomes an error
   pointing at `(Generator T)` -- pick the error, it is what the report hit.
2. **`gen-next` / `gen-done?` on a parameter.** They take any `TY_GENERATOR`;
   today the checks see a tyvar because the annotation did not resolve. With (1)
   they work unchanged.
3. **Typed `gen-unwrap`.** The element type comes from the generator's type when
   the argument is `(gen-next g)` or a binding initialised by one (both already
   tracked); `gen-unwrap` of `(gen-next x)` with `x : (Generator T)` is a `T`.
4. **Aggregate elements.** A struct or ADT yield does not fit the 64-bit slot.
   `yield` of a by-value aggregate stores a pointer to a copy in the generator's
   state (one box per generator, overwritten per yield, freed with the state), and
   `gen-unwrap` reads through it and copies out. The interpreter holds the yielded
   value beside the box already (`box_val`), so it only needs the type.
5. **Generics.** `(defn take2 [T] [g : (Generator T)] : T ...)` -- the element
   tyvar path (`element_tyvar`) already exists for a generator made inside a
   generic; extend it to one passed in.

## Fixtures

A function taking `(Generator int)` and one taking `(Generator float)` (floats
must come back as floats, not bits), a struct yield read with `.a` through a
parameter, a generic `take2`, and `errors/` for a `(Generator int)` argument
passed a `(Generator cstr)`. All three engines.

## Related

The generator state is never freed today (archived
generator-capture-yield-and-exhausted-reads notes it); a parameter that outlives
its creator makes that more visible but does not change it. Keep it out of this
plan.

## Outcome (2026-10-23)

- **(1)** `(Generator T)` resolves to `TY_GENERATOR` in every annotation path
  (`type_expr_from_form`, the `defn`/`fn` ladder `fn_type_from_form`, struct
  fields). The Type carries `generator_.element` (the full type) beside
  `element_kind` / `element_tyvar`; `type_generator` builds it, and `gen`
  records its first yield's full type the same way. `type_eq` compares
  elements, with an unknown or type-variable element matching any; the call
  check refuses a mismatch through `handle_targets_conflict` (`expected
  (Generator int), got (Generator cstr)`). Types print as `(Generator T)`. The
  bare `generator` / `Generator` (spaced or fused `:generator`) is an error
  naming `(Generator T)` -- the error, as the design picked. A program's own
  type named `Generator` still wins.
- **(2)** Unchanged, as predicted. One latent bug fixed on the way:
  `gen-next` on anything but a `(gen ...)` literal read `as.gen_.def` off
  another union member (an `EX_VAR`'s binding), which `srfi_prune` then walked
  as a `GenDef`.
- **(3)** `gen-unwrap` reads the element's full type through the
  `(gen-next g)` or the let binding (`Binding.gen_elem_type`).
- **(4)** Differs from the design: no per-generator box. The yield already
  boxed a by-value aggregate through the container-element bridge
  (`emit_carrier_bridge_escaping`), one heap copy per yield, and the
  generic-in-generator read already bridged a carrier word back; a struct,
  ADT, container (`TY_APP`) or function element now takes that read at its
  own type. The per-yield copy leaks as the per-yield `int64_t` slot already
  did -- the never-freed generator state the Related note keeps out of scope.
  The interpreter returns `box_val` for those kinds. A later `yield` of a
  different struct / ADT / container than the first is now an error (the
  kinds agreeing let a `Q` be read as a `P`).
- **(5)** `call_collect_type_bindings` and `call_instantiate_type` recurse
  into the element, and the tyvar walkers in `elab_call.c` see through it, so
  `(defn take2 [T] [g : (Generator T)] : T ...)` binds `T` per call.
- Fixtures: `gen-param-typed` (int / float / cstr parameters, returns, a
  generator parameter driven from another generator, `let`, `fn` and struct
  field annotations), `gen-param-struct-yield` (struct and ADT yields, local
  and through a parameter; a vector and a closure element),
  `gen-param-generic`, `gen-param-stdlib-macros` (compiled + JIT; inline-C
  carve-out under turi), and `errors/gen-param-element-mismatch`,
  `errors/gen-bare-generator-type`, `errors/gen-type-arity`,
  `errors/gen-yield-struct-mismatch`. All three engines.
- Found on the way, filed, not fixed:
  [gen-body-two-vec-literals-duplicate-state-member](../reported/gen-body-two-vec-literals-duplicate-state-member.md),
  [gen-collect-rejects-float-elements](gen-collect-rejects-float-elements.md).
