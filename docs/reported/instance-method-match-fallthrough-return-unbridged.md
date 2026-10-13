# An instance method returning `(Cons a)` from a `match` emits an unbridged fall-through `return`

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
