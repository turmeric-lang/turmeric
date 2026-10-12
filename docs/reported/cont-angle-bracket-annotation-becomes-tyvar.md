# `cont<T>` (angle-bracket) type annotation silently becomes a type variable

**Severity: medium (silent loss of type checking).** Nothing a program does
with such a parameter is checked. This is how
[resume-of-cont-typed-param-is-a-no-op](../archive/resume-of-cont-typed-param-is-a-no-op.md)
stayed silent: the direct emitter's `resume` treated an unrecognised
continuation type as "return the value". Found 2026-10-12 while fixing that
report.

This is the same defect as
[rc-angle-bracket-annotation-becomes-tyvar](../archive/history/rc-angle-bracket-annotation-becomes-tyvar.md)
(fixed 2026-07-20), for the continuation types. That fix did not cover them.

## Repro

```turmeric
(defn helper [k : cont<int>] : int (+ k 1))   ; TUR-E0006: "first arg type tyvar"
(defn h2 [k : cont<cstr>] : cstr k)          ; accepted: a tyvar returns as cstr
(defn h3 [k : effect-cont<int>] : int 1)     ; accepted, also a tyvar
```

A `cont<int>` parameter behaves like an unconstrained generic `A`. You can
pass it anything and use it nearly anywhere.

## Root cause

`cont<int>` reaches the elaborator as ONE symbol whose text is `cont<int>`.
It is not read as `cont` applied to `int`. The annotation ladders try, in
order:

1. The bare continuation names. `cont_flavor_from_name`
   (`src/compiler/types.h:1709`) matches only `cont`, `escape-cont`,
   `serial-cont`, `effect-cont` and `multishot-effect-cont`.
2. The angle-bracket forms. `ptr<T>` (`ptr_type_from_keyword_name`) and
   `rc<T>` / `ref<T>` / `lref<T>` / `weak<T>`
   (`rc_family_type_from_keyword_name`, `src/compiler/elab_types.c:546`)
   have one. Nothing handles `cont<`.
3. Aliases and ADT names.
4. The fallback: "an unknown keyword is an implicit type variable"
   (`src/compiler/elab_fns.c:10027`).

The KB-026 typo pass (`elab_fns.c:11013`) demotes a name that appears once in
a signature, but only to another named type variable, so no diagnostic fires.

Meanwhile the language spells these types `cont<T>`. TUR-E0001 prints
`expected cont<?>`, and four guides write it: `effects-system-guide.md:654`,
`delimited-control-operators-guide.md:121`, `async-await-guide.md:220` and
`effects-vs-monads.md:682`.

## Why parsing it alone is not the fix

A handler case's `k` is typed `int` (an `is_continuation` binding). Every
continuation-typed parameter refuses it at the call: `[k : effect-cont]`
gives TUR-E0001 "expected cont<?>, got int". So the spellings that let a case
hand its `k` to a helper are `int`, no annotation, or an accidental type
variable such as `cont<int>`. If `cont<int>` resolved to a real `cont`, that
pattern would stop compiling.

The `k` a case hands out also has two runtime shapes:

- a bare DK chain, from a one-shot case;
- a `tur_cloneable_cont` wrapper, from a `^multishot`, fn-payload or
  `__Shift` case (`case_is_multishot`, `src/compiler/emit_cps_ir.c`).

`__dk_resume_word` resumes either shape from a one-shot resume. The
multishot snapshot resume reads only the wrapper.

## Fix directions

1. Resolve `cont<T>`, `escape-cont<T>`, `serial-cont<T>`, `effect-cont<T>`
   and `multishot-effect-cont<T>` to the flavored `TY_CONT` with
   `returns = T`, the same as the one-argument `(cont T)` form. Wire it into
   every site that calls `rc_family_type_from_keyword_name`.
2. Let a handler case's `k` pass where an `effect-cont` is expected, so a
   helper can declare its continuation honestly. Allow `multishot-effect-cont`
   only when the case's `k` is multishot: its snapshot resume cannot read a
   bare chain.
3. Then the resume can be typed from `returns`, which is related to the
   resume typing gap noted in the archived report.

The new fixture `tests/fixtures/resume-handler-k-outside-its-case` uses
`cont<int>` on purpose, because that is the archived report's repro. Update
it when this lands.
