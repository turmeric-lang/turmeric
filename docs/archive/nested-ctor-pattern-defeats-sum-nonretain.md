# A nested constructor pattern on an `(Option T)` parameter makes the function keep every box it is handed

**RESOLVED 2026-10-22** -- see [Resolution](#resolution-2026-10-22). Pinned by
`option-from-self-recursive-producer-freed` (`requires.leak-check`), whose
`show` is now the nested-pattern form beside its hand-flattened twin.

**Severity: low (leak; one box per call).** Found 2026-10-21 writing the
leak-checked fixture `option-from-self-recursive-producer-freed`.

A function that reads an `(Option T)` parameter and does not keep it lets
its caller free a fresh argument box after the call (the
`nonretain_sum_param_mask` drop-after stamp). Written with a NESTED
constructor pattern, the same function is treated as keeping the box, so
every caller leaks it:

```turmeric
(defdata Value :copy (LongVal :int) (StrVal :cstr))

;; leaks the argument's box at every call
(defn show [o : (Option Value)] : cstr
  (match o
    (Some (StrVal s))  s
    (Some (LongVal _)) "long"
    (None)             "none"))

;; the same function as two matches: the caller frees the box
(defn show2 [o : (Option Value)] : cstr
  (match o
    (Some v) (match v (StrVal s) s (LongVal _) "long")
    (None)   "none"))

(defn main [] : int
  (println (show (some (StrVal "x"))))   ;; 16 B leaked
  (println (show2 (some (StrVal "x"))))  ;; freed
  0)
```

Built with `TUR_CC_FLAGS="-O1 -g -fsanitize=address"` and run with
`ASAN_OPTIONS=detect_leaks=1`: the `show` call leaks 16 bytes from
`some__spec__tur_adt_Option__Value_tur_adt_Value`; the `show2` call does not.
The emitted `main` frees the argument after `show2` and emits nothing after
`show`.

## Root cause (partly traced)

The bit comes from `sum_param_is_nonretaining` (`src/compiler/emit_core.c`),
the `box_uses_confined` walk with the sum accessors admitted. The nested
pattern is desugared before that walk sees it: the emitted `show` binds the
`Some` payload to a synthetic local (`_un_unmp_un1_17`) and matches on it
again. Whatever node or binding that desugaring produces is a shape the walk
does not model, and its default is "escapes". `show2`'s explicit `(Some v)`
binder followed by `(match v ...)` is modelled.

## Fix directions

1. Find the node the nested-pattern desugaring emits (the synthetic payload
   binder and the inner match) and admit it in `box_uses_confined` the way
   an arm binder and a `match` on it are admitted.
2. Extend `option-from-self-recursive-producer-freed` (or a sibling
   `requires.leak-check` fixture) with the nested-pattern consumer.

## Resolution (2026-10-22)

The walk was never the problem: `box_uses_confined` models every node the
lowering produces. What it never reached is the gate in front of it. The
nested-pattern lowering (`elab_structs.c`) puts an unreachable
`(panic "match: nested pattern fallthrough (unreachable)")` at the end of
every exhaustive group's test chain, and `expr_subtree_has_inline_c`
(`emit_core.c`) -- the inline-C exclusion that guards the whole nonretain
inference in `elab_fns.c` -- left `EX_PANIC` to its conservative `default`,
"may hide inline C". So the inference was switched off for the body before
any walk ran, the mask stayed 0, and every caller kept its box.

`EX_PANIC` and `EX_PANIC_WITH` now walk only their payload: a panic lowers to
the preamble's `tur_panic_at` and carries no inline C of its own. With that
the nested-pattern `show` is stamped non-retaining like its flattened twin,
and the fixture is clean under LeakSanitizer with both consumers.
