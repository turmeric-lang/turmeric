# A serial-cont that escapes its receiver, or is minted from bytes, is never freed

**Severity: low (leak; one chain per capture or per deserialize).**  Filed
2026-10-03 as the residue of
[serial-cont-chain-never-freed](../archive/serial-cont-chain-never-freed.md),
whose fix frees a receiver's chain only when the receiver *confines* `k`.

## Repro

Both shapes are in `tests/fixtures/serial-shift-colored-receiver`:

```turmeric
(load "stdlib/serial.tur")
(defn page [env : cstr hole : int] : int (+ hole 1))
(defn apply1 [^fat f : (fn [int] int) v : int] : int (f v))

;; 1. k captured by a lambda -- the walk cannot follow it, so it keeps the chain.
(defn helper [k : serial-cont] : int (apply1 (fn [x : int] : int (k x)) 3))
(defn run-repro [] : int
  (serial-reset (page "" (serial-shift (fn [k : serial-cont] : int (helper k)) 0))))

;; 2. a serial-cont minted by bytes->serial-cont is the caller's, and nothing
;;    frees it (nor the bytes serial-cont->bytes returned).
(defn roundtrip [k : serial-cont x : int] : int
  (match (bytes->serial-cont (serial-cont->bytes k))
    (Ok k2)  (k2 x)
    (Err m)  (do (println m) -1)))
```

Built the way `tests/run-leak-check.sh` builds, the fixture leaks ~3.9 KB:
`dk_new` under `tur_serial_cont_deserialize` / `bytes->serial-cont`, the
`tur_serial_cont_serialize` buffers, and the chains handed to `helper`.  The
fixtures that spell the round trip with the raw runtime calls
(`cps-oracle-serial-native-*`, `serial-context-*`:
`(tur_serial_cont_resume (tur_serial_cont_deserialize (tur_serial_cont_serialize k)) 5)`)
leak the same two things.

## Root cause

A `serial-cont` is a plain carrier word with no owner: `(k v)` resumes a copy,
so no use is a last use the compiler can see.  The fix for the receiver's chain
recovers ownership for one case by walking the receiver's body; outside it there
is nothing to walk.

- **Shape 1.**  `serial_k_use_ok` (`src/compiler/emit_cps_ir.c`) refuses a `k`
  captured by a lambda: the lambda's lifetime is not visible from the receiver
  (here `apply1` only calls it, but a callee that stores its fn argument would
  outlive the free).
- **Shape 2.**  `bytes->serial-cont` returns a fresh chain in
  `(Result serial-cont cstr)`; the program owns it and has no way to release it.
  The bytes from `serial-cont->bytes` are a bare `ptr<void>` with no free either
  (`bytes-free` takes the linear `Bytes`).

## Fix directions

1. **Shape 1, narrowly:** follow a lambda that captures `k` when it is passed
   only to callees whose fn parameter is itself confined (called, never stored)
   -- the same interprocedural walk, one level up.  Covers `helper`.
2. **Give `serial-cont` an owner** (the old report's direction 2): an affine
   continuation consumed by resume, cloned explicitly, with a drop, makes every
   case syntactic -- including a deserialized one.  A surface change.
3. **Shape 2, cheaply:** a `serial-cont-free` (and typing
   `serial-cont->bytes`'s result as `Bytes` so `bytes-free` applies) gives a
   program that knows its continuation is dead a way to say so.
