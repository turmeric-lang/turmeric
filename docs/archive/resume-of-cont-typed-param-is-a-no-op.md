# A `resume` of a `cont<T>`-typed parameter compiles to its argument: the continuation never runs

**Fixed 2026-10-12.** Both backends now print 17 for the repro below, and
`tests/fixtures/resume-handler-k-outside-its-case` pins it, opted into the
leak gate.

- **The root cause was a little different from what this report says.**
  `cont<int>` does not elaborate as `TY_CONT` at all: an unknown `name<T>`
  head elaborates as a type variable (`(defn h [k : cont<int>] : int (+ k 1))`
  reports "first arg type tyvar"). The miscompile happened because the direct
  emitter's resume sent every k that was not `CK_MULTISHOT` and not `TY_INT`
  to the "return the value" arm. The `TY_INT` arm was broken too: it called
  `tur_effect_cont_resume`, which was deleted along with the fiber effect
  runtime on 2026-07-19, so `[k : int]` or an unannotated `k` failed to link.
- **The fix (fix direction 1).** `emit_effects_resume`
  (`src/compiler/emit_effects.c`) now lowers every `resume` that is not
  multishot through `__dk_resume_word` (`emit_dk_resume_word_prelude`,
  `src/compiler/emit_dk_runtime.c`). That is the same `dk_invoke` the CPS
  backend's `emit_resume` uses for any k that is not the case's own. The
  value is slot-stored the same way, a boxed aggregate is reaped, `consumed`
  is set, and a panic is propagated.
- **A helper cannot know which case its `k` came from.** A one-shot case
  hands out the bare DK chain. A `^multishot`, fn-payload or `__Shift` case
  (`case_is_multishot`) hands out that chain wrapped as a
  `tur_cloneable_cont`. `__dk_resume_word` tells the two apart by checking
  whether the word's first field is `__dk_cont_fn`. The check is only
  emitted when the cloneable <-> DK bridge, and so any wrapper, is in the
  program.
- **Two shapes that compiled to the same no-op are now compile errors.**
  Neither backend could run them:
  - `resume` of a cloneable, escape or serial `cont` (TUR-E0001, "call the
    continuation instead: (k v)"; `elab_resume`). The interpreter used to
    fail at run time with "resume: not a continuation". A `resume` of an
    `effect-cont` now builds the same EX_RESUME as `(k v)`.
  - A lexical (reified) `shift` whose receiver takes a one-shot
    `effect-cont` (TUR-E0016, as the cross-function route already said;
    `elab_cont_shift_core`). Its `k` is the cloneable wrapper, which a
    one-shot resume cannot read. `(k v)` there used to fail to link.
  - `tests/fixtures/errors/resume-of-cloneable-cont` and
    `errors/shift-receiver-one-shot-effect-cont` pin these.
- **Not addressed here:**
  - `cont<T>` silently becoming a type variable. A `cont<T>` that meant
    `TY_CONT` would refuse a handler `k` at the call
    ("expected cont<?>, got int"), so the spelling the repro uses is what
    now works.
  - A `resume` expression is typed as its VALUE, not as what the handle
    delivers. `(defn helper [k v : P] : int (resume k v))` with a struct `P`
    is a TUR-E0709 return mismatch, in the case body as much as in a helper.
  - The interpreter refuses an explicitly annotated `multishot-effect-cont`
    lexical shift receiver ("resume: not a continuation"), which compiles
    and runs.

---

**Severity: high (silent wrong answer), narrow.** Found 2026-10-12 probing
what is left of
[effect-nontail-resume-copies-held-until-outer-entry](../reported/effect-nontail-resume-copies-held-until-outer-entry.md)
(a `k` resumed outside its case body).  Reproduces on `main` before that
report's work; `tur --interpret` is right.

## Repro

```turmeric
(defeffect Ask [] int)
(defn helper [k : cont<int> v : int] : int (resume k v))
(defn step [i : int] : int
  (handle (+ i (perform (Ask))) (Ask [] k) (helper k 10)))
(defn main [] : nil
  (println (step 7)))
```

| | prints |
| --- | --- |
| `tur run` | 10 |
| `tur --interpret` | 17 |

## Root cause

`emit_effects_resume` (`src/compiler/emit_effects.c`, the direct emitter's
`EX_RESUME`) dispatches on the continuation's type: `CK_MULTISHOT` takes the
snapshot resume, `TY_INT` the fiber `tur_effect_cont_resume`, and anything
else -- here `TY_CONT`, the type `cont<int>` spells -- falls to the "Phase 18
path: return value directly", which emits the resume VALUE and nothing else:

```c
static int64_t helper(int64_t k, int64_t v) {
        return v;
}
```

The case body `(helper k 10)` is itself lowered correctly (a direct call
with the DK continuation as a word); it is the callee that drops it.  The
elaborator accepts `cont<int>` for a handler continuation passed on, so
nothing warns.

## Fix directions

1. In `emit_effects_resume`, lower a `TY_CONT` resume through the DK runtime
   as the CPS backend does for a case-local `k` -- `dk_invoke((DK *)k, v)` --
   when the continuation came from a DK handler case, which is every handler
   `k` the CPS backend installs.  The fiber path (`TY_INT`) predates DK and
   is what `tur_effect_cont_resume` serves; the two need telling apart by
   the value's provenance or by making a handler `k`'s type name the DK
   substrate.
2. Until then, refuse the shape: a `resume` whose continuation is `TY_CONT`
   and not `CK_MULTISHOT` should be a compile error naming this report,
   not a value pass-through.

Note that a stored handler continuation (`(set! saved k)` into a `^mut`,
then `(resume saved v)`) is NOT this shape: the cell is `int`-typed and
takes the `TY_INT` path, which runs the DK chain; both back ends print the
same there.
