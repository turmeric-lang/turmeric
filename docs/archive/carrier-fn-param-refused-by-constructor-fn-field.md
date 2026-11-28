# A function parameter with a scalar signature cannot be stored in a closure field

> **RESOLVED 2026-11-28.** The argument check in `elab_call_fn_inner`
> (`src/compiler/elab_call.c`) now boxes a poly-carrier fn parameter, or a
> `let` alias of one, handed to a constructor's boxed `(fn ...)` field. It
> builds the same EX_POLY_TO_FAT the constructor post-pass builds for
> `make-struct`, typed with the field's declared signature from its `CtorDef`
> (a constructor binding has no `arg_full_types`), and only when the
> signatures match (`fn_sig_kinds_match`). A mismatch is still TUR-E0001, and
> the message now names both types:
> `expected (fn [int] : int), got (fn [int] : float)`.
>
> Ownership (fix direction 2): `fnfield_closure_owned` (`elab_forms.c`) counts
> an EX_POLY_TO_FAT as owned. The box is fresh and has a NULL drop header, so
> dropping it frees the box and leaves the caller's closure alone. That also
> stopped a leak the parent fix had added to `make-struct`'s spelling of this
> shape: `fn-param-stored-in-struct-field` is back at `main`'s 88 B.
>
> Not covered: a carrier value that is not a variable, such as
> `(H 1 (if c f g))`. The `if` is typed `ptr<void>` and is still refused.
> That gap is wider than this shape, and some of it segfaults. It is open as
> [fn-value-through-if-or-match-loses-its-representation](../reported/fn-value-through-if-or-match-loses-its-representation.md).
> Pinned by `carrier-fn-param-in-ctor-fn-field` and
> `errors/carrier-fn-param-ctor-field-sig-mismatch`.

**Severity: medium (false rejection, misleading diagnostic).** A function
parameter whose type has only scalar arguments and results, such as
`(fn [int] int)`, cannot be passed to a struct constructor's closure field. The
constructor refuses it with TUR-E0001, and the message does not name the
types:

```
error [TUR-E0001]: function 'H' arg 2: expected (fn [] : ?), got ptr<void>
```

The same parameter is accepted when it is passed to another function's
`(fn [int] int)` parameter. A constructor function that takes the closure to
store is an ordinary shape, so this is easy to hit. Found 2026-11-28 while
writing fixtures for
[fnfield-struct-binder-frees-borrowed-closure](fnfield-struct-binder-frees-borrowed-closure.md).
Reproduces on `main` at `318aaaf2`, before that change.

## Repro

```turmeric
(defstruct H [raw : int handler : (fn [int] int)])
(defn mkp [f : (fn [int] int)] : H
  (H 1 f))
(defn main [] : int
  (.raw (mkp (fn [x : int] : int x))))
```

`tur check` reports the error above at the `f` in `(H 1 f)`. It does not
matter whether the result is returned, let-bound, `^copy` or move-only, and it
happens with any scalar signature: `(fn [float] int)`, `(fn [int int] int)`,
`(fn [cstr] int)`, `(fn [int] cstr)`.

Each of these compiles:

| Variant | Why it works |
| --- | --- |
| `(fn [Ev] int)`, where `Ev` is a `defdata` | the signature is not scalar, so the parameter stays a TY_FN |
| `[^fat f : (fn [int] int)]` | a `^fat` parameter stays a TY_FN |
| `(make-struct H 1 f)` | goes through a different path |
| `(H 1 (fn [x : int] : int (f x)))` | the argument is a new lambda |
| `(take f)`, where `take` has a `(fn [int] int)` parameter | that parameter is on the same carrier, so both sides are `ptr<void>` |

## Root cause

A plain function parameter with a concrete signature, no effects and only
scalar types is routed onto the poly-closure carrier: `tur_poly_fn_t {env, fn}`,
binding type `TYPE_PTR_VOID`, with `is_poly_fn` set
(`repr_of_fn_param(pb, ann) == REPR_CARRIER_I64`,
`src/compiler/elab_fns.c:11397`). `--emit-abi-trace` prints
`fn-param f carrier` for the repro, and `fn-param f thin-fn non-scalar-sig`
for the `Ev` variant.

A constructor's closure field is a boxed TY_FN slot. The argument check in
`elab_call_fn_inner` (`src/compiler/elab_call.c`) converts a carrier parameter
into a fat handle (EX_POLY_TO_FAT) for one kind of slot only, a type
variable. That conversion was added by fn-param-at-type-variable-slot, at
`elab_call.c:9707`:

```c
if (expected_arg_kind == TY_TYVAR && args[i]->type.kind == TY_PTR_VOID) {
```

For a TY_FN slot nothing converts the parameter, so the check
`args[i]->type.kind == expected_arg_kind` fails: `ptr<void>` against `fn`. The
diagnostic then prints the slot from its kind, so it reads `(fn [] : ?)`
instead of `(fn [int] int)`.

## Fix directions

1. Widen the conversion at `elab_call.c:9707` to a TY_FN slot whose declared
   type (`arg_full_types`) is boxed and whose signature matches the
   parameter's `poly_type` (`fn_sig_kinds_match`). Build the same
   EX_POLY_TO_FAT, typed with the boxed slot type. A `^fat` parameter is
   already a fat handle at runtime, so this gives the field the
   representation it takes from `^fat`.
2. Ownership: the fat box EX_POLY_TO_FAT allocates is fresh. If the
   constructed struct is let-bound and stays in scope, the closure-field drop
   frees that box. `fnfield_closure_owned` (`elab_forms.c`) peels
   EX_POLY_TO_FAT and then sees a parameter, so it answers "not owned" and the
   box leaks. Count the shim box as owned and leave the caller's closure
   inside it alone, or let the shim borrow it. Pin whichever you choose under
   `tests/run-leak-check.sh`.
3. Print the slot's full type in the diagnostic. The TY_FN arm of the
   `expected_ty` lookup a few lines below the check should already find it in
   `arg_full_types`; work out why it prints `(fn [] : ?)`.

## Workaround

Mark the parameter `^fat`, or build the struct with `make-struct`.

## Related

- [fn-param-at-type-variable-slot-boxed-per-call](../reported/fn-param-at-type-variable-slot-boxed-per-call.md):
  the same carrier parameter passed to a type-variable slot. That is where
  the conversion this report needs comes from.
