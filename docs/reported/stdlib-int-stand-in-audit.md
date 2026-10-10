# stdlib: `:int` stand-ins for callbacks and container payloads

**Status:** Reported -- most of it landed; the residue is below.
**Severity: low-medium (design debt) -- downgraded from "S1 subset high"
2026-10-08.** What made it high is gone: since 2026-09-28 no
`^fat <name> : int` callback is left in `stdlib/`, so no Turmeric closure
parameter has an unchecked shape any more (see "Done 2026-09-28"). S2 landed
13 of 19 sites (ref, chan, atomic) and S3 landed both items (`json/bool`,
`either.tur`). What is left, none of it a silent wrong answer:

- **S1, the `ptr<void>` callbacks** (`timer-set`, `scheduler-timeout`,
  `once-call`, `hamt/map` / `hamt/filter`, `fiber-new`, `register-test`) --
  raw C code pointers handed to C APIs, not Turmeric closures. They want a C
  function-pointer type or a fat-to-C trampoline: a design item, not a
  signature pass.
- **S2, 6 sites**: `dfs-set` (unblocked 2026-09-25, but `(BtCell A)` breaks
  every caller that spells bare `BtCell`), `future.tur`'s 4 (a 34-`defn`
  module redesign -- `(Future A E)`, `future-get` returning `(Result A E)`),
  and `fiber-yield` (no handle to carry a type). A `float` into any of them is
  still a loud `TUR-E0001`. A by-value struct no longer fails in cc, as
  measured below: re-measured 2026-10-08, `(future-of (Pt 42))` checks and
  runs. The struct is erased into the word, so the type is lost on the way
  back.

The `TUR_REGION_NOTE` gaps noted under `fiber-yield` are fixed (`fiber.tur`
carries 2, `future.tur` 4).

**Original severity (2026-09-16):** Design defect / expressiveness hole. Highest subset: **38 callback
parameters whose signature is entirely unchecked** -- any arity, any argument
types, any return type is accepted. Second subset: **19 container/cell payload
parameters declared `:int`**, which cannot carry a `float` at all and accept a
by-value struct that then fails in cc.
**Discovered:** 2026-09-16, while auditing runtime seams for
[the seam family](../archive/router-payloads-are-int64-only.md).
**Scope:** `stdlib/*.tur` only.

This is the **stdlib twin** of
[docs/archive/spices-int-stand-in-audit-2026-06-14.md](../archive/spices-int-stand-in-audit-2026-06-14.md),
which audited 35 spices and explicitly scoped itself to
`../turmeric-spices/spices/*/src/**`. The stdlib was never swept. Its S1-S4
rubric is reused below so the two are comparable, with one class added (S2) that
the spices audit did not need.

Everything below is measured against `./build/tur` built from `main` 145df1d49,
2026-09-16, Apple clang 21.0.0 (arm64-apple-darwin27).

## Two measurements that change how this reads

Both correct a naive reading of "it's declared `:int`, so the checker protects
you." Neither is what I expected going in.

**1. `^fat f : int` enforces callable-ness but nothing about the shape.** A bare
integer is rejected:

```
error: argument 1 to fat (^fat) parameter of 'sink' must be a function or
       closure, got int
```

but every one of these passes `tur check` with exit 0 into `(defn sink [^fat
handler : int] : int handler)`:

```turmeric
(sink (fn [a : cstr b : cstr c : cstr] : cstr a))   ; 3 args, all wrong types
(sink (fn [] : float 7.25))                          ; nullary, float result
(sink (fn [a : int b : int c : int d : int] : int a)) ; 4 args
```

So the protection is "is it callable", and that is all. This is better than the
spices audit's S1 wording implies (a non-callable *is* caught) and still leaves
handler shape completely unchecked -- which is the defect
[CLAUDE.md](../../CLAUDE.md) names by name, since `tour-tourist` shipped
`(fn [req : int] : int)` where it meant
`(fn [ctx : Ctx] : option<Response>)`.

**2. A payload parameter declared `:int` is loud for `float` and UNSOUND for
aggregates.** The two failure modes are not the same severity:

```
$ ./build/tur check chan-float.tur
error [TUR-E0001]: function 'chan-send' arg 2: expected int, got float   <-- loud

$ ./build/tur check chan-struct.tur      # (chan-send ch (Pt 42))
                                          <-- exit 0, ACCEPTED
$ ./build/tur run chan-struct.tur
error: passing 'tur_adt_Pt' (aka 'struct tur_adt_Pt') to parameter of
       incompatible type 'int64_t'
  8309 |   chan_hysend((void *)(intptr_t)(ch_1635), __ps_276);
```

