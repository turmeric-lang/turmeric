# A `resume` of a `cont<T>`-typed parameter compiles to its argument: the continuation never runs

**Severity: high (silent wrong answer), narrow.** Found 2026-10-12 probing
what is left of
[effect-nontail-resume-copies-held-until-outer-entry](effect-nontail-resume-copies-held-until-outer-entry.md)
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
