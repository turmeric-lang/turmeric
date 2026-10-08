# `tur --interpret`: every call copies `any` boxes and never-written structs, so arguments are most of what an r7rs run keeps

**RESOLVED 2026-10-07** by fix directions 1 and 2 together.
`turi_copy_byvalue_struct_arg` (`src/turi/eval.c`) now copies an argument only
when `turi_struct_arg_may_be_written` says a write could reach it: the value's
ADT has a field write somewhere in the program, or a by-value struct inside it
does. Turmeric writes a struct field only with `(set! (.f x) v)`, and its one
construction site (`elab_set_field`, `src/compiler/elab_forms.c`) now sets
`AdtDef.field_written` on the receiver's ADT. An `any` box is never written
itself (EX_ANY_CAST only reads its payload), so it is exactly as writable as
its payload: a box over a symbol, a char or an int is never copied, and a box
over `R7rsNull` is not either, since no program writes that type. `__rc` and
`:heap` values stop the walk as before; a struct without a constructor record
is assumed writable and still copied.

Measured, Release `tur`: `r7rs-srfi-14` peaks at **48 MB, down from 132 MB**,
same output. A symbol and `'()` passed through 600,000 calls grow peak RSS by
0 MB (was ~32 MB Release; 78 MB Debug with ASan's quarantine off). The
compiled backend is untouched and still agrees on write semantics: a `^mut`
parameter written directly, a by-value field written through its own
parameter, and a callee's `(let [^mut q p] (set! (.x q) ...))` all leave the
caller's value alone on both back ends.

Pinned by the second program in `tests/check-turi-frame-reclaim.py` (ctest
`tur_turi_frame_reclaim`), which bounds that growth at 16 MB, with ASan's free
quarantine off so a Debug `tur` measures live memory. `tests/run-turi.sh`
(2652) and `tests/run.sh` (3604) are green.

**Not done:** direction 3 (copy on write). One known looseness of the flag:
under the REPL, a field write elaborated in a later turn does not reach values
an earlier turn already passed uncopied. Observing it needs the earlier value
stored somewhere long-lived and then written through a `^mut` alias.

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
