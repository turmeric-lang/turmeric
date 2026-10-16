# An `int` argument binds a class's only instance, even a `cstr` one

**Severity:** high. `tur check` accepts a call whose argument type has no
instance. The emitted C reinterprets the value as the instance's type, and the
program segfaults (or cc fails).

## Repro

```turmeric
(defclass Shout [a] (shout [x : a] : int))
(definstance Shout [cstr] (shout [x] (do (println x) 0)))
(defn main [] : int
  (shout 5)
  0)
```

- `tur check`: succeeds.
- `tur run`: cc notes `source object is likely at address zero`, and the
  program dies with `Segmentation fault`. The emitted C casts `5` to
  `const char *`.

With a lone struct instance instead of `cstr`, the C compiler rejects the
program rather than crashing it.

## Root cause

`src/compiler/elab_typeclasses.c:elab_method_call_impl` resolves a
receiver-directed call through tiers. `:int` is deliberately excluded from
`concrete_distinct_receiver`, because an `int` may be a carrier word standing
for an erased value. So an `int` receiver falls through to the fallback tier.
The ambiguity check (TUR-E0020) fires only when there are **two or more**
fallback candidates. With exactly one instance in scope, it is bound silently,
whatever its head type.

## Fix directions

- When the lone fallback's head kind cannot be carried in an `int` word
  (`cstr`, a by-value struct, a float), report TUR-E0015 (no instance) rather
  than binding it.
- Keep the carrier allowance only for heads the int carrier can actually
  represent.
