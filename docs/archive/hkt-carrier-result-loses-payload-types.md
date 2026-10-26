# A typed `fmap` on a heterogeneous Result/Either prints a cstr payload as a raw word

**RESOLVED 2026-09-17.** Root cause as filed -- the carrier-path dispatch result
never grounded -- and the fix is the report's own location, with one change of
address in each half:

- **Elaborator** (`src/compiler/elab_typeclasses.c`): the result type is not
  recovered through the instance binding's `rft` at all, which is where T4's
  hole erasure swaps the arms. The class method's receiver IS `(f a)` and its
  result IS `(f b)`, and the head fixes every slot of `f` but the hole, so
  `(f b)` is the RECEIVER with its hole slot replaced by `b`
  (`m7_app_replace_slot`; slot 0 for a hole-at-0 head, the outermost slot for a
  leftmost partial head like `(Either E)`). `b` is read from the function
  parameter that states it: a bare `b` result (`fmap`) is the closure's result
  itself, and an `(f b)` result (`bind`'s continuation) is the closure's result
  with the same hole slot read out of it -- the class-variable unification of
  `(f b)` against `(Result int cstr)` binds `b` to the OUTERMOST argument, the
  same T4 reversal, so `cstr-err-r` in `result-monad-nested-bind-typed-boundary`
  briefly came out as `(Result cstr cstr)` before that read was made hole-aware
  too. Committed only as a carrier-path refinement, and only for a STATIC
  dispatch (receiver head a concrete ADT) --
  `m7_byvalue_grounded` stays false, no by-value spec is minted, the receiver
  stays wherever the M2 gates left it -- on the same three representation
  terms as the `byval_agg` arm.
- **Emitter** (`src/compiler/emit_expr.c`, the call hoist): rather than
  teaching the `byval_agg` consumer bridge every consumer position one at a
  time (let-init was the one the Either attempt hit; the match scrutinee, an
  argument, a return and a tail are the others), the carrier box is bridged
  into the aggregate AT PRODUCTION, the same way the `any` straddle already
  is. Scoped to a dictionary dispatch through a Turmeric-bodied callee whose
  declared result is the generic `(f b)` and whose emitted return is the
  carrier word, refined to a concrete non-heap non-niche by-value ADT app.
  The box is freed when the producer is fresh and no drain owns it; a
  borrowed box is copied out and left alone.

Both repros print `bad` / `x`; the ok / Left arms still print `42` / `7`; and
the result flowing straight into a consumer with no `let` between works too.
Pinned by `tests/fixtures/hkt-carrier-result-payload-types`, leak-checked.
The hoist frees the fresh Result box. The Either path first showed a second,
older leak -- the poly-to-fat closure adapter the instance body mallocs to
hand its method closure to `either-map`'s `^fat` parameter, 32 bytes per
call, nothing freeing it -- which was closed in the same change the way the
bare-fn shim already was: the argument-position `EX_POLY_TO_FAT` now carries
the sink's non-retaining proof (`stack_ok`, from the inferred mask or a
declared `^borrow`) and the emitter gives such a box the call's own stack
frame. `docs/guides/typeclass-guide.md`'s holes section now says what the
two routes are and that the result type is precise on both.

**Severity: high** -- a silent wrong answer on the TYPED path, no diagnostic,
no panic. Found 2026-09-10 while fixing `saffron-dynamic-surface-pass` M2, as a
baseline measurement of code the M2 change deliberately leaves untouched.

## Repro

```turmeric
(defn main [] : int
  (let [e : (Result int cstr) (err "bad")
        n                     (fmap e (fn [x : int] : int (* x 2)))]
    (match n (Ok v) (println v) (Err s) (println s))
    0))
```

Prints a number of the shape `94884486443026` -- the string's address -- where
`bad` is expected. The `(ok 21)` arm of the same program prints `42`
correctly, because an int payload IS an int64.

Either shows the same thing on its varying arm:

```turmeric
(load "stdlib/either.tur")
(defn main [] : int
  (let [e : (Either int cstr) (Right "x")
        m                     (fmap e (fn [s : cstr] : cstr s))]
    (match m (Left a) (println a) (Right b) (println b))
    0))
```

Prints `94257618870290`, not `x`. `(Left 7)` prints `7`.

## Root cause (measured)

Both instances are partially-applied heads -- `Functor [(Result _ B)]`,
`Functor [(Either E)]` -- and both `fmap` calls ride the erased carrier
`__inst_Functor_fmap_<X>_tyvar`. On that path the dispatch RESULT TYPE never
grounds: it stays the def-less `(type-app ? ?)` shell, so the `match` on it has
no payload types and every arm binder defaults to the int carrier. The `Err s`
/ `Right b` binder is therefore an `int64_t`, and `println` prints the pointer.

The un-grounding is the same missing binding M2 was about: the instance's own
head tyvar (`B` / `E`) is not a class variable, so the class-method unification
never binds it and `m7_byvalue_grounded` stays false. M2's fix binds it, but
only where doing so is slot-safe -- a by-value-bodied method on a HOMOGENEOUS
receiver (the Saffron all-`any` case), because T4's hole erasure reversed the
class's reading of `(f a)` for a hole-at-0 head (`a := cstr`, the err arm, on
`(Result int cstr)` -- measured) and reconstructs `(f b)` with the arms
swapped. A heterogeneous receiver is exactly the case that cannot be grounded
by value without a hole-preserving representation of the class-variable
binding, so it deliberately stays on the carrier, where this bug lives.

Either is a different sub-case: its `fmap` body delegates to `either-map`, so
it is not by-value-expressible (`m7_body_byvalue_ok == 0`) and can never take
the by-value route regardless of the receiver.

## Why the obvious fix is not safe

There IS an arm for "ground the precise result type for the CONSUMER while the
producer stays on the carrier" -- the `byval_agg` branch in the M7 dispatch
code (`method-result-functor-inference`), which commits `result_type =
substituted` without minting a spec and relies on the consumer to bridge the
carrier to the aggregate. Binding Either's `E` un-gated reached that arm and
produced `tur_adt_Either__int__cstr m = __ps_N;` -- an invalid initializer,
because the bridge does not cover a let-init. So that arm's consumer-side
bridge is incomplete, and that is the real fix location for this bug: make it
cover every consumer position (let-init included), then ground the head tyvar
for the carrier-bodied and heterogeneous cases so the consumer sees real
payload types.

Do NOT fix this by grounding heterogeneous hole-at-0 heads by value: the slot
swap turns the printed-address bug into a miscompiled body (`g` called on the
err arm).

## Guides to update when fixed

- `docs/guides/typeclass-guide.md` -- the HKT instance section should say that
  a partially-applied instance head is on the carrier path and what that
  implies for payload types, until this is fixed.
