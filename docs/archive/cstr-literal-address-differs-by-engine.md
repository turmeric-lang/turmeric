# Two identical string literals compare equal by address under `tur run`, unequal under `tur jit` -- in plain Turmeric

**RESOLVED 2026-10-23** -- fix direction 1, by
[string-literal-interning-plan](string-literal-interning-plan.md): the emitter
gives each distinct literal text in a unit one `static const char
__tur_str_<N>[]` (`cstr_codegen_register`, `src/compiler/emit_core.c`) and every
use names it, so the repro prints 1 on all three engines. Pinned by the fixtures
`cstr-literal-one-object-per-unit` and `cstr-literal-nul-is-distinct`. Across
units literals stay two objects (jit-guide).

**Decision (2026-10-21):** store each distinct literal once per unit (fix
direction 1).

**Severity: low-medium** (engine divergence; silent wrong answer). Found
2026-10-21 reviewing the resolution of
[jit-string-literal-identity-differs-from-cc](../archive/jit-string-literal-identity-differs-from-cc.md),
which declined to make literal identity a guarantee partly because it "would
rewrite every snapshot" -- not a reason under CLAUDE.md's fixture policy -- and
partly because only inline C could observe it. The second reason does not hold.

## Repro

No inline C, no `unsafe`:

```turmeric
(defn main [] : int
  (println (if (= (:: ":user/name" :int) (:: ":user/name" :int)) 1 0))
  0)
```

`tur run`: `1`. `tur jit`: `0`. `tur --interpret`: `1` (there `(:: "hi" :int)`
is the string itself, not an address -- type-forms-gaps item 7).

## Root cause

The emitter writes each string literal inline where it is used
(`{.s = ":user/name"}` twice in the repro's C). Whether two identical C literals
are one object is unspecified: gcc merges them within a unit
(`-fmerge-constants`), c2mir gives each its own storage. An erasing ascription of
a `cstr` to `:int` hands the address to the program, so the answer depends on
which C compiler ran. The archived report fixed the one program that depended on
it (`examples/datalog/indexed.tur`) and documented the rule in jit-guide and
c-integration-guide; the language itself still lets an ordinary program depend
on it.

## Fix directions

1. **Make identity a guarantee within a unit.** Emit one `static const char
   __tur_str_N[]` per distinct literal text and reference it at every use, so cc
   and the JIT see one object. Regenerates every snapshot that contains a string
   literal; that is fine under the fixture policy. Across units it stays
   unspecified (as it is for gcc without LTO), which the jit-guide note already
   says.
2. **Or stop handing out the address.** Make `(:: s :int)` of a `cstr` an error
   outside `(unsafe ...)` -- it is the `:int` stand-in CLAUDE.md warns against --
   and keep inline C as the only way to read a literal's address.
3. Either way, the interpreter's answer for `(:: "hi" :int)` (the string, not a
   number) should follow whichever rule is picked.
