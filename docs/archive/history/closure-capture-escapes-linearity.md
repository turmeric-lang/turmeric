# A closure that consumes a captured `^linear` / `^unique` value escapes the checker

**Severity:** high (memory-unsafe -- a confirmed double-free, not a rejection gap)
**Status:** RESOLVED (2026-07-26). Implemented as the report's own fix direction
-- the narrow "consumes, not merely captures" rule. All seven rows of the scope
table below now behave; see [Resolution](#resolution).
**Found by:** the "confirm no double-drop when a linear value is captured behind
an `rc`" open question in
[docs/upcoming/v1/gc-cycle-collection-followup-plan.md](../../upcoming/v1/gc-cycle-collection-followup-plan.md)

## Summary

The linearity checker does not follow a linear or unique value through a closure
capture. Consuming a captured linear value inside a closure body does not mark
the outer binding consumed, so invoking the closure twice consumes it twice --
with no diagnostic.

```turmeric
(load "stdlib/serial.tur")
(defn main [] : int
  (let [b (bytes-alloc 8)]
    (let [f (fn [] : void (bytes-free b))]
      (f)
      (f)))                              ;; accepted -- no diagnostic
  0)
```

```
==3028==ERROR: AddressSanitizer: attempting double-free on 0x502000000010 in thread T0
SUMMARY: AddressSanitizer: double-free in free
```

The control is caught, which is what makes this a hole rather than a missing
feature -- the same two consumptions written without the closure are rejected:

```turmeric
(let [b (bytes-alloc 8)]
  (bytes-free b)
  (bytes-free b))
;; error [TUR-E0101]: linear value 'b' used after being consumed
```

## Scope

Measured on `Bytes` (a `:linear` struct with a real `bytes-free`), and on
`^unique` via `ref`:

| shape | result |
| --- | --- |
| `(bytes-free b)` twice, directly | **rejected** TUR-E0101 |
| `(rc/of b)` -- wrap a linear value | **rejected** TUR-E0103 |
| `(rc/of u)` -- wrap a unique binding | **rejected** TUR-E0202 |
| closure captures `b`, called twice | **accepted** -> double-free |
| closure captures `b`, aliased (`g = f`), each called once | **accepted** |
| closure captures a `^unique` value, called twice | **accepted** |
| closure capturing `b`, then `(rc/of closure)` | **accepted** |

Two of those deserve separate mention.

**The aliasing row is the nastiest.** Neither call site is locally suspicious --
`f` is called once and `g` is called once. Only the fact that `g` is `f` makes it
a double consume, and nothing in the checker connects them.

**`rc` is a vector, not the cause.** The question that prompted this was about
`rc`, and `rc` does make it worse -- `(rc/of closure)` is accepted even though
`(rc/of linear-value)` is rejected, so shared ownership of the closure gives you
a supported, ergonomic way to call it from several places. But the minimal repro
needs no `rc` at all. Fixing the `rc` surface alone would leave the bug intact.

## Root cause

`elab_fn` (`src/compiler/elab_fns.c:4398-4403`) collects the closure's free
variables for capture analysis and never consults their `CopyKind`:

```c
/* Phase 3: Capture analysis - collect free variables in the body */
uint32_t n_captures = 0;
Binding **captures = collect_free_vars(body, params, n_params,
                                       letrec_self_group, n_letrec_self_group,
                                       &n_captures);
```

Linear consume-state *is* tracked carefully elsewhere -- `if`/`cond`
(`elab_forms.c:2416-2623`) and `match` (`elab_structs.c:3356-3960`) snapshot,
branch, and merge it via `linear_state_snapshot_bindings` /
`linear_state_capture_current` / `linear_state_restore`. Closure bodies were
simply never wired into that discipline.

The concept already exists for the analogous case. `TUR_E0500` rejects a
`^multishot` handler that captures a `CK_UNIQUE` / `CK_LINEAR` binding
(`elab_effects.c:2045-2058`), using exactly the machinery this needs -- the same
`collect_free_vars` result, filtered on `CopyKind`:

```c
Binding **caps = collect_free_vars(cases[i].body, hparams, n_hparams, NULL, 0, &n_caps);
for (uint32_t ci = 0; ci < n_caps; ci++) {
    CopyKind ck = caps[ci]->type.copy_kind;
    if (ck == CK_UNIQUE || ck == CK_LINEAR) { /* TUR_E0500 */ }
}
```

So the gap is not a missing capability. A `^multishot` continuation is
definitionally invoked N times, so a blanket rejection is right there; an
ordinary closure is not, so the same check cannot simply be copied over.

## Fix direction

**A closure that consumes a captured linear value is itself linear.** That is the
standard treatment and it makes both bad shapes fall out of checks that already
exist, with no new diagnostic:

- `(f) (f)` becomes "linear value used after being consumed" -- TUR-E0101.
- `(rc/of f)` becomes "cannot wrap a linear value in rc<T>" -- TUR-E0103.
- Dropping the closure unused becomes "linear value dropped" -- TUR-E0100,
  which is also correct: the captured resource would never be released.

The same rule with `CK_UNIQUE` gives the unique case, via TUR-E0202.

**Consumes, not merely captures -- this distinction is the whole design.** A
closure that only *reads* a captured linear value is safe at any arity, and the
blanket E0500-style rule would reject a great deal of working code. Blast radius
measured across the fixture tree with a temporary probe in `elab_fn`:

```
fixtures affected: 73   total captures: 78
```

Most of those are almost certainly read-only captures -- the `httpd-*`
middleware closures dominate the list, and a per-request handler that consumed a
linear value would already be crashing on the second request. So the narrow rule
("the closure inherits linearity only if its body *consumes* the capture")
should shrink 73 to a handful, while the blanket rule would break all 73. Do not
implement the blanket version.

Determining "consumes" means asking, for each captured binding, whether the
closure body moves it into a consuming position -- the same question
`is_linear_consumed` already answers for straight-line code, evaluated against
the body with the capture in scope.

## Notes

- This was found while confirming an open question that was recorded as
  *unverified* rather than assumed safe. It was not safe.
- No fixture is added here. A fixture pinning the current behavior would have to
  assert a double-free, and one pinning the *correct* behavior would be a
  standing red. Both repros above are two-liners; reproduce with
  `TUR_CC_FLAGS="-fsanitize=address" tur build --runtime=source`.
- Related: the `^multishot` capture rule (TUR-E0500) is the nearest existing
  precedent and should stay as-is -- it is correct for its case.

## Resolution

The fix direction above, implemented as written: **a closure that consumes a
captured linear/unique value is itself linear/unique.** No new diagnostic; every
bad shape falls out of checks that already existed.

### Detecting "consumes"

Cheaper than the report anticipated, because of a detail of how the bug worked.
Elaborating the closure body already marks the OUTER binding consumed -- at the
point the closure is *built*, not where it is called, which is exactly why
`(f) (f)` slipped through. So the body's effect is readable by comparing the
enclosing bindings' substructural state before and after body elaboration; no
separate "does this body consume x" analysis is needed.

`elab_fn` snapshots every enclosing linear/unique binding before the body
(`FnCaptureLinSnap`, arena-allocated -- elab_fn has many early returns between
the snapshot and its use, and the compiler path is leak-checked), then intersects
"transitioned during this body" with the existing `collect_free_vars` capture
list.

### Three fn types, not one

The mark had to be restated three times, which is the one thing that made this
fiddly. `elab_fn` builds the lambda's type, then -- on the capturing path, the
only path that can carry an inherited obligation -- rebuilds it twice: once as
the env-prepended thunk type, once as the `EX_CLOSURE` value type. Both come from
a fresh `type_fn()`, which zero-inits `copy_kind`, so setting it only on the first
was silently discarded before any binding saw it. The `EX_CLOSURE` type is the one
a `let` actually binds.

### Calls have to discharge the obligation

Marking the closure linear was not enough: a call in head position does not go
through the general var-use path in `elab_toplevel.c`, so nothing ever consumed
the closure and a *called* closure was reported as dropped (E0100). Added
`call_consume_linear_callable` at the `elab_call_fn` choke point. Continuations
are excluded -- `(k v)` on a `TY_CONT`/`is_continuation` binding is application
sugar with its own consume-and-check in `elab_call_fn_inner`, and running both
consumes here then reports a spurious use-after-consume there.

### One narrowing the report did not call for

The `^unique` half needed `let` to infer uniqueness from the initializer's type,
which it previously did only from an explicit `^unique` annotation. That
inference is scoped to `TY_FN` initializers rather than written the way the
CK_LINEAR branch beside it is (any type): `ref<T>` also carries CK_UNIQUE, so a
general rule would silently make every `(let [r (ref 7)] ...)` unique -- a much
bigger behaviour change than this report asks for. A closure is the only shape
that can acquire CK_UNIQUE the new way.

### Results

Every row of the scope table above:

| shape | before | now |
| --- | --- | --- |
| closure captures `b`, called twice | accepted -> double-free | **TUR-E0101** |
| closure captures `b`, aliased (`g = f`), each called once | accepted | **TUR-E0101** |
| closure captures a `^unique` value, called twice | accepted | **TUR-E0201** |
| closure capturing `b`, then `(rc/of closure)` | accepted | **TUR-E0103** |
| ditto, `^unique` | accepted | **TUR-E0202** |
| closure capturing `b`, never called | accepted | **TUR-E0100** |
| closure only *reads* `b`, called twice | accepted | accepted (unchanged) |

### Blast radius: zero

The report measured 73 fixtures / 78 captures at risk under a blanket rule, and
predicted the narrow rule would cut that to a handful. It cut it to **none** --
`bash tests/run.sh` is **2365 passed, 0 failed** with the six new fixtures
included and no existing fixture touched. The read-only-capture control is the
one that pins this.

### Coverage

- `tests/fixtures/errors/closure-capture-linear-called-twice` -- E0101, the
  minimal repro.
- `tests/fixtures/errors/closure-capture-linear-aliased` -- E0101 via the
  aliasing shape the report called the nastiest.
- `tests/fixtures/errors/closure-capture-linear-in-rc` -- E0103.
- `tests/fixtures/errors/closure-capture-linear-dropped` -- E0100.
- `tests/fixtures/errors/closure-capture-unique-called-twice` -- E0201.
- `tests/fixtures/closure-capture-linear-read-only` -- the negative control: a
  read-only capture called twice still runs, printing `8` twice.

The report declined to add fixtures because one pinning correct behaviour would
have been a standing red. That reason is gone.

### Known limitation, pre-existing and not introduced here

A consuming call inside a loop is still accepted:

```turmeric
(while (< i 3) (f))          ;; accepted
(while (< i 3) (bytes-free b))  ;; also accepted -- same, without the closure
```

Linear checking is flow- but not iteration-sensitive, so one syntactic
consumption site reads as one consumption. This affects direct code identically,
which is the point: after this change the closure form is no longer *weaker* than
writing the consumption out by hand, which is what the report was about. Closing
the loop case is a separate piece of work on the checker itself.

### Verification

`bash tests/run.sh` **2365 passed, 0 failed**. The turi-interpreter suite is
1803 passed / 1 failed (`refine-off-is-contracts-only`), unchanged from base.
Two `cps-tramp-resume-*` failures seen in a combined `ctest` run were CPU
contention from 14 targets in parallel, not regressions -- both pass standalone
and the turi harness run on its own is clean.
