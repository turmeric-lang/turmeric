# An instance method returning `(Cons a)` from a `match` emits an unbridged fall-through `return`

**Resolved 2026-10-14: the report is archived.** The repro builds under gcc
and clang and prints `1`. The fall-through now reads
`return (int64_t)(intptr_t)__t70;`.

**The emitter was not the one this report named.** The line comes from
`emit_tail`'s `EX_MATCH` arm (proper-tail-calls T3, `emit_fns.c`), not
from the final `else` of the return ladder. A TCO-routed tail match emits
each arm through `emit_tail`, so each arm's return takes the full ladder
(`emit_fn_return_spelling`) and is bridged. The arm then spelled the
fall-through `return <temp>;` itself, raw. The ladder already had the right
rule: a bare temp whose *recorded* C type is a pointer, returned from an
int64 function, gets `(int64_t)(intptr_t)`. The match temp is recorded,
too. The fall-through just never asked the ladder. It now does, with the
match as the tail. That is how a match that isn't TCO-routed is returned,
and how every other `emit_tail` leaf has worked since
emit-tail-return-path-lacks-carrier-bridges. A nil-typed match keeps the
plain spelling.

**The variants the report asked for:**

- A `(Vec a)`-returning method was already fine on the previous commit, as
  were an `(Option a)`, a `float` and an `int` method. The `(Cons a)` one
  was the only one that failed.
- **Found on the way, and worse:** a *literal-pattern* match declared its
  result temp from the match's static type alone, while the ADT path
  resolves it through the active spec (M7 / SR2b). Returning the type
  parameter, `(defn pick [T] [c : bool a : T b : T] : T (match c true a
  false b))`, therefore printed **`7` for `(pick true 7.1 0.5)`**. A
  double was stored into an `int64_t` temp, with no diagnostic. At cstr,
  or with an `(Option T)` result, it was a cc error instead.

  Both paths now take the result type from one helper,
  `match_result_temp_type` (`emit_expr.c`). The literal path also records
  its temp's C type, as the ADT path does, so the arms' stores and the
  return ladder read the real spelling.

Pinned by `tests/fixtures/match-result-temp-typed-return`, under `run.sh`
(gcc 13, clang 18) and `run-turi.sh`. It has the repro, the four other
class results, a generic tail match, and the literal-pattern shapes: a 7.1
float, an `int`, a cstr, a let-bound match, and an `(Option T)` result at
int and at 2.5. No `expected.c` moved; full `run.sh` was 3801 passed, 0
failed. The plan's `Plated` class (ZP3), with a `match`-bodied instance
for a `:heap` tree, now builds and runs.

**Filed:**
[none-arm-not-grounded-by-sibling-some-arm](../reported/none-arm-not-grounded-by-sibling-some-arm.md).
A *value-position* literal match whose arms are `(None)` and `(Some x)`
still fails, because the two arms build different representations. That
is an inference gap, and the `if` form is a type error.

The original filing follows.

**Severity: medium.** This is a build failure under clang 15+ and gcc 14
(`-Wint-conversion`). gcc 13 warns, and `tests/run.sh` fails a fixture on
that warning. The offending line is unreachable, so the program is correct
wherever it compiles.

**Discovered:** 2026-10-13, prototyping
[generic-zipper-plan](../upcoming/generic-zipper-plan.md) (its D4, which
blocks the plan's ZP3 `Plated` class), on `main` at v0.64.4 (`704cc531`).

## Repro

```turmeric
(defdata R :heap :copy (Lf int) (Br (Cons R)))
(defclass Kids [a] (kids [x : a] : (Cons a)))
(definstance Kids [R]
  (kids [r] (match r (Lf _) (tnil) (Br cs) cs)))
(defn main [] : int
  (println (tlength (kids (Br (tcons-of (Lf 1) (tnil))))))
  0)
```

`CC=clang tur run`:

```
error: incompatible pointer to integer conversion returning
'tur_adt_Cons__R *' from a function with result type 'int64_t' [-Wint-conversion]
```

## What is emitted

```c
static int64_t __inst_Kids_kids_R(int64_t r) {
        tur_adt_Cons__R * __t70 = {0};
        {
            tur_adt_R *__scrut = (tur_adt_R *)(intptr_t)(r);
            switch (__scrut ? __scrut->tag : 0) {
            case 0: { ... return (int64_t)(intptr_t)__ps_N; ... }   /* bridged */
            case 1: { ... return (int64_t)(intptr_t)cs_N; ... }     /* bridged */
            default: break;
            }
        }
        return __t70;                                               /* NOT bridged */
}
```

The instance method's C return type is the `int64_t` carrier. The tail
`match` returns from each arm, and those returns are bridged. The match's
result temp is declared with the method's typed result, `(Cons R)`, that
is `tur_adt_Cons__R *`. The trailing `return __t70;` after the `switch`
(reached only by `default`) is passed through as is.

## Root cause

The trailing `return` comes from the final `else` of the return emission in
`src/compiler/emit_fns.c` (`:5070`, `buf_printf(out, "return %s;\n",
ret_val)`). The two earlier rules don't apply:

- The `ret_val_ct == "int64_t"` rule (`:4553`) handles a temp that is
  *already* the word.
- `carrier_return_concrete_stmt` (`:5063`) does not claim a tail whose
  value is a `match` temp of pointer C type.

So nothing bridges a pointer-typed temp into an `int64_t` return.

## Fix directions

- **Bridge in the fallthrough.** In that final branch, when
  `ret_is_int64_carrier` holds and the temp's recorded C type is a pointer
  (`emit_localvar_lookup_ctype` ends in `*`), emit
  `return (int64_t)(intptr_t)<tmp>;`. That is the same bridge the arm
  returns already use.
- **Or declare the temp as the carrier.** Declaring the match temp with the
  method's emitted return C type would remove the mismatch at its source.
- **Pin it.** Make the repro a fixture. Also add a variant with a
  `(Vec a)`-returning method, to show whether any typed-pointer class
  result takes the same path.
