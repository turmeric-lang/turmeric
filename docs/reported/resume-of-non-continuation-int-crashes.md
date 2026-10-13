# `resume` of an `int` that is not a continuation crashes compiled

**Narrowed 2026-10-13: a word that cannot be a pointer is a panic, not a
crash.** Both runtime helpers (`emit_dk_resume_word_prelude`,
`src/compiler/emit_dk_runtime.c`) now ask `__dk_word_not_cont` before they
dereference. The answer is yes for a word in the first page (below 4096, which
no supported target maps) or one not aligned to a pointer, since a DK node and
a `tur_cloneable_cont` are structs holding pointers. Then:

- `__dk_resume_word` panics `resume: not a continuation` (the interpreter's
  words). Under `catch-unwind`, `tur_panic` returns, so the helper returns 0
  and the caller's check unwinds; the panic is caught with its message.
- `__dk_cont_live_word` answers `false`, as the interpreter does.

The repro now panics, and `(cont? 5)` prints `false`. A real continuation
passed through the same `:int` helper still resumes. Pinned by
`tests/fixtures/resume-non-continuation-panics` (0, 5, 4095 and the misaligned
4097, each caught; `cont?` of three of them; a real `k` through `(helper k
41)`). With the guard reverted the fixture fails (stdout mismatch).

**Still open:**

- **The heuristic's limit.** An `int` that looks like a pointer -- aligned and
  at least 4096, `(cont? 4096)` -- is still dereferenced, and still crashes.
  Only fix direction 1 below, a typed `k`, can tell a continuation from a
  number.
- **The interpreter's error is not a panic.** `turi_error`
  (`src/turi/eval.c`), as for `discontinue`, ends the program even inside
  `catch-unwind`; compiled, the panic is caught. The fixture is
  `requires.compiled` for that reason.
- **The panic names the runtime's line.** `panic at <unit>.c:N`, the
  runtime-helper case of
  [panic-location-names-the-runtime-not-the-call-site](panic-location-names-the-runtime-not-the-call-site.md).

**Severity: low (was low-medium).** It needs a program that resumes a number. That is
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

`(cont? k)` has the same shape. Since
[cont-query-on-wrapped-handler-k-reads-past-wrapper](../archive/cont-query-on-wrapped-handler-k-reads-past-wrapper.md)
it lowers to `__dk_cont_live_word`, which reads a word the same way. So
`(defn main [] : nil (println (cont? 5)))` segfaults compiled (it used to
fail to link). The interpreter prints `false`. Any fix here should cover both.

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
