# `tur --interpret`: every call copies `any` boxes and never-written structs, so arguments are most of what an r7rs run keeps

**Severity:** medium (interpreter memory and time). Under `tur --interpret`,
every by-value struct argument is deep-copied into the value pool at every
call. Most copies are of values nothing can write through, such as an `any`
box holding a symbol or the r7rs empty list. None of them is ever freed. Now
that call frames are reclaimed
([turi-call-frames-never-reclaimed](../archive/turi-call-frames-never-reclaimed.md)),
these copies are the largest thing an ordinary r7rs loop still keeps. They
are 67% of `r7rs-srfi-14`'s peak heap. Found 2026-10-05.

## Repro

```sh
valgrind --tool=massif --depth=3 ./build-release/tur --interpret tests/fixtures/r7rs-srfi-14/input.tur
# peak 132 MB; 60.0% + 6.9% under turi_copy_byvalue_struct_arg (eval.c:1427/1433)
```

Counting the copies by struct name (a temporary `fprintf` in
`turi_copy_byvalue_struct_arg`, not checked in):

| struct | `is_any_box` | copies in `r7rs-srfi-14` |
| --- | --- | --- |
| `Sym` | yes | 1,465,467 |
| `R7rsNull` | no (by-value ADT, 1 field) | 93,619 |
| `R7rsChar` | yes | 7,827 |
| `Vec` | yes | 353 |

Every copy is a `TuriStruct` plus its field array, about 50 bytes, and goes
into `value_scratch` for the life of the process.

## Root cause

`turi_copy_byvalue_struct_arg` (src/turi/eval.c:1422) runs on every argument
the three call-frame bind loops bind. It came from
[struct-param-mutation-backend-divergence](../archive/struct-param-mutation-backend-divergence.md):
a callee's `set!` through a by-value struct parameter must not reach the
caller, as on the compiled side. The function stops only at an `__rc`
wrapper and a `:heap` ADT, so it copies:

- **`any` boxes** (`is_any_box`). `turi_any_box_widen` makes one for every
  value widened to `any`, and r7rs code is all `any`. A box has one field and
  no surface that writes it. `EX_ANY_CAST` is its only unwrap, and that reads
  `fields[0]`. When the payload is not a struct (a `Sym`, a `char`, an int),
  the copy can never be observed.
- **By-value ADTs that nothing writes.** `R7rsNull` is the prelude's empty
  list. No program writes a field of it, yet every call that passes `'()`
  copies it.

## Fix directions

1. **Skip `any` boxes with a scalar payload.** Return `v` unchanged when
   `src->is_any_box` and `fields[0]` is not a `TURI_STRUCT`. When the payload
   is a by-value struct, keep the copy, because an unwrapped payload can be
   written. This alone removes ~94% of the copies above.
2. **Skip types the program never writes.** The elaborator sees every
   `set!` / `set-field!` target. Record, per struct and ADT definition,
   whether any field write in the unit can reach a value of that type. Copy
   only those types. The prelude's r7rs types would then never copy.
3. **Copy on write instead of on call.** Mark a struct shared when it is
   bound as an argument, and copy it at the first field write through that
   binding. This matches the compiled semantics with no copy in the common
   case. It is the larger change.

## Pinned by

Nothing yet. `tests/check-turi-frame-reclaim.py` could grow a second program
that passes symbols and `'()` through 1e5 calls, with an RSS bound against
`TUR_TURI_FRAME_RECLAIM=0`.
