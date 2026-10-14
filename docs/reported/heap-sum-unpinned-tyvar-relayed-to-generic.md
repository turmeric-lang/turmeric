# A `:heap` sum whose element nothing in the program fixes, passed to a generic, still reaches the dead-base trap

**Severity: low.** A correct program aborts at runtime, but only one whose
element type is genuinely undetermined: nothing anywhere fixes `T`. The
interpreter runs it.

**Discovered:** 2026-10-14, executing
[heap-sum-generic-never-specialized](../archive/heap-sum-generic-never-specialized.md).
That fix covers a pinned element, an open slot at a constructor, and an open
slot reaching a generic through the call's bindings. This is the one shape it
leaves.

## Repro

```turmeric
(defdata Fr :heap [T] (Nil) (Frame T T))
(defn bump [T] [f : (Fr T)] : (Fr T)
  (match f
    (Nil) (Nil)
    (Frame a b) (Frame b a)))
(defn has? [T] [f : (Fr T)] : bool
  (match f
    (Nil) false
    (Frame a b) true))
(defn main [] : int
  (println (has? (bump (Nil))))
  0)
```

- **Compiled:** `tur: internal error: base constructor 'ctor_Fr_Nil' of a
  parametric type has no definition; a call reached the dead base generic path
  instead of a per-spec clone`, then `abort()`.
- **`tur interpret`:** prints `false`.
- `(match (Nil) ...)` alone, with no generic, works: the constructor grounds
  its open slot to `int` (`emit_heap_ctor_open_suffix`).

## Root cause

`(Nil)` here types as the bare `Fr`, and the call's ABI binding for `bump`
is `T -> T`: a **named** type variable, not an `open_slot` one. That is the
same shape a generic relaying its own `T` produces, so
`emit_abi_register_call` cannot tell "never fixed" from "relayed"
(`src/compiler/emit_module.c:6942` grounds only open slots). No clone is
minted, and the base `bump` runs its own `(Nil)` arm, which names the
dead-base trap.

## Fix directions

1. **Mark it open at elaboration.** When a constructor application's
   parameter is still unsolved at the end of the enclosing monomorphic
   definition, record it as an open slot (as `(One 7.1)`'s `B` already is)
   rather than leaving the callee's own name in the binding. The existing
   grounding then applies unchanged.
2. **Or ground at a monomorphic call site.** In a caller that is not itself
   generic and not inside a spec, a binding that still resolves to a type
   variable cannot be a relay, so it can be grounded to `int`. This needs a
   reliable "the current function has no type parameters" signal.
3. **Pin it** by adding the repro's `(has? (bump (Nil)))` line to
   `tests/fixtures/heap-sum-generic-rebuild`.
