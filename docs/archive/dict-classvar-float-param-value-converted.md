# A float reached a rank-2 dictionary method as the wrong number

**Severity: high.** A silent wrong answer:

```turmeric
(defclass Sh1 [a] (sh1 [x : a] : cstr))
(definstance Sh1 [float] (sh1 [x : float] : cstr (if (> x 7.0) "big1" "small1")))
;; a van Laarhoven lens over `Sh1`, applied at s : float
(shown1-float sh1-lens 7.1)        ; compiled: small1   interpreter: big1
```

Found 2026-10-01 while sweeping the `-fsanitize=function` traps
(`docs/archive/emitted-c-indirect-calls-are-not-type-exact.md`): the
`Show bool` dictionary slot trapped, and the same mechanism with a `double`
in the slot, which no fixture had, is a wrong answer.
**RESOLVED 2026-10-01.**

## Two defects on one path

1. **The carrier call value-converted the argument.** The generic
   `tur_poly_fn_t` dispatch (`emit_expr.c`) spelled every argument
   `(int64_t)(v)`, so 7.1 became 7 before any instance saw it. The emitted-C
   float lint flags it (`F2I`). The corpus had no rank-2 call at a float, so it
   never ran on this shape.
2. **The dictionary slot held the raw instance method.** A class parameter that
   is the class variable (`(sh [x : a])`, or an unannotated `(show [x])`) is
   erased to the int64 word at every dict-passing dispatch site, which cannot
   know the instance. The slot held `sh1(double)`, called through
   `(const char *(*)(int64_t))`. The word went in an integer register and the
   method read a float register. With several instances the cast followed a
   "representative" instance's types (`const char *` here), so the
   three-instance program answered right by accident: the float register
   still held 7.1.

## Fix

- The carrier call puts each argument in as its word: bits for a float kind,
  through `intptr_t` for a pointer (`poly_carrier_arg_word`, built on
  `emit_word_slot_bits`).
- `dict_slot_param_is_word_scalar` (`emit_stmt.c`) answers whether a parameter
  is the class variable while the instance takes it as a non-word scalar
  (`double`, `float`, `bool`, the narrow ints, a pointer). For such a
  parameter the slot holds a per-instance `__dictwrap_*` that takes the word
  and converts it, as D8 piece 2 already did for a by-value aggregate. The dict
  struct field is spelled to match, and the dispatch site (`emit_call_name`)
  asks the same question, so every instance's slot has one signature.

## Verified

`tests/fixtures/dict-classvar-float-param-value-converted`, compiled and
`--interpret`. The single-instance lines fail on the baseline compiler.
