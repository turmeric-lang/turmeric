# `resume` of an `int` that is not a continuation crashes compiled

**Severity: low-medium.** It needs a program that resumes a number. That is
a misuse, but the compiler accepts it, and the result is a segfault rather
than a diagnostic. Noted 2026-10-12 in
[resume-of-cont-typed-param-is-a-no-op](../archive/resume-of-cont-typed-param-is-a-no-op.md)
and filed 2026-10-13. Before that fix the same program failed to link (it
called the deleted `tur_effect_cont_resume`), so the failure moved from
build time to run time.

## Repro

```turmeric
(defn h [k : int v : int] : int (resume k v))
(defn main [] : nil (println (h 5 6)))
```

| | result |
| --- | --- |
| `tur run` | Segmentation fault |
| `tur --interpret` | `eval: resume: not a continuation` |
| `main` before the resume fix | link error, `tur_effect_cont_resume` undefined |

## Root cause

`elab_resume` accepts any `k`. A non-multishot `resume` lowers to
`__dk_resume_word((intptr_t)k, v)` (`emit_effects_resume`,
`src/compiler/emit_effects.c`), which compares the word's first field with
`__dk_cont_fn` and otherwise treats it as a `DK *`. A plain integer is
neither, so it is dereferenced.

`int` has to stay accepted for now, because it is how a handler `k` is still
carried in two places:

- a `^mut` cell it is stored into (`(set! saved k)`, then
  `(resume saved v)`, the B7 escaping-continuation shape);
- an untyped helper parameter (`(defn helper [k v] (resume k v))`).

Nothing distinguishes those from `(h 5 6)`. That is the
CLAUDE.md "no lazy `:int` stand-ins" problem in miniature.

## Fix directions

1. **Give a stored handler continuation a real type.** Since
   `cont-angle-bracket-annotation-becomes-tyvar` a handler `k` passes as an
   `effect-cont` parameter. Extend that to a `^mut effect-cont` cell (the
   assignment check). Then warn on `resume` of a `k` whose type is a plain
   `int` and that is not an `is_continuation` binding, and later refuse it.
2. **Cheaper interim:** refuse `resume` when `k` is a literal or an
   arithmetic expression, which can never be a continuation. That catches
   the obvious misuse without touching the carrier spellings.
