# Two identical string literals compare equal by address under `tur run`, unequal under `tur jit` -- in plain Turmeric

**Resolved 2026-10-23** by [string-literal-interning-plan](string-literal-interning-plan.md) (fix direction 1). `atom_cstr` now names one `static const char __tur_str_<N>[]` per distinct literal in the unit (`cstr_codegen_register` / `cstr_codegen_emit`, `src/compiler/emit_core.c`), keyed on (bytes, length), written first at final assembly in both the program and implementation-unit paths. The repro is `1` under `tur run`, `tur jit` and `--interpret`; full `tests/run.sh` and `tests/run-jit.sh` are green on the change. Across units (a `--shared` build) two literals stay two objects; jit-guide and c-integration-guide now say "one object per unit, not across units". Item 3 (what the interpreter's `(:: "hi" :int)` should be) is unchanged: it compares by content, which agrees on equal literals and differs only past an embedded NUL. Pinned by `tests/fixtures/cstr-literal-one-object-per-unit` and `tests/fixtures/cstr-literal-embedded-nul-own-object`.

**Decision (2026-10-21):** store each distinct literal once per unit (fix
direction 1); plan: [string-literal-interning-plan](string-literal-interning-plan.md).

**Severity: low-medium** (engine divergence; silent wrong answer). Found
2026-10-21 reviewing the resolution of
[jit-string-literal-identity-differs-from-cc](jit-string-literal-identity-differs-from-cc.md),
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
