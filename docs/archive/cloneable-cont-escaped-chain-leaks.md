# A cloneable continuation that escapes its shift receiver has no owner

> **RESOLVED 2026-11-28 (fix direction 1): a `let` owns the escaped `k` it
> binds and only reads.** `emit_let_binding_ccont_owned`
> (`src/compiler/emit_cps_ir.c`) admits a binding whose init is a fresh
> continuation nothing else frees -- a `cloneable-reset` whose one shift is
> reached on every path and whose receiver returns its `k` and names it
> nowhere else, a `tur_cloneable_cont_clone` / `tur_continuation_snapshot`, or
> a call to a function every result of which is one of those (`(make 5)`) --
> when every use of it in the let is a K_CLONE read (`serial_k_use_ok`: a
> resume, a clone, a snapshot, a callee that does the same). The direct
> emitter drops it at the let's scope exit (`tur_cloneable_cont_drop`); a
> colored function's let has no scope exit, so `cps_bind_let_init`
> (`src/passes/cps_ir.c`) registers it with the outermost DK entry's reap list
> (reap kind 4, `__dk_reap_ccont`), which runs after every resume -- a
> `^multishot` handler that re-runs the let's tail drops it once. The
> elaborator binds a handle passed straight to a resume --
> `(tur_cloneable_cont_resume (tur_cloneable_cont_clone c) 10)`,
> `(tur_cloneable_cont_resume (make) 10)` -- so the same rule reaches it.
>
> A program that drops the value itself (`tur_cloneable_cont_drop` is not a
> K_CLONE read), returns it, stores it or captures it keeps that job, as the
> delimited-control guide says. Three shapes stay conservative and leak as
> before: a shift the reset may not reach -- `(cloneable-reset (if c (+ 1
> (cloneable-shift k-id 0)) 42))`, whose value may be an `int`
> (`cps-oracle-cloneable-native-shape2-if`); a read the K_CLONE walk does not
> follow, such as a resume inside a `handle` body
> (`cps-join-after-cloneable-reset-kont`); and a reset reached through a
> local lambda, `(tur_cloneable_cont_resume (f) 42)`
> (`cloneable-capture-precision`).
>
> Of the 64 fixtures that use a cloneable continuation, 34 that leaked are
> leak-clean now and 7 leak less; none reports an ASan error, and all print
> the same. Most of what the rest leak is other owners' business: the env
> clone of a `:heap` capture (`cloneable-owning-consuming-carrier`), and the
> serial round trip (`context-call-frame`, `context-division`). Pinned by
> `cloneable-cont-escaped-owned`
> (both engines) and `cloneable-cont-escaped-owned-multishot` (the reap
> path; compiled only -- the interpreter cannot run that handler multi-shot,
> on `main` too); `cloneable-cont-escaped-dropped` still pins the program's
> own drop.

## As filed

**Narrowed 2026-11-06: fix direction 2 is done -- the rule is written
down.** The delimited-control guide says, under "Who frees a cloneable `k`",
that a `k` that leaves its receiver is the program's to drop with
`tur_cloneable_cont_drop`, clones included, and shows the repro below with
the drop. The leak-checked fixture `cloneable-cont-escaped-dropped` pins
that the documented program is clean (a resume, a clone and its resume, both
drops). What is left is fix direction 1: an automatic owner.

**Severity: low (leak).** 488 B per escaped continuation: the 32-byte wrapper
plus its DK chain copy. A clone or snapshot the program takes and never drops
leaks the same amount. This is the residue of
[lexical-shift-receiver-cont-never-freed](../archive/lexical-shift-receiver-cont-never-freed.md)
(fixed 2026-10-12), and it mirrors the serial flavour's
[serial-cont-escaped-or-deserialized-chain-leaks](../archive/serial-cont-escaped-or-deserialized-chain-leaks.md).

## Repro

```turmeric
(defn k-id [k] : int k)                       ; the receiver returns k
(defn make [base : int] : int
  (cloneable-reset (+ base (cloneable-shift k-id 0))))
(defn main [] : int
  (let [c (make 5)]
    (println (tur_cloneable_cont_resume c 1)))  ; 6
  0)
```

Built the way `tests/run-leak-check.sh` builds, this leaks 488 B. Adding
`(tur_cloneable_cont_drop c)` after the resume makes it clean. That call has
been emitted since the same fix; before it, `tur_cloneable_cont_drop` in
statement position compiled to nothing.

38 of the 99 fixtures that use `shift`, `cloneable-shift` or `call/cc*`
still leak for this reason, for example the `cloneable-owning-*` family
(`k-identity` returns `k`), `call-cc-star` and
`cps-oracle-cloneable-native-shape2-*`.

## Root cause

The shift body drops the wrapper after the receiver returns only when the
receiver provably keeps nothing of `k` (`serial_recv_confines_k`, role
`K_CLONE`, `src/compiler/emit_cps_ir.c`). A `k` the receiver returns, stores
or captures escapes, and the compiler cannot see the last use of it. The
continuation is an `int` carrier (a `cont` typed `int`, or untyped), so no
scope exit drops it.

## Fix directions

1. **Give an escaped cloneable continuation an owner.** For example, a
   `cont` value whose binding drops it at scope exit unless it is moved, the
   way an `rc` is. It is the same design item the serial report names.
2. **Until then, the program owns it:** `tur_cloneable_cont_drop` an escaped
   `k` and every clone. The effects guide could say so where it introduces
   `call/cc*`.
