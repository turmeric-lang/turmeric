# A by-value ADT local's spine is not released when the exit could carry a pointer into it

**Severity:** low (a leak, linear in iterations). Both common faces are fixed;
what is open is the loop whose signature is not all numbers.

**Status:** partially fixed. Face i (tail self-call) was fixed on `main`
independently, by the inline tail arm's `let_binding_push_scope_frees`. Face ii
(an explicit `return`) was fixed 2026-10-21, ported from the abandoned branch
`claude/non-macos-windows-reports-6pz2pr` (2026-09-18), which found both.
Residue 1 is open.

## The two faces

`drop_localowned_<T>(&xs)` is a TRAILING free, emitted after a `let` body. Two
exits never reach it:

- **Face i, a tail self-call.** `emit_tail` emits a tail-position `let` inline
  with no trailing code. Fixed: its releases ride the early-exit channel
  (`any_scope_drops`), which the back-edge fires, and `tco_let_refusal` keeps a
  binding that is used as more than a plain number off that path.
- **Face ii, an explicit `return`** -- 2 boxes per call, every call:

  ```turmeric
  (defdata Rec [] (RCons [hd : int tl : Rec]) (RNil))
  (defn once [i : int] : int
    (let [xs (RCons i (RCons 2 (RNil)))]
      (return (match xs (RCons h t) h (RNil) 0))))
  ```

  Two causes. The elaborator's confinement walk (`box_uses_confined`, strict
  mode) had no `EX_RETURN` arm, so a body containing a `return` read as
  escaping and `xs` was never flagged `drops_local_owned`. And
  `emit_let_value` collected the spine drop only for bodies with no
  `return`. Fixed: the walk treats a `return`'s value as an unconfined result
  (so `xs`, or a binder aliasing it, is still refused there), and the drop is
  collected unconditionally, emitted on the fall-through path, and pushed onto
  the channel -- which the `return` fires after hoisting its value -- when
  `EmitCtx.current_fn_spine_drop_safe` holds: every parameter and the result a
  non-pointer scalar. That flag is reset wherever a nested C function body is
  emitted (handler fns, thunks, generators, CPS terms, fused tail-call groups).

Pinned by `tests/fixtures/byval-spine-drop-past-early-exit` (both faces, both
glue paths -- a recursive field and an `:any` field -- leak-clean).

## Open: residue 1, the non-scalar signature

```turmeric
#lang saffron
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn drive [i : int n : int carry] : int
  (if (= i n)
    (cast (match carry (Cons h t) h (Nil) 0) int)
    (let [xs (Cons i (Cons 7 (Nil)))]
      (drive (+ i 1) n (match xs (Cons h t) t (Nil) xs)))))
```

leaks `xs`'s spine every turn (100 allocations over 50 turns). That refusal is
correct as far as it goes: `carry` receives `t`, a pointer into this turn's
spine, so freeing the spine at the back-edge would be a use-after-free on the
next turn. But a loop that passes the parameter something UNRELATED to the
spine is refused the same way, because neither gate can tell the two apart.

`tests/fixtures/byval-spine-drop-refused-nonscalar-param` pins the refusal and
carries `known-leak` naming this report; a wrongly admitted free would show
there as a use-after-free, which the marker does not excuse.

Fix direction: ask the confinement walk about the back-edge ARGUMENTS rather
than the parameter types -- an argument that is `xs` or a tracked alias of it
refuses, anything else admits -- and replace the signature gate with that
answer. The walk already tracks match binders as aliases in strict mode, which
is the part that makes this possible.
