---
title: Returning `Result` / `Option` from Inline-C
category: Interoperability
description: Build typed Result/Option values inside inline-C bodies with the preamble helpers (tur_ok_ptr, tur_err_int, tur_some_ptr, tur_none, ...) instead of hand-rolling the struct or returning a magic-sentinel :int
---

# Returning `Result` / `Option` from Inline-C

A fallible C constructor -- one that allocates or acquires a handle in C
and can fail (open a MIDI port, connect a socket, parse a file) -- should
return a real `(Result Handle E)` or `(Option Handle)`, **not** a
`:ptr<void>` and **not** a magic-sentinel `:int` (`-1`, `0`-as-absent,
`INT64_MIN`). A sentinel throws away the type the checker could otherwise
enforce, and forces every caller to remember the convention.

You do not have to hand-roll the result struct. Every emitted translation
unit carries a small set of preamble helpers that build and inspect
Option/Result values through the **canonical** heap layout -- the same one
[`stdlib/option.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/option.tur)
and [`stdlib/result.tur`](https://github.com/turmeric-lang/turmeric/blob/main/stdlib/result.tur)
use -- so a value built in C flows straight into the stdlib accessors
(`ok?`, `err?`, `ok-val`, `err-val`, `some?`, `unwrap`) and vice versa.

The builders return the `int64_t` **carrier** -- a heap pointer to the
canonical layout. That is still what an inline-C body should hand back, and
nothing here changed when concrete monomorphs started flowing by value: the
compiler bridges the carrier into the aggregate at the boundary, and a
`(none)` carrier (the null pointer) survives the crossing. What the boundary
needs is a real declared type on the C function -- `: (Result MidiIn int)`,
not `: int`. A producer declared `: int` hands back a bare word the accessors
cannot type, so its callers have to write `(ok? (:: r (Result int int)))` at
every use site.

## The helpers

The builders come in three flavours. Prefer the **typed** `_int` / `_ptr`
builders: they spell out the payload's cast direction at the call site, so
an inline-C author never has to remember whether a pointer payload needs an
`(int64_t)(intptr_t)` widening.

| Helper | Builds | Payload |
|--------|--------|---------|
| `tur_ok_ptr(void *p)`   | `(Result A B)`, ok  | pointer handle, widened for you |
| `tur_ok_int(int64_t v)` | `(Result A B)`, ok  | integer code, passed as-is |
| `tur_err_ptr(void *p)`  | `(Result A B)`, err | pointer handle, widened for you |
| `tur_err_int(int64_t e)`| `(Result A B)`, err | integer code, passed as-is |
| `tur_some_ptr(void *p)` | `(Option A)`, some  | pointer handle, widened for you |
| `tur_some_int(int64_t x)`| `(Option A)`, some | integer payload, passed as-is |
| `tur_none()`            | `(Option A)`, none  | -- (NULL) |

`tur_none()` is the function-call companion to the `TUR_NONE` macro; use
whichever reads better next to the typed builders around it.

Inspectors, for an inline-C body that *consumes* a Result/Option built
elsewhere (the carrier is the `int64_t` the opaque/Result lowered to):

| Helper | Reads |
|--------|-------|
| `tur_is_ok(int64_t r)`    | `bool` -- is the result ok? |
| `tur_ok_value(int64_t r)` | the ok payload (as `int64_t`) |
| `tur_err_value(int64_t r)`| the err payload |
| `tur_is_some(int64_t o)`  | `bool` -- is the option some? |
| `tur_opt_value(int64_t o)`| the some payload |

### The untyped `tur_box_*` builders

The typed builders are thin wrappers over the original carrier-level
builders, which take the raw `int64_t` carrier directly:

| Helper | Builds |
|--------|--------|
| `tur_box_ok(int64_t v)`   | `(Result A B)`, ok, payload `v` |
| `tur_box_err(int64_t e)`  | `(Result A B)`, err, payload `e` |
| `tur_box_some(int64_t x)` | `(Option A)`, some, payload `x` |
| `TUR_NONE`                | the none `(Option A)` (NULL) |

These remain valid and are how older inline-C blocks in the tree call into
the layout. For a pointer payload they require the explicit
`tur_box_ok((int64_t)(intptr_t)h)` cast -- which is exactly the friction
`tur_ok_ptr(h)` removes. Reach for `tur_box_*` only when the payload is
already an `int64_t` carrier you are forwarding unchanged; otherwise prefer
the `_int` / `_ptr` builder that names your intent.

## Worked example: an rtmidi-shaped port constructor

A C MIDI binding wraps `RtMidiInPtr` -- an opaque library handle that
`rtmidi_in_create_default()` returns and that the close call consumes. The
constructor can fail (the backend is unavailable, the requested port index
is out of range), so it returns a typed `(Result MidiIn int)`: ok carries
the handle, err carries an integer status code.

```turmeric
;; A linear opaque over the C library handle -- consumed exactly once by
;; midi-in-close. See opaques-guide.md for the :linear discipline.
(defopaque MidiIn :ptr<void> :linear)

;; midi-in-open : port -> (Result MidiIn int)
;;   ok  = the live RtMidiInPtr, built with tur_ok_ptr (no hand cast)
;;   err = an integer status code, built with tur_err_int
(defn midi-in-open [port : int] : (Result MidiIn int)
  ```c
  RtMidiInPtr h = rtmidi_in_create_default();
  if (h == NULL)        return tur_err_int(1);   /* backend unavailable */
  if (!h->ok)           { rtmidi_in_free(h); return tur_err_int(2); }
  unsigned n = rtmidi_get_port_count(h);
  if ((unsigned)port >= n) { rtmidi_in_free(h); return tur_err_int(3); }
  rtmidi_open_port(h, (unsigned)port, "turmeric-in");
  return tur_ok_ptr(h);
  ```)

;; midi-in-close : consume the handle (linear: exactly one call).
(defn midi-in-close [m : MidiIn] : void
  ```c
  RtMidiInPtr h = (RtMidiInPtr)(intptr_t)m;
  rtmidi_close_port(h);
  rtmidi_in_free(h);
  ```)
```

The consumer is plain Turmeric -- the stdlib accessors read the C-built
value with no special handling:

```turmeric
(defn with-first-port [] : int
  (let [r (midi-in-open 0)]
    (if (ok? r)
      (let [m (ok-val r)]
        (do
          (poll-loop m)
          (midi-in-close m)
          0))
      (err-val r))))          ;; surface the status code on failure
```

An `(Option MidiIn)` "open the default port if there is one" variant uses
`tur_some_ptr` / `tur_none` the same way:

```turmeric
(defn midi-in-default [] : (Option MidiIn)
  ```c
  if (rtmidi_get_port_count_default() == 0) return tur_none();
  RtMidiInPtr h = rtmidi_in_create_default();
  rtmidi_open_port(h, 0, "turmeric-in");
  return tur_some_ptr(h);
  ```)
```

## Why this beats the alternatives

This is the blessed replacement for two anti-patterns that used to spread
through spices (see
[docs/archive/history/no-stdlib-result-builder-for-inline-c.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/history/no-stdlib-result-builder-for-inline-c.md)):

- **Re-declaring the struct in raw C** -- `struct { bool is_ok; int64_t
  ok_val; int64_t err_val; } *r = malloc(...)` returned as `:ptr<void>`.
  This duplicates the layout, drifts silently if the canonical layout ever
  changes, and discards the `Result` type. **The drift is no longer
  hypothetical: SR2b changed the canonical layout to the tagged sum
  `{ int tag; union { ... } as; }`, so any surviving hand-rolled copy of the
  struct above is now reading the wrong bytes.** The preamble carries a
  `_Static_assert` pinning `tur_option_t` / `tur_result_box_t` to their
  byte layout precisely so the *helper* path cannot drift; a hand-rolled
  copy gets no such guard.
- **Aborting on failure** -- `fprintf(stderr, ...); abort()`. That is the
  right call for a genuinely unrecoverable allocation (a control block the
  process cannot run without -- this is why `threadpool-new` /
  `task-group-new` abort), but it is wrong for routine, recoverable
  failures like a port that is busy or a connection that is refused.

## Who owns the box

`tur_some_ptr` / `tur_ok_ptr` / `tur_box_*` **malloc** the option/result
carrier box. Until 2026-08-30 nothing freed it: the allocation happens inside a
C body, so no elaborated expression corresponded to it and the compiler had
nothing to give ownership to -- every call through the idiom this guide
recommends leaked one box.

That is fixed, and the fix is a contract worth stating explicitly:

> **A function whose body is inline C and whose DECLARED return type is
> `(Option T)` / `(Result T E)` transfers ownership of the box to its caller.**
> The compiler frees it at the point the value is read back into an ordinary
> Turmeric value.

Two consequences for anyone writing such a body:

- **Return a FRESH box.** `tur_some_ptr(...)`, `tur_ok_ptr(...)`, `tur_none()`
  (which is the null carrier and allocates nothing) all satisfy this. A body
  that cached a box in a static and returned it twice would hand the same
  allocation to two owners -- a double free, not a leak.
- **Returning a BORROWED box needs a different signature.** If the box belongs
  to something else -- a container, a cache -- do not declare the result
  `(Option T)`. Declare the element type and let the caller wrap it, which is
  what `vec-get [A] (v : (Vec A)) : A` does: the vector owns the box and frees
  it in `vec-free`, and the borrow-shaped signature is what tells the compiler
  so.

The distinction is the DECLARED type, not the type at a particular call site:
`(:: (vec-get v 0) (Option int))` resolves to an Option and is still a borrow.

**The payload follows the box (since 2026-09-03).** A body that boxes a
*value-struct* payload as a pointer -- `Box *b = malloc(sizeof *b); ...;
return tur_box_ok((int64_t)(intptr_t)b);` for a declared `(Result Box cstr)`
-- hands that allocation over too: the monomorph stores such a payload as a
pointer slot, and the compiler frees the live arm together with the cell once
the value has been read back or its consumer has returned. So the payload must
be a FRESH allocation as well; boxing the address of a static or of something
else's field is a double free, not a leak. This is the same rule the
freshness analysis applies to a Turmeric producer, extended to inline C by
declaration (`elab_stamp_sum_freshness`).

## Limitation: only `_int` and `_ptr` payloads

v1 ships the monomorphised `_int` / `_ptr` builders, which cover an integer
error code or an opaque pointer handle -- the shape every audited spice
site needs. A `(Result MyStruct MyErr)` where `MyStruct` / `MyErr` are
user-defined *by-value* types is **not** constructible from inline-C with
these helpers: the payload has to fit the single `int64_t` carrier slot.
Wrap the value behind an opaque pointer handle (the rtmidi pattern above)
or construct the `Result` in Turmeric instead.

## Nesting the builders: `(Result (Option T) E)` and friends

The builders compose. A body declared to return a sum whose payload is itself
a sum builds the inner value first and hands its box to the outer builder:

```turmeric
(defn read-line-ish [n : int] : (Result (Option cstr) IoError)
  ```c
  if (n < 0) return tur_err_int(-n);
  if (n == 0) return tur_ok_int(tur_none());       /* (Ok (None)) */
  return tur_ok_int(tur_some_ptr((void *)"line")); /* (Ok (Some "line")) */
  ```)
```

The same goes for `(Option (Result T E))`, `(Result (Result T E) E2)` and
deeper nestings (`tur_ok_int(tur_some_int(tur_some_int(n)))`). The inner
boxes belong to the outer one: the compiler converts the whole value when it
reads it back and frees every box, so the same freshness rule applies to each
level.

Before 2026-10-07 this compiled and silently misbehaved -- the readback treated
the builders' box as the monomorph's by-value layout, read the inner tag out
of a pointer, and neither arm of a nested `match` fired
([docs/archive/inline-c-builders-cannot-nest-option-in-result.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/inline-c-builders-cannot-nest-option-in-result.md)).
`tests/fixtures/inline-c-result-nested-sum` pins it.

## A control form around an `if` over these builders

The builders below return the int64 CARRIER, and the consumer bridges it to the
by-value aggregate. When an `if`'s arms are both carrier producers, `emit_if`
bridges each arm into its merge temp -- and a `let` or `do` wrapping that `if`
used to bridge the already-concrete result a second time:

```c
__t172 = (*(tur_adt_Result__Handle__int *)(intptr_t)(__t174));  /* already a struct */
```

`tur check` was silent; it failed at `cc` with `operand of type 'tur_adt_...'
where arithmetic or pointer type is required`, naming no `.tur` line. The same
`if` as the whole function body always worked, which is what made the workaround
("hoist the block into its own defn") effective and the cause obscure.

Fixed 2026-09-02; both the `let` and `do` wrappers are pinned by
`tests/fixtures/control-form-around-if-carrier-arms/`. Nothing about how you
write the inline C changes -- this is recorded because the shape it broke is the
one this guide recommends, so an older compiler will still reject it. See
[docs/archive/control-form-around-if-double-unboxes-carrier-arms.md](https://github.com/turmeric-lang/turmeric/blob/main/docs/archive/control-form-around-if-double-unboxes-carrier-arms.md).

## See also

- [opaques-guide.md](opaques-guide.md) -- the `defopaque` handles these
  constructors hand back, and the `:linear` / `:affine` discipline.
- [c-integration-guide.md](c-integration-guide.md) -- inline-C blocks, the
  `int64_t` carrier, and the FFI boundary in full.
- [error-handling-guide.md](error-handling-guide.md) -- `Result` / `Option`
  on the Turmeric side: the accessors these C-built values flow into.
- `tests/fixtures/inline-c-result-builder/` -- end-to-end fixture
  exercising every typed builder against the stdlib accessors.
