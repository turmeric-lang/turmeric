# A `ptr<Foo>` parameter accepts a `ptr<Bar>`: the pointee is not checked

**Severity: medium (silent loss of type checking).** `type_eq` treats two
typed pointers as equal only when their pointee types are equal
(`src/compiler/types.c:278-284`). Only the untyped `ptr<void>` is meant to
match any pointer. At a call, though, a `ptr<Foo>` parameter accepts a
`ptr<Bar>` argument with no diagnostic. CLAUDE.md recommends `:ptr<TheStruct>`
as a typed handle, and today that gives no more protection than `ptr<void>`.
Filed 2026-10-17 while correcting CLAUDE.md's type spellings.

## Repro

Measured on the v0.65.5 release binary with `tur check`:

```turmeric
(defstruct Foo [a : int])
(defstruct Bar [b : int])
(defn mk-bar [] : ptr<Bar>
  ```c
  return 0;
  ```)
(defn f [x : ptr<Foo>] : int 0)
(defn main [] : int
  (println (f (mk-bar)))   ; accepted, exit 0
  0)
```

The same result in every position tried:

| Shape | Result |
| --- | --- |
| `[x : ptr<Foo>]` given a `ptr<Bar>` | accepted |
| `[x :ptr<Foo>]` (unspaced) given a `ptr<Bar>` | accepted |
| `: ptr<Foo>` return type whose body returns a `ptr<Bar>` | accepted |
| `[x : ptr<int>]` given a `cstr` | refused, but the message reads `expected ptr<void>, got cstr` |

That last row is a lead. By the time the argument check runs, the parameter's
pointee seems to be gone, and the message prints the untyped spelling. The
pointer-ness of the argument is still checked.

## Root cause

Not located. `ptr_type_from_keyword_name` (`src/compiler/elab_types.c:468-527`)
does build a typed pointer with its pointee, and `type_eq` compares it. So the
loss is somewhere between the signature and the call-site comparison. The
argument checker's pointer handling in `src/compiler/elab_call.c` (for example
around `:1660-1690` and `:1912`) is the first place to look, along with
whatever records a defn parameter's type for later calls. There is no negative
fixture for the rule: `tests/fixtures/errors/` has none for a pointee
mismatch.

## Fix directions

1. Find where the pointee is dropped and keep it. Then add
   `tests/fixtures/errors/typed-ptr-pointee-mismatch/` with the repro above.
2. Run the corpus. Code that passes a typed pointer where a different one is
   declared has been compiling, so expect some real mismatches to surface.
3. Until then, CLAUDE.md steers new handles to `defopaque`, which is checked.

## Note (2026-10-21): `rc<T>` has the same hole

An `rc<U>` argument is accepted for an `rc<S>` parameter, with the callee
defined before or after the call (`(defn b [^borrow st : rc<S> k : int] ...)`
called as `(b (rc/of (U 7)) 3)` passes `tur check`). The call check
(`src/compiler/elab_call.c`, `arg_ok = (args[i]->type.kind == expected_arg_kind)`)
compares kinds, and only re-checks the full type for a struct, ADT, session or
role parameter. Found closing
[forward-call-with-rc-argument-rejected](../archive/forward-call-with-rc-argument-rejected.md).

## Resolution (2026-10-21)

The pointee was never dropped: a defn's `arg_full_types` carries `ptr<Foo>`
in full. The call check simply never asked -- it matched pointer kinds, and
re-checked the full type only for a struct, ADT, session or role parameter.

- **Calls** (`src/compiler/elab_call.c`): after the kind match, a `ptr<T>`,
  `rc<T>` or `weak<T>` parameter is compared with `handle_targets_conflict`.
  Two typed pointers conflict when type_eq tells their pointees apart; an
  untyped `ptr<void>` on either side, or a pointee that is still open
  (`ptr<A>` in a generic -- `ptr_pointee_is_open`), never does, so raw
  `raw-malloc` handles and generic pointer helpers are unaffected. For
  `rc` / `weak`, whose type_eq compares only the target KIND (and the call's
  kind check not even that), two different target kinds (`rc<int>` /
  `rc<float>`) or two different ADT defs (not merely two copies of one name)
  conflict; a type-variable or unannotated (`rc`, read `rc<?>`) target
  matches anything. The diagnostic
  prints the declared type: `expected ptr<Foo>, got ptr<Bar>`, and
  `rc<S>` / `rc<U>` instead of `rc<<adt>>` twice (`src/compiler/types.c`
  names an rc/weak's ADT now).
- **Forward calls** (`fwd_decl_apply_ref_params`, `src/compiler/elab_core.c`):
  a callee defined below the call gets its typed-pointer parameters in the
  forward declaration, as `rc<S>` ones already did; a pointee resolved to a
  scalar or a registered non-generic ADT (through `ptr<ptr<...>>` and
  `const-`) settles, anything else retries as before.
- **Returns** (`src/compiler/elab_fns.c`): a `: ptr<Foo>` (or `: rc<S>`)
  return whose body yields a different target is `RET_CONFLICT_NOMINAL`,
  `function 'g' declares return type 'ptr<Foo>' but its body returns
  ptr<Bar>`.

The corpus ran clean: no fixture passed one typed pointer for another. The
only movement was three `errors/hrt-*` diagnostics, whose rank-2 parameter
has the pointer kind and now prints as `(forall [a] (fn [a] : a))` rather
than `ptr<void>`.

Pinned by `errors/typed-ptr-pointee-mismatch`, `-forward`, `-return`,
`errors/rc-target-mismatch` and `errors/rc-target-kind-mismatch`. CLAUDE.md no longer steers handles away from
`ptr<TheStruct>` on this report's account.

**Addendum (2026-10-21, later).** A `let` annotation is checked by the same
predicate: `[x : ptr<Foo> (mk-bar)]` and `[r : rc<S> (rc/of (U 7))]` are
`let binding 'x': type annotation does not match initializer` (pinned by
`errors/typed-ptr-let-annotation-mismatch` and `errors/rc-let-annotation-mismatch`;
`typed-ptr-pointee-controls` is the positive control). A call through a
let-bound lambda is still unchecked:
[typed-ptr-pointee-not-checked-through-closures](typed-ptr-pointee-not-checked-through-closures.md).

