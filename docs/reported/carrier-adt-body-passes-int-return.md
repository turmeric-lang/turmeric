# A word-carried ADT body passes a `: int` return silently

**Severity: medium.** A type error is accepted, and the function returns a heap
address as its integer answer. Arithmetic on that "integer" gives garbage, and
nothing warns. The compiled engine rejects the same mistake for a by-value ADT
(TUR-E0709), so whether the checker catches it depends on how the ADT happens
to be represented. The interpreter accepts every shape.

Found 2026-10-24 by the quote-data corpus sweep (quote-list-data-plan item 4):
`errors/nil-tail-macro-expanded-definition`'s macro produced a `Datum` under
`--enable=quote-data`, and the `Datum` went out through `(defn f [] : int ...)`
with no error. `quote-data` exposed it but did not cause it. Reproduces on
`main` (`289d688d`) with no experiment enabled.

## Repro

```turmeric
(load "stdlib/list-typed.tur")
(defdata U (UA int) (UL (List int)))
(defn f [] : int (UA 1))
(defn main [] : int
  (println (f))
  (println (+ (f) 1))
  0)
```

| Body's ADT | `tur run` / `tur jit` | `tur --interpret` |
| --- | --- | --- |
| `(defdata Z (ZA int) (ZB Z))` (by-value) | TUR-E0709 | prints an address |
| `(defdata W :copy (WA int) (WB cstr))` | TUR-E0709 | prints an address |
| `U` above (holds a `(List ...)`) | **prints an address** | prints an address |
| `Datum` (`(quote (a b c))` under `--enable=quote-data`) | **prints an address** | prints an address |
| `cstr` body, for comparison | TUR-E0709 | -- |

The emitted C for `U` is `static int64_t f(void) { return ctor_U_UA(INT64_C(1)); }`.
`U` is carried as an `int64_t` word, so `cc` has nothing to object to either.

## Root cause

`return_type_carrier_aggregate_conflict` (`src/compiler/elab_core.c:3307`)
fires only when one side is a *non-carrier* aggregate, and
`rv_is_noncarrier_aggregate` (`elab_core.c:3279`) decides that from the C
spelling:

```c
if (t.kind != TY_ADT) return false;
const char *cn = type_c_name(t);
return cn && strcmp(cn, "int64_t") != 0;
```

A bare ADT whose C name is `int64_t` (a word-carried node, which is what an ADT
holding a `(List ...)` becomes, `Datum` included) is treated as the carrier
itself. So the check passes it as a "bridge" between two `int64_t`s. That is
true of the bits, but this is a type mismatch, not a representation question.
The same function returns `false` up front under `g_interpret_mode`, so the
interpreter never checks it at all.

The committed-only reverse pointer check
(`return_type_pointer_scalar_reverse_conflict`, `elab_core.c:3188`) already
draws the right line for `cstr`: in a monomorphic, non-`#{Unsafe}` defn there is
no carrier crossing to bridge, so a `cstr` body under `: int` is TUR-E0709. A
word-carried ADT body in the same position is the same defect and gets no such
check.

## Fix directions

1. **Committed-class check on the TYPE, not the C spelling.** In
   `return_position_conflict` (`elab_core.c`), beside the `RET_CONFLICT_TYPE_REVERSE` arm, add a
   committed-only arm: a concrete `TY_ADT` / ground `TY_APP` body under an
   integer-family declared return (and the reverse) is TUR-E0709, whatever
   `type_c_name` says. Generic, `#{Unsafe}` and instance-method positions keep
   today's tolerance, since they do have a carrier crossing.
2. **Under `--interpret`, too, for the committed class.** The
   `g_interpret_mode` early-out exists for
   `tests/fixtures/map-multiword-struct-key`, whose `:turi` reader-conditional
   arm returns a boxed ADT through an integer return on purpose. Check whether
   that function is in the committed class. If it is, it should spell the
   erasure (`(:: x int)` or `#{Unsafe}`) rather than keep the hole open for
   every program.
3. Expect corpus fallout: anything returning a node through `: int` as a
   stand-in handle is exactly CLAUDE.md's "no lazy `:int`" defect, and should
   get a real return type rather than a tolerance.

Out of scope here: a node passed as an *argument* to an `: int` parameter
(`(h (ZA 1))` with `(defn h [x : int] ...)` prints an address even for the
by-value `Z`). That is the elaborator's deliberate implicit erasing ascription
(CLAUDE.md, "Region Store Hooks"), a separate design decision, not this
check's gap.