A `:int` payload therefore does not merely fail to express a float -- it lets a
by-value struct through the type checker and into a cc error naming
`tur_adt_Pt`, on a program the Turmeric checker accepted. That is the same
"checker accepted, cc refused" shape as the runtime-seam family, arrived at from
the opposite direction: there the erasure is in a runtime slot, here it is in the
declared signature.

## S1 -- Callback typed `:int` or `ptr<void>` (38 sites)

Highest priority. Signature unchecked per the measurement above.

| Spelling | Count | Files |
| --- | --- | --- |
| `^fat <name> : int` | 24 | `httpd.tur` (17), `free.tur` (3), `httpd-compress.tur` (2), `reactor.tur` (1), `parsec.tur` (1) |
| `<name> : ptr<void>` | 14 | `hamt.tur` (4), `threadpool.tur` (2), `future.tur` (2), `timer.tur`, `thread.tur`, `test.tur`, `sync.tur`, `scheduler.tur`, `fiber.tur` (1 each) |

The `httpd.tur` cluster is the most consequential because it is the public
surface every web spice composes against -- `httpd-new`, `httpd-new-pool`,
`httpd-new-tls`, `httpd-new-async`, `router-add` all take `^fat handler : int`.
The spices audit's central observation applies verbatim: **new code written
against these APIs inherits the looseness**, because a downstream wrapper
parrots whatever the upstream signature says.

