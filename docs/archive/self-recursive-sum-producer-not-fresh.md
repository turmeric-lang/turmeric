# A self-recursive function returning a fresh `(Option T)` was not a fresh producer

**RESOLVED 2026-10-21** -- found and fixed in the same change, leak-checking
the typed `examples/datalog/blog.tur`. Pinned by
`option-from-self-recursive-producer-freed` (`requires.leak-check`). The
argument-side twin is
[fresh-option-arg-to-self-call-leaks](fresh-option-arg-to-self-call-leaks.md).

## The bug

```turmeric
(defn get-attr-from [db : Db entity : int attr : cstr i : int] : (Option Value)
  (if (>= i (db-count db))
    (none)
    (let [d (db-ref db i)]
      (if (matches? d) (some (.value d)) (get-attr-from db entity attr (+ i 1))))))
```

A caller that matches on such a function's result frees the box only when
the callee is a fresh producer (`returns_fresh_sum_box`): every value path
mints a box. The freshness walk (`fresh_sum_walk`, `src/compiler/elab_fns.c`)
reads the callee's flag at each call -- including the function's own
self-call, before the flag was set -- so a self-recursive producer was never
fresh, and every caller leaked one box per call (20 boxes in `blog.tur`'s
demo).

## The fix

`elab_stamp_sum_freshness` decides a body as a greatest fixed point: it walks
the body with the function assumed unconditionally fresh, and keeps the answer
only if the walk confirms it unconditionally; otherwise it decides again
without the assumption, as before. Sound because a value the function returns
comes out of a finite chain of self-calls ending in a non-self path, and the
walk checked each such path mints a fresh box. A function that returns its
parameter on some path stays not fresh.

The fixture's consumer is written `(Some v) (match v ...)`: a nested
`(Some (StrVal s))` pattern defeats the separate non-retain inference, open as
[nested-ctor-pattern-defeats-sum-nonretain](../reported/nested-ctor-pattern-defeats-sum-nonretain.md).
