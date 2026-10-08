# `tests/errors/defsystem-set-undeclared` passes for the wrong reason

> **RESOLVED 2026-08-20 -- option 4: a new `defsystem-world` macro.**
>
> The report offered three options and called it the owner's decision. The
> chosen resolution is a fourth: `defsystem-world`, a NEW macro in
> `ecs/system` alongside an unchanged `defsystem`, lowering exactly as
> `sized-defsystem-scheduled` does -- typed impl `[^borrow w : World]`, an
> int-carrier wrapper that calls `load-<World>`, and the `System` value.
> Cap-binding and auto-consume carry over unchanged.
>
> This restores the capability (option 3's benefit) without option 3's
> breaking macro-signature change, and follows the precedent set by
> `defworld-classes` three weeks earlier: add the macro, do not change the
> one every caller already uses.
>
> One premise in "Options" is worth correcting for the record. Option 3 was
> described as "the only option that restores the capability", which is true
> as far as it goes, but the report did not note that **no in-repo caller of
> `defsystem` passes a world at all** -- `stage-pair.tur` and `stage-wave.tur`
> hand it `(:: accum :int)` and reinterpret the carrier in the body. So the
> capability being restored had zero existing users to break, which is what
> makes the additive route strictly better rather than merely safer.
>
> - `defsystem-world` + docstring: `spices/ecs/src/ecs/system.tur`
> - Positive test: `spices/ecs/tests/defsystem-world.tur` (suite 72/72)
> - The fixture now reaches its subject: `errors/defsystem-set-undeclared.tur`
>   fails with **TUR-E0003 `unbound symbol 'Vel-write-cap'`** at the
>   `set-Vel!` call, not TUR-E0295 at a `::` bridge.
>
> The "Adjacent observation" section at the bottom is **also resolved** --
> see the note there.

**Severity:** medium (a negative test that no longer tests its subject; the
guarantee it names is still covered elsewhere, so nothing is unprotected --
but the file asserts something it cannot reach)
**Found:** 2026-08-18, while resolving turmeric's
`docs/archive/ecs-defsystem-writes-fixture-expects-old-spices.md`, which
pinned the same shape from the compiler side.

## Summary

`spices/ecs/tests/errors/defsystem-set-undeclared.tur` exists to prove that a
system declaring `:writes [Pos]` cannot write `Vel` through the cap-gated
`set-Vel!` accessor -- its header calls this "the spec'd *writes to a
component you didn't list is a compile-time error* promise".

It does still fail to compile, so it passes. But it fails for an unrelated
reason and never reaches the capability check:

```
tests/errors/defsystem-set-undeclared.tur:37:24:
error [TUR-E0295]: cannot reinterpret by-value aggregate 'GameWorld' as a
one-word carrier (:int / :ptr<void>); it is a C struct with no int64 handle.
```

That is line 37 -- the `(:: w GameWorld)` bridge -- not line 39, the
`set-Vel!` call the test is about.

It is the **only** ecs negative test in this state. Every other one produces
a diagnostic topical to its subject:

| test | diagnostic |
|---|---|
| cap-double-use | TUR-E0101 |
| cap-mint-wrong-component | TUR-E0001 |
| **defsystem-set-undeclared** | **TUR-E0295** (off-subject) |
| defsystem-undeclared-write | TUR-E0003 |
| frozen-despawn-in-region | TUR-E0200 |
| refined-world-sealed-alias | TUR-E0302 |
| set-without-cap / set-wrong-component | TUR-E0001 |
| sized-defsystem-undeclared-write | TUR-E0003 |
| xworld-undeclared-write | TUR-E0003 |

## Root cause

`defsystem` expands to

```turmeric
(defn <name>-impl [w : int] : nil ...)
```

and takes **no world-type argument**, so `w` is an untyped int. The
accessors `defcomponent-accessors` generates take `^borrow w : GameWorld` --
a by-value struct. `(:: w GameWorld)` used to bridge those when the world was
int-carried; since the world became a by-value aggregate that reinterpretation
is rejected, and a plain `defsystem` body now has **no way to reach its own
world**.

This is structural rather than an oversight in the test: the macro cannot
type `w`, because it is never told which world the system runs against.
`sized-defsystem` *is* told (`(sized-defsystem rogue GameWorld ...)`), binds
`w` at that type, and reaches the accessors normally -- which is why
`tests/errors/sized-defsystem-undeclared-write.tur` still produces the
intended `TUR-E0003` at its `set-Vel!` call.

## Coverage is not lost

The cap-gating guarantee itself is still genuinely tested, three ways, all
reaching `TUR-E0003: unbound symbol 'Vel-write-cap'`:

- `tests/errors/defsystem-undeclared-write.tur` (unsized, via `use-cap!`)
- `tests/errors/sized-defsystem-undeclared-write.tur` (sized, via `set-Vel!`)
- `tests/errors/xworld-undeclared-write.tur` (cross-world)

What is missing is specifically the *unsized accessor route* -- and that route
does not currently exist for a user either, so there is nothing left to test.

## Options (owner's call)

1. **Retire the test.** Its accessor-route claim is covered by the sized
   counterpart, and its cap claim by `defsystem-undeclared-write.tur`. Delete
   it and drop the "both world surfaces" sentence from the sized test's
   header, which is the line that is now inaccurate.
2. **Convert it to `use-cap!`** so it reaches TUR-E0003. This makes it a near
   duplicate of `defsystem-undeclared-write.tur`; the only thing it would add
   is that a `defworld` + accessors declaration is present in the same file.
3. **Give `defsystem` a world type** (`(defsystem name World [reads] [writes]
   body)`), matching `sized-defsystem`, so an unsized system body can touch
   its world again. This is the only option that restores the *capability*
   rather than just the test, and it is a breaking macro-signature change.

Not chosen here: the fix depends on whether an unsized system body is meant
to reach its world at all, which is an ecs design question.

For reference, the turmeric-side fixture that pinned the same shape
(`tests/fixtures/errors/ecs-defsystem-writes-unauthorized`) took option 2,
because its `expected.diag` already named TUR-E0003 and the goal there was to
make the fixture reach the diagnostic it always claimed to test.

## Adjacent observation: `tur test tests` counts every `errors/` fixture as a failure

> **RESOLVED 2026-08-20.** Both halves of this section are fixed.
>
> The 16 fixtures moved from `spices/ecs/tests/errors/` to
> `spices/ecs/errors/`, so `tur test tests` is **72 tests, 72 passed, 0
> failed** -- the ecs CI job is no longer red on this directory. And
> `spices/ecs/errors/run.sh` (modelled on `spices/secret/errors/run.sh`, as
> this section suggested) now asserts every fixture is rejected with its
> intended diagnostic.
>
> Two changes beyond what was asked for, both prompted by what the runner
> found:
>
> 1. **Assertions pin a witness substring, not just a code.** TUR-E0001 and
>    TUR-E0003 each cover several ecs fixtures for unrelated reasons, so the
>    code alone would not have caught the `defsystem-set-undeclared` drift
>    this report is about. Each assertion also requires a substring only the
>    intended defect produces (e.g. `unbound symbol 'Vel-write-cap'`).
> 2. **A SECOND drifted fixture turned up.** `xworld-unused-world.tur`
>    documents in its own header that it must fail TUR-E0003 on the bare
>    symbol `defxsystem--world-pred-declared-but-never-read-or-written`. It
>    was actually failing **TUR-E0001 `expected Slot, got int`** -- RE0's lift
>    of storage indices to `Slot` made its literal `0` arguments illegal, and
>    that fires before the macro's validation. So the unused-world check had
>    silently stopped being tested too. Fixed by passing `(slot-new 0)`;
>    it now reaches the intended symbol.
>
> The workflow's claim that "`tur test <dir>` is NOT recursive" was simply
> wrong (measured: it recurses) and is corrected in `ci.yml`, along with a new
> opt-in `Assert compile-fail diagnostics` step that runs any spice's
> `errors/run.sh`. Note that step also picks up `secret`'s runner, which had
> never actually executed in CI; it passes.

### Original observation

Noted while gathering the table above, and not caused by anything here.
`tur test` recurses, and `spices/ecs/tests/errors/` holds compile-*fail*
fixtures, so:

```
$ tur test tests
86 tests, 70 passed, 16 failed     # exit 1
```

All 16 "failures" are exactly `tests/errors/*.tur`; the 70 real tests pass.
CI runs `tur test tests` for this spice (it has flat `tests/*.tur` files, so
the flat branch of the workflow is taken), which means the ecs job is red on
the errors/ directory alone.

Those fixtures need either a dedicated runner that asserts each is REJECTED
with the right diagnostic code, or relocation outside `tests/`.
`spices/secret/errors/run.sh` is one worked example of the former -- and it
is what surfaced that these 16 are not currently checked against their
intended codes at all, only against "does not compile", which is how
`defsystem-set-undeclared` drifted onto TUR-E0295 unnoticed.