Note `httpd.tur:3469` already documents `httpd-handler-carrier` as the
sanctioned way to hand a composed `Handler` to "a server constructor (`^fat
handler : int`) or any `:int` handler sink that TAKES OWNERSHIP" -- so there is a
real, ownership-aware `Handler` type in that module, and the `:int` sinks are
what it has to degrade into. That makes httpd the cheapest S1 fix in the file:
the type already exists.

The `ptr<void>` variants are worse than the `^fat` ones in one respect --
`ptr<void>` does not even get the callable-ness check, since it is not a fat
parameter.

## S2 -- Container/cell payload declared `:int` (19 sites, 6 modules)

New class; the spices audit did not need it. Not a handle and not a callback --
the **value being stored**. Consequence: the container cannot carry a `float`,
`cstr`, or struct at all.

| Module | Sites |
| --- | --- |
| `chan.tur` | `chan-send`, `chan-recv`, `async-chan-send`, `async-chan-recv`, `async-chan-try-send`, `async-chan-try-recv` |
| `atomic.tur` | `atomic-new`, `atomic-load`, `atomic-store!`, `atomic-swap!`, `atomic-cas!` |
| `future.tur` | `promise-fulfill`, `promise-fail`, `future-of`, `future-error-of` |
| `ref.tur` | `ref-new`, `ref-get` |
| `fiber.tur` | `fiber-yield` |
| `backtrack-dfs.tur` | `dfs-set` |

**Deliberately excluded** as genuine integers, per the rule's own last bullet:
`chan-new`/`async-chan-new` `cap`, `atomic-add!`/`atomic-sub!` `delta` (atomic
*arithmetic* -- an int is the point), `async-chan-count`, and
`dfs-choose-int`'s `lo`/`hi`.

This class is the honest-but-crippled sibling of the runtime-seam family: you
cannot put a float in a channel, which is loud, rather than putting one in and
getting `7` back, which is silent. It is a smaller emergency and a larger
expressiveness hole -- "send a float between two threads" is not an exotic ask.

## S3 -- An ADT erased to `:int` in its own API

The type exists; the API talks `int` about it.

- **`either.tur`** is the clearest case: `left?`, `right?`, `from-left`,
  `from-right`, `either-map`, `either-map-left` all declare their Either
  parameter as `e : int`, and `either-map` takes `^fat f : (fn [int] int)` -- so
  an `Either` over any payload but `int` cannot be mapped at its own type, even
  though `match e (Left l) ... (Right r) ...` inside the body proves the ADT is
  real.
- **`free.tur`**: `free-pure [x : int] : int`, `free-fmap [free : int ^fat f : int]`.
- **`json.tur`**: `json/bool [v : int] : int` (a bool, as an int, returning a
  JSON value, as an int), `json/int`, `json/object-put [obj : int key : cstr val : int] : int`.
- **`csv.tur`** / **`csv-string.tur`**: `csv/emit-row [v : int]`, `csv/row-free [v : int]`.
- **`parsec.tur`**: `mbind [ma : int ^borrow ^fat fn : int] : int`.

`json/bool [v : int]` is worth singling out because it is the rule's "is it a
boolean? -> `:bool`, never `:int` with 0/1 convention" bullet, in the stdlib,
on a public constructor.

## NOT defects -- do not "fix" these

A sweep like this flags them, and changing them would be wrong. Recorded so the
next person does not.

`(defopaque List [A] :int)`, `Backtrack [A]`, `Kleisli [A B]`, `Zipper [A]`,
`NonEmpty [A]`, `SizedBuf [n]`, and the `:ptr<void>` variants `Goal [A]` and
`Parser [A]` are **phantom-typed newtypes over a carrier**, which is the
representation the typed path deliberately chose. `list-typed.tur:20` says so
outright -- "a typed view over the int64 cons-list carrier, carrying a phantom
element type A ... Since: end-to-end-monomorphization-plan (Phase 1.1)". The
carrier *is* the pointer; the type parameter supplies the static distinction the
rule asks for. Measured: `(list-of 7.25 1.5)` checks and runs clean.

`List` exposing only `list-empty?`, `list->carrier` and `list-count` with no
typed element accessor is a **feature in progress**, not an erasure defect --
element reads still go through the raw carrier because Phase 1.1 is where it
stopped.

Likewise, the per-file `: int` counts are a scale signal and **not** a defect
count: `httpd.tur` 50, `schema.tur` 42, `range.tur` 38, `parsec.tur` 35 include
many genuine lengths, counts, ports, and indices.

## Fix directions

1. **S1 first, starting with `httpd.tur`.** Spell the handler type:
   `^fat handler : (fn [Request] Response)` (or whatever the module's real shape
   is). httpd already has an ownership-aware `Handler`, so this is mostly
   propagating a type that exists. 17 of the 24 `^fat : int` sites are in one
   file.
2. **`ptr<void>` callbacks get real `:fn` types** -- these do not even get the
   callable-ness check.
3. **S2 wants a decision, not a mechanical rewrite.** Making `chan-send`
   parametric in its payload is the right answer and is the same design question
   the runtime-seam family raises: either monomorphize per payload type, or box
   with a tag the way `any` does (`tests/fixtures/any-box-struct/`).

   > **Corrected 2026-09-18.** This item originally said to sequence S2 against
   > `docs/upcoming/end-to-end-monomorphization-plan.md`. **That plan is
   > finished, and was already finished when this report was filed** -- it and
   > its successor were archived 2026-06-19, the successor's banner reading "End
   > to-end monomorphization landed ... the small ABI bridge that remains is
   > intentional and necessary, with no further work to be done on it". The
   > `../upcoming/` path had been dead for three months. So S2 is **not blocked
   > on a pending ABI decision**; there is nothing to sequence against, and the
   > question below is the real one.
4. **Until S2 lands, reject aggregates at the boundary.** The struct-through-a-
   `:int`-parameter case reaching cc is a plain soundness hole and is
   independently fixable: a by-value aggregate passed to a declared `:int`
   parameter should be a Turmeric diagnostic, not a `tur_adt_Pt` message from
   the C compiler. This is the cheapest item in the report and it protects every
   S2 and S3 site at once.
5. **S3 is per-module cleanup** and can follow at leisure, except `json/bool`,
   which should just be `:bool`.

Direction 4 is the floor. Everything else is a preference per the rule's own
"when you notice this in existing code" clause -- on the one track to v1 these
are matched, wrapped, or tightened as the work dictates, not a gate.

## Direction 4 reassessed 2026-09-18 -- it is NOT the cheapest item

**Attempted and reverted.** The claim above that direction 4 is "the cheapest
item in the report", "independently fixable", and "protects every S2 and S3
site at once" does not survive contact. The hole is real and still reproduces
(`tur check` exit 0 on a by-value aggregate into a declared `:int` parameter,
verified against v0.49.4), but **no type-level rule in the elaborator can close
it without rejecting crossings that work today.**

What was tried, both as a `TUR-E0295` rejection at the `TY_ADT -> TY_INT` hatch
in `elab_call.c` (the `Phase G0` comment there -- "ADT values are heap-allocated
and passed as int64_t pointers" -- is the stale premise):

| Rule | Fixture result |
| --- | --- |
| baseline, unmodified | **3047 passed, 0 failed** |
| reject `type_is_byvalue_aggregate` (the predicate `::` already uses) | 2989 passed, **59 failed** |
| narrowed to single-variant flat products (`n_ctors == 1`) | 3027 passed, **21 failed** |

The 59 and the 21 are regressions, not pre-existing failures -- the baseline
was measured on the same tree with the change reverted.

**Why the elaborator cannot decide.** The failures are not broken programs. They
are legitimate crossings the *emitter* handles:

- `emit_expr.c:8370` heap-boxes a by-value aggregate argument (`emit_agg_box`)
  at poly-carrier / HKT-wrapper boundaries -- "Slice 3
  (constrained-hkt-forall codegen)".
- ADT **constructor** arguments take a carrier field and box into it:
  `stdlib/logic.tur:105` `(StCons v ...)` with `v : Subst` is a by-value
  aggregate into an `:int` ctor field, and it is correct.
- SR1 by-value **sums** qualify as "by-value products" to
  `adt_is_byvalue_product` (via `adt_sr1_sum_candidate`), so
  `tests/fixtures/typed-slots/adt-float-payload` -- whose own comment reads
  "through polymorphic boundary: pointer survives as int64, value unchanged"
  -- is rejected by the blanket rule while being exactly the behaviour the
  fixture asserts.

So "by-value aggregate meets `:int` parameter" is **not** the predicate for
"this will fail in cc". The emitter knows; elaboration does not. That is also
why [byvalue-adt-int-cast-plan](../archive/byvalue-adt-int-cast-plan.md) could
close GAP 3 cleanly for `::` -- there the reinterpret is unconditionally
unsound, with no boxing rule to consult -- and why its headline, calling `::`
"the one erased-carrier boundary that neither uses [the box bridge] nor rejects
the cast", reads as complete but is not: **the call-argument position is a
second such boundary, and it is the harder one.**

**Revised direction 4.** A general rule does not belong in `elab_call.c`; the
boxing decision is the emitter's. But there is a **working precedent that is
not general**, and it is the more useful lead:

`turi-session-expansion-plan` S3.5 closed the whole silent-erasure seam family
(router / generator / async-await / binary-session payloads, all four archived
2026-09-16) with a payload-lowering pair --
`session_payload_to_word` / `session_payload_from_word`
(`src/compiler/elab_sessions.c:303+`, called from `elab_global.c`,
`elab_forms.c`, `emit_module.c`, mirrored in `src/turi/eval.c`). A float is
bit-reinterpreted, a pointer cast through `intptr_t`, and **a by-value struct
is rejected with a diagnostic (TUR-E0212) rather than reaching cc** -- which is
precisely what direction 4 asks for, already shipped, at four seams.

Why it worked there and not as a general rule: a session `send-to` / `recv-from`
is a **compiler-lowered template**, so the compiler owns the lowering site and
can wrap the payload on the way through. The general call boundary has no such
site -- which is what the 59/21-regression measurement above is really saying.

**The open question for both S2 and direction 4** is therefore narrower and
answerable: `chan-send` / `atomic-store!` / `ref-new` are ordinary stdlib
`defn`s over inline-C, **not** compiler-lowered forms, so the pair does not
obviously transfer. Either give those seams a lowering site the compiler owns
(the session shape), or make the containers parametric. That is the decision to
take -- not a wait on a plan that finished in June.

**Repro kept here rather than as a fixture.** An `errors/` fixture for this
would be permanently red -- the tree has no xfail/expected-fail marker (the
`requires.*` family are skip conditions, not known-failure ones) -- and a
standing red fixture with no way to mark it invites someone to "fix" it by
shipping one of the two rules measured above. The repro is three lines:

```turmeric
(defdata Vec2 :copy (Vec2 :int :int))
(defn sink [v : int] : int v)
(defn main [] : int (println (sink (Vec2 3 4))))
```

`tur check` exits 0; `tur run` fails in cc with
`passing 'tur_adt_Vec2' to parameter of incompatible type 'int64_t'`.

## Done 2026-09-18

- **S3, `json/bool`** -- `(defn json/bool [v : int] : int)` is now
  `[v : bool]`, the one item the report called out as unambiguous ("should just
  be `:bool`"). The inline-C body's `v ? 1 : 0` is unchanged, no caller outside
  the generated docstring table existed, and `(json/get-bool (json/bool true))`
  prints `true` / `false` across the pair. Suite 3047/0.

## The remaining 6 S2 sites, investigated 2026-09-19

S2 landed 13 of 19 sites (ref, chan, atomic). The other 6 were left with
"needs a design call" against them; this is what each one actually needs.
**None is blocked on effort, and none should be done the obvious way.**

### `dfs-set` (1 site) -- BLOCKED, and parameterising it today is a REGRESSION

> **Unblocked 2026-09-25.** The capture bug is fixed
> ([archived](../archive/generic-closure-capture-of-float-truncates.md)): a
> generic closure that captures a `7.25` and hands it to a carrier store
> (`vec-push!` in the measured stand-in for `bt-set!`) reads back `7.25`.
> Re-measure `(dfs-set c 7.25)` when parameterising.  The one adjacent shape
> that was still wrong -- a captured float passed to a fn-typed CALLBACK
> ([generic-closure-float-passed-to-fn-typed-callback](../archive/generic-closure-float-passed-to-fn-typed-callback.md),
> which `dfs-set` does not do: its `k` takes no argument) -- was fixed
> 2026-09-26.

Mechanically the smallest of the three: `(defopaque BtCell [A] :ptr)` plus
seven signatures in `trail.tur` and three in `backtrack-dfs.tur`, ~20 lines.
It was **written, measured and reverted**.

`dfs-set` returns a closure capturing its payload, and a generic type parameter
bound to `float` is **truncated when captured into a closure** --
[generic-closure-capture-of-float-truncates](../archive/generic-closure-capture-of-float-truncates.md),
a pre-existing compiler bug this investigation found and filed. Same program,
both ways:

| | `(dfs-set c 7.25)` |
| --- | --- |
| today, `v : int` | `error [TUR-E0001]: expected int, got float` -- loud |
| parameterised | runs, prints `3.45846e-323` -- silent wrong answer |

So the parameterisation converts a correct rejection into a wrong answer. **Do
not land it until that bug is fixed**; the ordering is a hard dependency, not a
preference.

Two notes for whoever picks it up. `dfs-choose-int` / `dfs-choose-go` enumerate
`lo..hi` into the cell, so they pin to `(BtCell int)` -- the `atomic-add!` case.
And `BtCell` lives in `trail.tur`, not the module this report lists.

A third, measured 2026-09-26 and the reason it was not landed with the
unblocking fix: **parameterising it is a breaking change for every caller that
spells the type.** A bare parametric name does not unify with an application
of it -- with `(defopaque AtomicCell [A] :ptr<void>)`, a
`(defn peek-it [c : AtomicCell] ...)` given `(atomic-new 41)` is
`expected AtomicCell, got (AtomicCell int)`. So `(BtCell A)` turns every
`[c : BtCell ...]` in user code into that error: five fixtures here
(`sx1-trail-basics`, `sx1-bt-row-checked`, `sx2-trail-measure-not-congruent`,
`self-recursive-goal-into-fat-sink`, `region-escape-via-store`, plus
`errors/sx1-bt-row-pure-caller-rejected`), the backtrackable-state guide's
examples, and whatever spices build on the trail. That wants the sibling
`turmeric-spices` checkout in hand, or bare-`BtCell` acceptance first.

### `future.tur` (4 sites) -- not 4 functions, 34

The report counts payload parameters; the module has **34 `defn`s**, and
parameterising `Promise`/`Future` touches nearly all of them. The payload path
itself is the easy part and would work today (it is `ref.tur`'s shape, no
closure capture, so the bug above does not apply). The surface is the problem:

- **`future-get` already builds a `Result`** -- `tur_box_ok(fc->value)` /
  `tur_box_err(fc->exn)` -- and then declares it `: ptr<void>`. With
  `(Future A E)` it should return `(Result A E)`, which is a strict
  improvement and arguably the single highest-value edit in the module.
- **The cell has two payload slots** (`int64_t value; int64_t exn;`), so the
  question really is `(Future A)` vs `(Future A E)`. Two parameters is the
  honest shape.
- **The combinators change the type**, which no other S2 module had to do:
  `future-map` is `A -> B`; `future-all2` / `future-join` are
  `(Future A) + (Future B) -> (Future (Tuple2 A B))`, where `Tuple2` is a
  hand-rolled `ptr<void>` whose `tuple-first`/`tuple-second` return `:int` and
  would need parameterising too; `future-race-n` / `future-all-n` /
  `future-any-n` take a raw `ptr<void>` array of futures, which wants a typed
  array before it can carry an element type.

This is a module redesign with a real payoff, not a signature pass. Worth its
own plan.

### `fiber-yield` (1 site) -- needs a handle it does not have

`(defn fiber-yield [value : int] : nil)` takes **no handle**. It is called from
inside the fiber body and reaches the runtime through
`tur_fiber_block_yield(value)`, which stores into
`tur_current_fiber->result` -- there is nothing in its arguments to carry a
type parameter. Typing the payload means either giving it a handle parameter or
routing it through an effect-typed mechanism, and either way the protocol has
**three** payloads that have to agree: `fiber-resume`'s argument, the yielded
value, and `fiber-resume`'s return.

Also worth noting while here: `tur_fiber_block_yield` writes a caller word into
a `FiberBlock` that outlives the yield, and `stdlib/fiber.tur` carries **zero**
`TUR_REGION_NOTE`s -- the same missing-hook shape that `chan.tur` had before
S2. `future.tur` has zero as well, across four stores of a caller word into a
malloc'd cell (`promise-fulfill`, `promise-fail`, `future-of`,
`future-error-of`). Both are independent of the typing work and are cheap to
fix on their own.

## Done 2026-10-10 -- `reactor-add-chan`'s channel, and a limit on typing C-called callbacks

`reactor-add-chan` took its channel as a bare `ch : ptr<void>`. Getting one
from a linear `(Chan A)` took an erasing ascription, which consumes the
handle, so a caller kept a raw view and re-typed it to send on and free it. It
is now `[A] [^borrow r : Reactor ^borrow ch : (Chan A) ...]`, with
`reactor-add-async-chan` for `(AsyncChan A)`. `reactor.tur` loads `chan.tur`
for the types.

The callback's value slot deliberately stays `int`, not `A`. reactor.c calls
every callback as `(env, int64, int64, int64)`, and a closure keeps its
declared C parameter types. A `(fn [int A ptr<void>] nil)` instantiated at
float therefore read its double from a float register reactor.c never loaded:
measured `0.25` and then `-1.7e260` under gcc for `7.1 + 0.25`, and an
fnsan trap against an instrumented libturi. The `int` slot keeps the
structural float-vs-word refusal. The same limit bounds S2's `future.tur`
redesign: a callback that C code calls cannot have a type-variable slot until
something bridges the carrier word to the slot's real type.

**`local-park-chan`, the same day.** It took `ch : ptr<void>` too, and
returned an `int` with `-1` as its not-in-a-fiber sentinel, which a received
`-1` could not be told apart from. It now borrows a `(Chan A)` and returns
`(Option A)`; `local-park-async-chan` is the `AsyncChan` twin. libturi gained
`tur_local_park_chan_status`, which returns the status separately from the
value; the old single-return entry is kept for ABI compatibility. Unlike a
reactor callback's argument, the value here is a return, so the compiler's
carrier-to-`A` conversion applies: a float channel yields its float.

Found on the way and fixed: a channel watch, or a fiber park, on a channel
that already held a value never fired. `cap_timeout` computed io_poll's
timeout from timers alone, and `tick_chans` only runs after io_poll returns,
so with no timer and no `reactor-wake` the poll blocked forever. This
reproduces on v0.63.9. `cap_timeout` now returns 0 while a watched channel
holds a value (`reactor-chan-watch-nonempty`).

**The fiber group handle, the same day.** `g` was a bare `ptr<void>` across
the driver (`local-fiber-group-new` / `-free`, `local-spawn`, `local-park-*`,
`reactor-run-fibers`), so the `Reactor` itself type-checked where the group
belonged, and a dropped or doubly freed group was invisible. It is now
`(defopaque LocalFiberGroup :ptr<void> :linear)`, the `Reactor`'s
discipline: `local-fiber-group-free` consumes it and everything else
borrows. A fiber body that uses `g` captures it read-only, so it stays an
ordinary closure. No existing caller passed the group through a `ptr<void>`
helper, so all six fiber fixtures pass unchanged. `httpd` reaches its group
only in inline C and is unaffected. Pinned by `errors/local-fiber-group-dropped`
(TUR-E0100), `-use-after-free` (TUR-E0101) and `-wrong-handle` (a Reactor
where the group is expected).

## Done 2026-10-10 -- five plain `cb : int` reactor callbacks S1 did not count

S1 measured `^fat <name> : int` and `<name> : ptr<void>`. A callback spelled
plain `cb : int` (no `^fat`) matched neither pattern, and `stdlib/reactor.tur`
had five: `reactor-add-fd`, `-timer`, `-interval`, `-signal`, `-chan`. They
were worse than unchecked. A captureless lambda arrived as a bare C function
pointer, and reactor.c called it through a fat box's slot 0 (SIGSEGV). They
are now `^fat cb : (fn [int int ptr<void>] nil)` / `(fn [int ptr<void>] nil)`,
as `local-spawn`'s body already was. See
[nil-argument-to-ptr-param-emits-void-expression](../archive/nil-argument-to-ptr-param-emits-void-expression.md).
A future sweep should grep for bare `<name> : int` callback parameters as well.

## Done 2026-09-28 -- S1's `^fat : int` class is gone, and one checker hole found on the way

**S1, `^fat <name> : int` -- zero left in `stdlib/`** (was 24; httpd's 17
and the `reactor` / `httpd-compress` sites had already moved to their real
handler types).  The last six now spell their shape:

| Site | Now |
| --- | --- |
| `free-bind` `kont`, `free-fmap` `f`, `free-run` `interp` | `(fn [int] int)` -- over the erased `Free` carrier, the honest shape of what the inline-C body calls |
| `parsec.tur` `mbind` `fn` | `(fn [int] int)` -- the signature `backtrack.tur`'s `mbind` already had |
| `compose-middleware-of` `base` | `(fn [ptr<void>] nil)` -- the handler type `httpd-new` / `router-add` take |

and two of the `ptr<void>` callbacks, both fat closures under the hood:
`future-map`'s `fn` is `(fn [int] int)`, `future-then`'s is
`(fn [int] Future)`.  Each inline-C body is unchanged -- a fn-typed `^fat`
parameter is the same `int64_t` fat handle in C -- so this is purely what the
checker sees: `(future-map fut (fn [] : float 7.25))` and a three-`cstr`
lambda into `free-bind` are now `TUR-E0001` ("expected a function of type
(fn [int] : int) ... arity, argument types and result type must match").
Callers in the tree needed no change.

**A hole this exposed: a VARIADIC callee's fixed fn-typed parameter was never
shape-checked.**  Typing `compose-middleware-of`'s `base` changed nothing at
first -- a two-argument lambda still passed `tur check` with exit 0 -- because
the variadic call path (elab_call.c, AR8) elaborates the fixed arguments with
none of the fixed-arity path's checks.  It now runs the same structural test
(`fn_type_structurally_compatible`, the plain-fn-typed-params-are-kind-matched
rule) on each fixed fn-typed parameter.  Pinned by
`tests/fixtures/errors/variadic-fixed-fn-param-shape-checked`.  Note the
test is structural, as it is on the fixed-arity path: arity and register
class are enforced, but `cstr` and `int` are both one word, so a
`(fn [cstr] cstr)` still passes where `(fn [int] int)` is declared -- on
either path.  That is the existing rule, not something this change weakened.

**The remaining `ptr<void>` "callbacks" are mostly not closures at all.**
`timer-set`, `scheduler-timeout`, `once-call` (`pthread_once`),
`hamt/map` / `hamt/filter` (C predicates taking `ctx`), `fiber-new`,
`register-test`: each hands a raw C code pointer to a C API with a fixed C
signature.  A Turmeric `(fn ...)` type describes a Turmeric-ABI function
value (thin or fat), not a C function pointer, so retyping these as `:fn`
would promise something the body does not deliver.  They want a C
function-pointer type (or an adapter that owns the fat-to-C trampoline), which
is a design item, not a signature pass.  `with-cancel-guard` DOES take fat
closures (it reads slot 0 as the thunk) and is the one left that could be
typed directly; its calling convention (`thunk(closure, 0)`) wants reading
before choosing the signature.

> **2026-10-03: `with-cancel-guard` typed.**  Read, its convention was
> wrong as well as loose: each zero-argument closure was called as
> `int64_t (*)(void *, int64_t)` with a stray `0`, while a `(fn [] nil)`
> thunk is `void (*)(void *)` -- a `-fsanitize=function` trap on every call.
> Both parameters are `^fat (fn [] nil)` now and slot 0 is called at exactly
> that type (`tests/fixtures/cancel-guard-typed-closures`, clean armed).  A
> word-returning body was still accepted into the `nil` slot -- a general
> checker gap, filed as
> [word-result-fn-into-nil-slot](../archive/word-result-fn-into-nil-slot.md)
> and resolved the same day: it is `TUR-E0001` now.

**S3, `either.tur` -- attempted, NOT landed, and why.**  Making the module
generic in the `option.tur` / `result.tur` idiom (`left? [L R] [e : (Either
L R)]`, `from-right [L R] [dflt : R e : (Either L R)] : R`, `either [L R C]`,
`either-map [L A B] ... : (Either L B)`) type-checks every caller in the tree
but one -- and that one is a real compiler gap the `:int` erasure has been
hiding:

```turmeric
(load "stdlib/either.tur")
(defn inc [x : int] : int (+ x 1))
(defn main [] : int (println (from-right -1 (fmap (Right 41) inc))) 0)
```

`(Right 41)` alone leaves `L` open, and a constructor application of a
parametric sum with an undetermined parameter is typed as the BARE ADT
(`Either`, no applied arguments -- a `let` annotation mismatch reports it as
"got adt").  The dispatch's result grounding (hkt-carrier-result-loses-
payload-types, elab_typeclasses.c) works on the receiver's TY_APP chain, so
with no chain it has nothing to ground and `fmap` falls back to the def-less
`(type-app ? ?)` shell -- which no generic `(Either L R)` parameter unifies
with.  `Result` already shows it today: `(ok-val (fmap (Ok 41) inc))` is
`TUR-E0001 ... expected (Result A B), got (? ?)`.  With the receiver typed
(`(let [e : (Either int int) (Right 41)] ...)`) both work.  So the generic
`either.tur` is ready and waits on the constructor typing (an application
with its open parameters as fresh variables, not a bare ADT), and
`sum-either-functor-instance` is the regression it would otherwise cause.
That is a type-system change with a wide blast radius, not a stdlib pass.  `str->int-checked`'s `: int` return (str.tur) belongs to the
same change: it builds an `Either` in inline C and should declare
`(Either int int)`.

> **2026-09-29: the constructor typing landed; `either.tur` meets a second
> gap.** `(Right 41)` is `(Either L int)` now, with `L` an open slot, and
> `fmap` over it grounds
> ([fmap-over-underdetermined-constructor-is-a-defless-shell](../archive/fmap-over-underdetermined-constructor-is-a-defless-shell.md),
> archived). The generic `either.tur` was rewritten again against it. Every
> caller in the tree passed, `sum-either-functor-instance` included, once
> `str->int-checked` declared `(Either int int)`. It still did not land: a
> generic map's by-value monomorph refuses its own arms in C. `(either-map inc
> (Left 9))` reached cc, where the `int`-typed version runs it. The cause, and
> `result-map`'s identical failure on `main` for a type-changing function, is
> [generic-call-result-leaks-callee-tyvar-names](../archive/generic-call-result-leaks-callee-tyvar-names.md).
> That is the blocker now.

> **2026-10-01: that blocker is resolved** (archived).  A generic map over a
> private `defdata` with both arm spellings -- constructor arms, and arms
> through generic constructor helpers -- runs in both engines, including over
> an open `(Left 9)`; `tests/fixtures/generic-call-result-binds-from-expected`
> pins it.  The `either.tur` rewrite itself has not been redone.

> **2026-10-02: landed.**  `either.tur` is `(Either L R)` throughout --
> `left?` / `right?` `[L R]`, `from-left [dflt : L]`, `from-right [dflt : R]`,
> `either [L R C]`, `either-map [L A B] ... : (Either L B)`,
> `either-map-left [A B R] ... : (Either B R)` -- and `str->int-checked`
> declares `(Either int int)`.  Every caller in the tree passed unchanged
> (suite 3502/0 before the new fixtures; the four snapshot moves are uncalled
> generics no longer emitted at the carrier).  Both engines agree on a
> `(Either cstr float)` -- `7.25`, `bad`, a doubled `14.5` through
> `either-map` -- which the `e : int` signatures rejected outright, and on
> `(either-map inc (Left 9))`; armed `-fsanitize=function` and the
> float-conversion lint are clean on it.  An `int` default against a
> `(Either cstr cstr)` -- accepted before, answering a word that was
> sometimes a string pointer -- is `TUR-E0001`.  Pinned by
> `tests/fixtures/either-generic-payloads` and
> `tests/fixtures/errors/either-from-right-default-type`.  No turmeric-spices
> file calls these functions.

## See also

- [docs/archive/spices-int-stand-in-audit-2026-06-14.md](../archive/spices-int-stand-in-audit-2026-06-14.md)
  -- the spice-side twin, same rubric, 35 spices.
- [router-payloads-are-int64-only](../archive/router-payloads-are-int64-only.md),
  [generator-yield-payload-is-int64-only](../archive/generator-yield-payload-is-int64-only.md),
  [async-await-payload-is-int64-only](../archive/async-await-payload-is-int64-only.md),
  [session-payloads-are-int64-only](../archive/session-payloads-are-int64-only.md) -- the
  same erasure where it is SILENT rather than declared.
- [CLAUDE.md](../../CLAUDE.md) -- "No Lazy `:int` Stand-Ins -- STRICT RULE".
