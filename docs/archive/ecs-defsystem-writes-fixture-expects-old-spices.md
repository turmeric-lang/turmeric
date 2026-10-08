# errors/ecs-defsystem-writes-unauthorized expects a diagnostic current spices HEAD never reaches

**Status:** RESOLVED 2026-08-18.
**Severity:** low (one `requires.spices` negative fixture; invisible unless
the optional sibling checkout is present)
**Found:** 2026-08-17, when cloning `../turmeric-spices` for the row-types R1
demand survey un-skipped the fixture for the first time in this container.

## Summary

`tests/fixtures/errors/ecs-defsystem-writes-unauthorized/expected.diag`
expects

```
TUR-E0003
unbound symbol 'Vel-write-cap'
```

but against `turmeric-spices` HEAD the program never gets that far:
elaboration of the ecs spice itself fails earlier, inside
`spices/ecs/src/ecs/world.tur` (`no instance binding for associated type
'Storage' at this type`, `defdata: could not resolve constructor field
type`, `no typeclass method found for 'Pos'`), so the write-capability
check whose diagnostic the fixture pins is never reached and the harness
reports `diagnostic mismatch`.

Verified pre-existing at ffbde045 with an unmodified build: the harness
fails the fixture identically with and without the macro-provenance change
that was in flight when this surfaced.  The 2026-08-17 provenance NOTE does
additionally appear in the stderr (`in expansion of macro 'defworld' ...`),
but the mismatch is the two missing expected substrings, not the extra
note.

## Root-cause direction (unverified)

Either the fixture predates an ecs spice refactor (world.tur's
Storage-associated-type machinery looks newer than the fixture's
write-capability vocabulary), or the ecs spice at HEAD needs a newer
turmeric than this branch provides (an associated-type feature gap).
Deciding which requires bisecting turmeric-spices against a fixed turmeric
-- start by checking whether ANY current turmeric branch elaborates
`spices/ecs/src/ecs/world.tur` cleanly; if none does, the drift is in the
spice, and the other `requires.spices` fixtures pass only because they do
not import `ecs/world`.

## Repro

```sh
git clone https://github.com/turmeric-lang/turmeric-spices/ ../turmeric-spices
TUR_TEST_FILTER='^errors/ecs-defsystem-writes-unauthorized$' bash tests/run.sh
```

Without the sibling checkout the fixture PASS-skips and the suite shows
nothing -- which is why this sat invisible.

## RESOLVED (2026-08-18)

The report's **first** hypothesis was right -- the fixture predates an ecs
spice refactor -- and there turned out to be a second, independent layer
underneath it.

### Layer 1: the missing `defcomponent` registrations

`spices/ecs/src/ecs/world.tur` elaborates **cleanly** against current
turmeric (`tur check` on it exits 0), so the drift was never a
turmeric-vs-spice feature gap. What changed is that `defworld` now requires
each component's storage to be registered first:

```turmeric
(defcomponent Pos)   ;; E2d-P4: so (Storage Pos) projects to (Dense Pos)
(defcomponent Vel)
```

Without them the `(defworld GameWorld [Pos Vel])` expansion fails on the
associated type -- which is exactly the `no instance binding for associated
type 'Storage'` triple the report recorded. The spice's own twin of this
fixture (`spices/ecs/tests/errors/defsystem-set-undeclared.tur`) is otherwise
byte-for-byte similar and *does* carry the two `defcomponent` calls, so the
turmeric copy had simply been left behind.

### Layer 2: the accessor route is unreachable from a plain `defsystem`

Adding the registrations gets past `defworld` and straight into a different
wall:

```
TUR-E0295: cannot reinterpret by-value aggregate 'GameWorld' as a one-word
carrier (:int / :ptr<void>)
```

`defsystem` expands to `(defn <name>-impl [w : int] : nil ...)` and takes no
world-type argument, so `w` is an untyped int -- while the generated
accessors take `^borrow w : GameWorld`, a by-value struct. The fixture's
original `(let [gw : GameWorld (:: w GameWorld)] ...)` bridged those when the
world was int-carried; it cannot now, and there is no supported replacement
for a *plain* `defsystem`. (`sized-defsystem` does take the world type, binds
`w` at it, and reaches the accessors fine.)

Crucially that diagnostic fires **before** the cap check, so an
accessor-route body stops testing the thing the fixture exists to pin.

### Fix

The fixture keeps declaring the world and its accessors -- so it still
exercises the real `defworld` / `defcomponent-accessors` surface -- and its
system BODY becomes `(use-cap! Vel-write-cap)`, matching the shape the spice
itself uses in `tests/errors/defsystem-undeclared-write.tur`. It now fails
with exactly the pinned diagnostic:

```
input.tur:56:13: error [TUR-E0003]: unbound symbol 'Vel-write-cap'
```

`expected.diag` is unchanged -- the fixture now reaches the diagnostic it
always claimed to test, rather than the diagnostic being relaxed to whatever
it happened to produce.

Verified with the sibling checkout present (a symlink at
`<turmeric-root>/../turmeric-spices`; note a worktree sits one level deeper
than a plain clone, so the conventional sibling path does not resolve without
one). Suite: **2621 passed, 0 failed**, with the sole `requires.spices`
fixture un-skipped.

### Spice-side finding, filed separately

`spices/ecs/tests/errors/defsystem-set-undeclared.tur` is the only ecs
negative test that produces TUR-E0295 -- every other one gives a topical
diagnostic -- so it is currently **passing for the wrong reason** and its
header comment ("together they prove the cap-gating guarantee holds on both
world surfaces") is no longer true of the unsized half. Reported in
turmeric-spices as `docs/ecs-unsized-defsystem-cannot-reach-its-world.md`.
The guarantee itself is still covered, by `defsystem-undeclared-write.tur`,
`sized-defsystem-undeclared-write.tur`, and `xworld-undeclared-write.tur`
(all TUR-E0003).
