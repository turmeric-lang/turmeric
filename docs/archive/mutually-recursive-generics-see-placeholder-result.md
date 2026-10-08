# Mutually recursive generics see each other's result as the `int` placeholder

**Severity: low-medium.** An expressiveness gap: a correct program is refused
at `tur check`.  Filed 2026-10-02, the residue of
[forward-call-to-generic-callee-typed-as-placeholder](forward-call-to-generic-callee-typed-as-placeholder.md),
whose fix orders a caller after its generic callee -- which a cycle cannot do.

**Status: RESOLVED 2026-10-02**, the same day, by a variant of fix direction
1 that needs no new forward-declaration code.  See [Resolution](#resolution).

## Repro

```turmeric
(defn ping [A] [n : int x : A] : (Option A)
  (if (= n 0) (some x) (pong (- n 1) x)))
(defn pong [A] [n : int x : A] : (Option A)
  (if (= n 0) (some x) (ping (- n 1) x)))
(defn main [] : int
  (match (ping 3 7)
    (Some v) (println v)
    (None) (println "none"))
  0)
```

```
error: if branches have mismatched types: then=(Option A) else=int
```

The same with `7.1`, and inside a `defmodule`.  Reproduces before and after
the 2026-10-02 fix.  A cycle whose result is the bare type variable (`: A`)
works, at int, float and a struct: the placeholder and `A` meet as words.

## Mechanism

Whichever member is elaborated first calls the other through its pass-1
forward decl.  For a result that mentions the defn's own type parameters both
pre-passes keep the `TY_INT` placeholder (`fwd_type_mentions_tp` in
`src/compiler/elab_toplevel.c`), since the decl has no parameter types to
instantiate `A` from.  `FwdGenOrder` breaks a cycle at its first generic
member, so that member still sees its partner's forward decl.

## Fix directions

1. Forward-declare a generic in full -- type parameters, parameter types,
   constraints -- for the cycle members only (direction 1 of the parent
   report, scoped down to what a cycle needs).
2. Within one call, instantiate a self-referential cycle's forward decl from
   the CALLER's own signature: a member calling its partner with its own `A`
   could commit `(Option A)` with the caller's `A`, the way a self-call
   already does.

## Resolution

`elab_defn` already forwards a defn's declared signature onto its binding
before it elaborates the body -- the early update that lets a generic call
itself (`Phase HRT5` / `RR1` in `src/compiler/elab_fns.c`) -- and that update
survives a body that fails.  So when `fwd_gen_order_drain` finds the waiting
defns stuck on a cycle, it now first PRIMES the cycle's lossy members: each
is elaborated once under a capture frame (`tl_probe_slot` / `md_probe_slot`),
kept if that succeeds and rolled back if not -- the rollback the symptom-A
instance deferral already uses.  After one round every member's binding
carries its full signature, so the member elaborated for real sees `(Option
A)` where it saw the `int` placeholder.  In the repro, `ping`'s attempt fails
and is rolled back; `pong`'s then succeeds against `ping`'s primed binding and
is kept; `ping` follows, against `pong`'s.  A cycle whose result is the bare
`A` never needed this and elaborates as before.

The one cost is the symptom-A one: a rolled-back attempt's WARNINGS pass the
capture frame, so a warning in a primed member's body can print twice.

Pinned by the `count-opt` / `ping-opt` / `pong-opt` cycle in
`tests/fixtures/forward-call-generic-callee` and
`forward-call-generic-callee-in-module`.  Suite 3489/0, turi 2521/0.

