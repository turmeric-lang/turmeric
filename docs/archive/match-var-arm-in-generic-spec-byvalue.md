# A catch-all variable arm in a generic's match was invalid C at every spec

**Severity: medium** -- a hard cc error, nothing ships broken.  **Status:
RESOLVED 2026-10-02**, same day as filed.  Found while probing forward calls to
generic callees: a bare `None` (no parens) is a VARIABLE pattern, which is how
it surfaced.

## Repro

```turmeric
(defn get-or [A] [o : (Option A) d : A] : A
  (match o
    (Some v) v
    other d))

(defn main [] : int (println (get-or (some 7.1) 1.5)) 0)
```

```
error: incompatible types when initializing type 'int64_t' using type
'tur_adt_Option__float'
    int64_t other_7 = *__scrut;
```

At `A := int` too.  The same arm in a non-generic defn compiled.

## Mechanism and fix

A variable catch-all binds the whole scrutinee.  Both match emitters
(`src/compiler/emit_expr.c`, the if-chain path and the switch path) declared
the binder at `type_c_name(binding->type)`.  Inside a spec that type is still
the declared `(Option A)`, spelled as the int64 carrier, while `__scrut` is the
monomorph's by-value aggregate.  A by-value (or pass-by-pointer) binder is now
declared at the scrutinee's own C type and recorded as such for the readers.

Pinned by `tests/fixtures/match-var-arm-in-generic-spec-byvalue` (switch path,
and the if-chain path through a guard; the binder returned as the result).
