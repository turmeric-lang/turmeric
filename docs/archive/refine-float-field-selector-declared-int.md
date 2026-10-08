# A `float` record field's selector was declared Int, making every VC with one contradictory

**Severity: high (soundness, default build, no experiment).** A refinement
obligation whose verification condition mentioned a `float` (or any
Real-sorted) record field could be "proved" whatever its goal, and its runtime
check was then elided. Found 2026-10-03 by the new `shape_reflect` of
`tests/refine-fuzz-src.py` (built for
[reflect-fuzz-never-reaches-the-rf3-rf4-encoder](reflect-fuzz-never-reaches-the-rf3-rf4-encoder.md)):
20 cases into its first run. Over `--n 400` at seeds 11 and 23 the unfixed
compiler gives **23 and 26 `BUG_soundness`**, every one a float list. Fixed
in the same change. **RESOLVED 2026-10-03.**

## Repro

```turmeric
(defdata FB (FB [a : float b : int]))

(defn f [p : int] : #refine{ r : int | (= r 7) }
  (let [x (FB 0.5 p)]
    3))

(defn main [] : int (println (f 1)) 0)
```

`refine: 1 obligation(s): 1 proven`. `f` returns 3, unchecked. The same
happens through a written hypothesis -- `(defn g [s : #refine{ v : SP | (= (.a
v) 0.5) } n : int] : #refine{ r : int | (= r 7) } n)` -- for a `defdata`
record or a `defstruct`. The fuzzer's shape was `(fz-len <a list of float
literals>)` "proved" equal to the wrong length.

## Root cause

`TUR_REFINE_DUMP=1` shows it in two lines:

```
(declare-fun .hd (Int) Int)
(assert (= (.hd (FzCons 0.5 FzNil)) 0.5))
```

A field selector `.f` resolved to nothing in `rt_resolve_fn`, so the encoder
treated it as an abstract measure with the default **Int** result sort,
whatever the field's type. Every constructor application in a return
obligation gets its record axioms (`rt_push_ctor_axioms`):
`(= (.f (C ... v ...)) v)`. For a field holding a non-integral float, that says
an integer equals 0.5, which is unsatisfiable. With unsatisfiable hypotheses,
every goal is valid, so the obligation proved and its check was elided. A
written `(= (.a v) 0.5)` reaches the same contradiction through the hypothesis
environment.

## Fix

`rt_resolve_fn` (`elab_fns.c`) answers a `.f` name that some record
constructor declares as a field: pure, with the field's sort. One sort per name
across every record with such a field: the field's own when they agree; Real
when Int and Real fields share the name (an integer is a valid Real, so this
forgoes only integer reasoning); and no answer when a Bool field shares it with
a number. In that last case the encoder refuses the mixed equality instead of
tightening it.

Fuzzer after the fix, `--only-shape reflect --n 400 --mode both`: seed 11
304 proven / 0 BUG; seed 23 316 proven / 0 BUG.

## Pinned by

- `tests/fixtures/errors/refine-float-field-selector-not-contradictory`: the
  repro, unknown under `--strict-refine` rather than proved.
- `tests/fixtures/refine-float-field-hypothesis-checked`: the hypothesis door.
  `g`'s check now fires, while an honest float-field goal and an Int field
  still run.
