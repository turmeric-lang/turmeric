# A cloneable continuation that escapes its shift receiver has no owner

**Severity: low (leak).** 488 B per escaped continuation: the 32-byte wrapper
plus its DK chain copy. A clone or snapshot the program takes and never drops
leaks the same amount. This is the residue of
[lexical-shift-receiver-cont-never-freed](../archive/lexical-shift-receiver-cont-never-freed.md)
(fixed 2026-10-12), and it mirrors the serial flavour's
[serial-cont-escaped-or-deserialized-chain-leaks](serial-cont-escaped-or-deserialized-chain-leaks.md).

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
