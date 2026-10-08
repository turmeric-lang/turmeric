# A serial-cont minted from bytes is never freed

**RESOLVED 2026-10-07** by fix direction 3 (shape 1 was fixed 2026-10-03).
`stdlib/serial.tur` gives a program three ways to release what a round trip
made:

- `serial-resume-owned k v` resumes a continuation from `bytes->serial-cont`
  and then `dk_free`s its chain.  The direction as written ("a
  `serial-cont-free`") did not fit the type: a `serial-cont` is affine, so
  `(serial-resume k2 x)` MOVES `k2` and nothing can name it afterwards
  (TUR-E0005).  Resuming and freeing in one consuming call is the shape that
  composes.
- `serial-cont-free k` drops a rebuilt continuation that is never resumed.
- `serial-bytes-free b` frees a `serial-cont->bytes` / `save-cont!` /
  `cont-from-file` buffer.  `serial-cont->bytes` keeps returning `ptr<void>`:
  retyping it as the linear `Bytes` would break every `cont-to-file` caller.

All three are documented as for the program's own values only; the
receiver's `k` stays the compiler's. Pinned by
`tests/fixtures/serial-cont-roundtrip-freed` (`requires.leak-check`): a
round trip resumed and released, and one buffer rebuilt twice with one copy
dropped and one resumed, leak-clean under `tests/run-leak-check.sh`.
Documented in `docs/guides/serializable-continuations-guide.md` ("Who frees
what").

**Left allocated on purpose:** a rebuilt frame whose env was a `cstr` keeps
its rebuilt string (the deserializer mallocs one per frame).  Freeing it with
the chain would dangle any copy the resumed computation kept; giving those
frames an owning env (`dk_frame_owning` with a strdup clone) is the way to
close it if it matters.  Direction 2 (an owned, cloneable `serial-cont`) stays
the general answer.

**Narrowed 2026-10-03 (same day): shape 1 is fixed** -- a `k` captured by a
lambda that the callee only calls is now followed (see *Fixed*).  What is
left is shape 2: a `serial-cont` minted by `bytes->serial-cont`, and the bytes
`serial-cont->bytes` returns.

**Severity: low (leak; one chain per deserialize).**  Filed
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

1. ~~**Shape 1, narrowly:** follow a lambda that captures `k` when it is passed
   only to callees whose fn parameter is itself confined (called, never stored)
   -- the same interprocedural walk, one level up.  Covers `helper`.~~ Done,
   below.
2. **Give `serial-cont` an owner** (the old report's direction 2): an affine
   continuation consumed by resume, cloned explicitly, with a drop, makes every
   case syntactic -- including a deserialized one.  A surface change.
3. **Shape 2, cheaply:** a `serial-cont-free` (and typing
   `serial-cont->bytes`'s result as `Bytes` so `bytes-free` applies) gives a
   program that knows its continuation is dead a way to say so.

## Fixed (2026-10-03): shape 1

`serial_k_use_ok` (`src/compiler/emit_cps_ir.c`) takes a role: `K_SERIAL` for
the receiver's `k`, `K_FNPARAM` for a fn value that may only be CALLED.  A
lambda capturing `k` (through the same Binding, checked against its capture
set) is accepted when its own body confines `k` and the value is then only
called -- as a call argument, the callee's parameter is walked in the
`K_FNPARAM` role; as a `let` init (the elaborator hoists a lambda argument that
way), the rest of the `let` is.  A callee that stores, returns or re-captures
the lambda keeps the chain, as before.  `serial-cont-receiver-chain-freed`
gained `recv-lam` (through `apply1`) and `recv-lam2` (through `apply2`, which
passes it on and calls it): both freed, leak-clean.

Following this shape found two use-after-frees on the CPS and fat-closure
paths, fixed in the same change -- see
[cps-reaped-closure-kept-by-callee](../archive/cps-reaped-closure-kept-by-callee.md).
