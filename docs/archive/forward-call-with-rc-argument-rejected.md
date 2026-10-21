# A call to a later `defn` that takes an `rc<T>` is rejected as "expected int"

**Severity: low-medium (expressiveness).** A function can't call another one
defined later in the file if the callee takes an `rc<T>`. The call fails with
`TUR-E0001: expected int, got rc<<adt>>`. Mutual recursion through an
`rc`-taking pair can't be written at all. The same forward call with a struct,
ADT or `(Vec T)` parameter compiles.

Found 2026-10-16 writing the CommonMark block parser (markdown-library-plan
M1). It threads its state as a `^borrow st : rc<MdSt>` through about 100
functions, so `stdlib/markdown/block.tur` defines every callee before its
callers. `stdlib/markdown/html.tur` folds the renderer's recursion into one
self-recursive function.

## Repro

```turmeric
(defstruct S [n : int])
(defn a [^borrow st : rc<S> k : int] : int (if (= k 0) (.n st) (b st (- k 1))))
(defn b [^borrow st : rc<S> k : int] : int (a st k))
(defn main [] : int (let [st (rc/of (S 7))] (do (println (a st 3)) 0)))
```

```
error [TUR-E0001]: function 'b' arg 1: expected int, got rc<<adt>>
```

With `[s : S ...]` in place of the `rc<S>` parameters, the pair compiles and
runs.

## Root cause

- **The forward declaration types the parameter as `int`.**
  `fwd_decl_scan_params` (`src/compiler/elab_core.c:170`) commits only kinds a
  bare type name determines. Any compound annotation, `rc<S>` included, is left
  as the `TY_INT` placeholder (`elab_core.c:228`, `:231`).
- **The call check has no `rc` exemption.** It accepts a struct, an ADT, a
  session or a type application where that placeholder stands
  (`src/compiler/elab_call.c:8564` and the hatches around it), because each
  is one machine word at runtime. There is no such hatch for `TY_RC`, so the
  forward call is checked against `int`.

## Fix directions

1. **Forward-declare the parameter's real type.** Resolve `rc<T>`, `weak<T>`
   and `^borrow` in the pre-pass. That covers ownership too: whether the
   argument is borrowed or moved at the call depends on the callee's real
   signature, which the placeholder hides.
2. **Add a `TY_RC` hatch** beside the `TY_ADT` one. It would be cheaper but
   unsafe: a call checked against an `int` parameter cannot know the callee
   borrows, so the caller may treat the rc as moved, or skip an increment it
   owes. Direction 1 avoids this.

## Resolution (2026-10-21)

Fix direction 1: the forward declaration carries the parameter's real type.

- **Fix.** `fwd_decl_apply_ref_params` (`src/compiler/elab_core.c`) runs beside
  `fwd_decl_apply_variadic` at all three forward-decl sites: the top-level
  pre-pass (`elab_toplevel.c`), the defmodule pre-pass (`elab_module.c`) and
  the letrec placeholder (`elab_forms.c`). For an `rc<S>`, `weak<S>`, `ref<S>`
  or `lref<S>` annotation whose pointee a bare name settles -- a scalar, or a
  registered non-generic ADT or struct -- it commits the slot's kind and full
  type through `rc_family_type_from_keyword_name`, the resolver the real
  signature uses. It also sets `FA_BORROW` on every `^borrow` slot, so a
  borrowed argument is lent at a forward call exactly as at a backward one
  (the move checker reads the flag; the placeholder had none).
- **Modules.** A module has no RF0 type pre-pass, so its own `defstruct S` is
  not registered when its forward decls are made. Such a slot is recorded on
  the existing pending list (`elab_fwd_note_pending_params`, beside
  `elab_fwd_note_pending_result`) and retried at the start of every defn, by
  which point a struct written above its first user is registered.
- **Not covered.** `rc<A>` over one of the defn's own type parameters keeps
  the placeholder, as before.
- **Same check as a backward call, holes included.** A forward call is now
  checked exactly as a call to an already-defined callee. That includes one
  gap the two share: an `rc<U>` argument is accepted for an `rc<S>` parameter
  in both directions, because the call check compares kinds. Noted in
  [typed-ptr-pointee-not-checked-at-calls](typed-ptr-pointee-not-checked-at-calls.md) (resolved 2026-10-21),
  which is the same hole for `ptr<T>`.
- **Pinned** by `forward-call-rc-param` (borrowed mutual recursion, an owning
  rc parameter, an `rc<int>`, a letrec lambda) and
  `forward-call-rc-param-in-module`, both engines. The borrowed case's
  compiled program is clean under ASan/LSan: `main` keeps ownership and drops
  the rc once.
