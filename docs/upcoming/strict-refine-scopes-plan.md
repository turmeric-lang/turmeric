# Plan: Scoped Strict Refinement (Knowing Before a Runtime Check Is Installed)

> **Status:** PLANNING (2026-10-27). In the planning stage only: the design is
> still being considered, nothing is scheduled, and nothing is implemented.
> Spellings below are proposals.
> **Type:** Compiler / Refinement types / Diagnostics
> **Related:** `docs/upcoming/refined-function-types-plan.md` (its F2 adds new
> fallback checks; its open question 5 is where this came from),
> `docs/guides/refinement-types-guide.md` (`--strict-refine`, "What gets
> checked"), `docs/guides/contract-types-guide.md` ("Where Checks Are
> Inserted")

## The problem

When the solver cannot decide a refinement obligation, Turmeric keeps a
runtime check. That is the right default: some obligations are undecidable,
and a runtime check is what keeps them sound. But performance-sensitive code
wants to **know before** such a check lands on its hot path, and often wants to
decide **where** it goes, for example once before a loop instead of once per
iteration.

The current tools do not fit:

- `--strict-refine` is all or nothing, for the whole build. It cannot be
  turned on for one hot function or module. It is a CLI flag only: neither
  `build.tur` nor an annotation reads it.
- Visibility is uneven. An unproved return refinement or loop invariant warns
  (`TUR-W0372`). An unproved direct-call argument is silent, which is fine
  today, because no check is added there (see below). It would not be fine
  for checks that new features add (refined function types, F2).
- When the solver cannot decide, the ways out are limited. An `if` guard gives
  the solver the fact it needs. An explicit `require!` / `assert!` does not
  (probe below), so "write the check yourself, where you want it" does not
  work today.

## Two kinds of runtime check

The design rests on separating:

- **Declared checks.** These come straight from a signature the programmer
  wrote: a refined parameter's **entry check** and the `:pre` check. They
  are always emitted, proof or not. Eliding an entry check needs
  whole-program knowledge, and the guide lists that as a by-design limit.
  The source already shows them.
- **Fallback checks.** These exist only because a proof failed, so a proof
  would have removed them. Today: a function's own return refinement and
  `:post`; a loop invariant's entry and preservation checks; the class result
  check added to an instance whose result variance is undecided. After
  refined-function-types F2, also the argument and result checks at a call
  through a refined function-typed binding. (A refined `let` binding's check
  belongs here too if discharge elides it; P0 confirms.)

**A strict scope governs fallback checks only.** It cannot make declared
checks disappear, and it should say so plainly. A strict hot function still
pays its callees' entry checks. That expectation needs setting up front.

Direct-call arguments add no check of their own (the callee's entry check runs
either way). So whether a strict scope should reject an unproved direct-call
argument is a separate question (open question 2).

## What a strict scope does

Inside a strict scope, an obligation that would keep a **fallback** check is an
**error** instead of `TUR-W0372` plus a check. Nothing else changes: the same
obligations are decided the same way, and severity is the only difference.
That is exactly what `--strict-refine` does, restricted to a region.

When the solver cannot decide, the error names the obligation, and the
programmer picks a way out. The ways out, from cheapest:

1. **Supply the fact.** Refine a parameter, add `:pre`, or branch on the
   condition. All three work today. A branch outside the loop often turns a
   per-iteration check into one check total, which is usually what the
   performance code wanted in the first place.
2. **Write the check explicitly.** The check still runs at run time, but it is
   in the source, at a place the programmer chose. This needs P1:
   `require!` / `assert!` do not inform the solver today.
3. **Assume it, unchecked.** An explicit, unsound escape hatch for a fact the
   programmer knows and the solver cannot see, like Rust's `get_unchecked`
   or Liquid Haskell's `assume`. Gated on `#fx{Unsafe}`. Last resort, and
   only if real code asks for it (P4).

## Today's behaviour, measured

With the in-tree Debug `tur` on 2026-10-27:

```turmeric
(load "stdlib/contract.tur")
(defn safe-div [n : int, d : #refine{ v : int | (not= v 0) }] : int (/ n d))

(defn via-if [d : int] : int          ; proved under --strict-refine
  (if (not= d 0) (safe-div 10 d) 0))

(defn via-require [d : int] : int     ; TUR-E0371 under --strict-refine
  (require! (not= d 0))
  (safe-div 10 d))
```

`require!` expands to a call to `tur-contract-check`, which the solver sees as
an opaque call. The fact it establishes (nothing after it runs unless the
condition held) never reaches the obligations that follow.

## Proposed spellings

**Function scope:** a function mark, the same slot as `^deprecated`:

```turmeric
(defn ^proved dot [a : Vec3, b : Vec3] : float ...)
```

`^tailcall` is the closest precedent: it turns a performance property into a
compile-time requirement. `^proved` is a working name. Alternatives:
`^strict-refine` (matches the flag) and `^no-fallback-checks` (says exactly
what it governs).

**Module scope:** a `build.tur` key listing modules, e.g.
`:strict-refine [hot/physics hot/raster]`, and possibly a per-file form, so a
whole hot module opts in without marking each function.

**Whole program:** `--strict-refine`, unchanged. It is the scope that covers
everything.

## Phases

### P0 -- Inventory and an audit report

- Confirm the fallback-check list above against `emit_fns.c` / `elab_fns.c`.
  In particular, settle the refined `let` binding.
- **Audit report.** For each function: the fallback checks kept (with the
  obligation and reason) and the declared checks. Either a summary over
  `--dump-refine=json` (which already records verdict and `runtime_guarded`
  per obligation) or a `tur check --report-checks`. This answers "what is this
  hot path paying for?" with no strictness at all, and it is useful whether or
  not the rest of this plan lands.

### P1 -- Explicit checks inform the solver

After `(require! p)` / `(assert! p)` returns, `p` holds: the check panics
otherwise. Make that a hypothesis for the rest of the enclosing body, as an
`if` guard already is. That gives way out 2 something to stand on, and it
helps ordinary code too.

Care point: the contract handler (`set-contract-handler!`) can be replaced,
and a handler that *returns* instead of panicking breaks "p holds after". The
fact is only sound if a failed check cannot fall through. Either guarantee
that (the handler's return is followed by an abort) or derive the fact only
when it is.

### P2 -- The function mark

- Parse the mark and carry it on the binding.
- Promote fallback-check `TUR-W0372` to an error for obligations whose site is
  inside a marked body.
- Decide: does a lambda inside the body inherit it? Do macro expansions?
  (Probably yes to both: the code runs in the hot function.)
- Fixtures: proved (silent), undecided (error), the three ways out, and a
  declared entry check that stays (no error: it is not a fallback check).

### P3 -- Module scope

The `build.tur` key, read where `--strict-refine` is consulted, plus the
per-file form if wanted.

### P4 -- Unchecked assumption (demand-driven)

`(assume! p)` under `#fx{Unsafe}`: a hypothesis with no runtime check. Only if
real code has a fact the solver cannot reach and an explicit check is too
expensive. Must be greppable, and must show in the P0 audit report.

## Open questions

1. **Spelling.** `^proved`, `^strict-refine` or `^no-fallback-checks`; and
   whether module scope is a `build.tur` key, a per-file form, or both.
2. **Direct-call arguments in a strict scope.** `--strict-refine` rejects an
   unproved argument even though no check is added there. Should the scope do
   the same, matching the flag, or govern only fallback checks, matching its
   stated purpose? The second is more precise; the first is less surprising
   to anyone who already knows the flag.
3. **Gating.** This only ever adds errors, and only where asked. That reads as
   diagnostic strictness, which the experimental-features rule exempts from
   `--enable=`. But a new function mark is new syntax. Gate it or not?
4. **`--no-contracts`.** No checks are emitted at all, so there is nothing to
   install. Should a strict scope still demand proofs there (it is a property
   of the code), or fall silent?
5. **Beyond refinements.** Other implicit runtime checks exist (division by
   zero through `tur_idiv`, container bounds). Performance code may want the
   same "tell me first" for those. Out of scope here, but the audit report
   (P0) is the natural place to list them later.
