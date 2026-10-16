# A `:heap` value moved between a word slot and a typed local without a cast

**Severity: medium** (clang rejects the emitted C with `-Wint-conversion`; gcc
warns and the program runs, so a Linux-only check passes and macOS fails).

**Status: RESOLVED 2026-10-16** (see [Resolution](#resolution)).

Found 2026-10-16 implementing turmeric-spices' `dom/writer`
(xml-html-parsers-plan follow-up B4), with a Release `tur` built from `main`
at `8c539640`. A `:heap` ADT is held two ways in emitted C: as the `int64_t`
word (a function's own parameter, a closure's parameter) and as the typed
`tur_adt_T *` (a `let`-bound local, a constructor's result). Two places move
a value from one to the other with no cast. The archived
[heap-adt-arg-to-closure-cross-module-missing-cast](../archive/heap-adt-arg-to-closure-cross-module-missing-cast.md)
fixed a third (the fat-dispatch arm of `emit_value_dispatch`).

## Shape 1: a parameter returned through a compound form

```turmeric
(defmodule a (export main)
(defdata E :heap :copy (A int) (B))
(defn touch [e : E] : E
  (do (println "x") e))          ; also (let [k 1] e), (if true e e),
                                 ; (do (match e ...) e)
(defn main [] : int
  (match (touch (A 7)) (A n) n (B) 0)))
```

```
static int64_t a__touch(int64_t e) {
        ...
        tur_adt_E * __t202;
        __t202 = e;                      /* int64_t into tur_adt_E * */
        return (int64_t)(intptr_t)__t202;
```

`CC=clang tur run a.tur`: `error: incompatible integer to pointer conversion
assigning to 'tur_adt_E *' from 'int64_t'`. A bare `e` body is fine; any form
that routes the result through a temporary is not. The temporary is declared
from the expression's Type (the typed pointer) while the parameter it is
assigned from is the word.

## Shape 2: a direct call to a known closure

```turmeric
(defmodule s1 (export main)
(defdata E :heap :copy (A int) (B))
(defn val [e : E] : int (match e (A n) n (B) 0))
(defn mk [k : int f : (fn [E] int)] : (fn [E] int)
  (fn [e : E] : int (+ k (f e))))
(defn main [] : int
  (let [h (mk 1 val)]
    (h (A 41)))))
```

The compiler knows which lambda `h` is and calls its lifted function
directly -- `s1____fn_9((void *)(intptr_t)(h), __ps_210)` -- whose parameter
is `int64_t e`, passing the `tur_adt_E *` constructor result uncast. clang:
`incompatible pointer to integer conversion passing 'tur_adt_E *' to
parameter of type 'int64_t'`. Same failure across modules (`mk` in one,
`main` in another). Calling `h` through a function parameter instead -- the
generic closure-dispatch path -- is fine.

The direct call is emitted by the let-bound closure thunk path in
`src/compiler/emit_expr.c` (the block commented "CC2
(curried-call-cast-rough-edges-plan)", which already wraps a pointer actual
for an `int64_t` formal when the actual is a closure or `ptr<void>`); a
`:heap` ADT actual is not covered.

## Workarounds in turmeric-spices

- `sax-event-retain` returns `nil` instead of its argument (shape 1).
- `tests/writer.tur` calls the result of `sax-transform-handler` through a
  helper that takes it as a parameter (shape 2). Real SAX drivers already
  call handlers that way (`sax-dispatch`).

## Fix directions

- Shape 1: when a compound form's result temporary is declared from the
  Type, bridge the assignment from a word-typed source the way
  `ptr_into_carrier` does (ask `emit_localvar_lookup_ctype` what the source
  was declared as).
- Shape 2: extend the CC2 coercion in the direct-thunk call to any actual
  whose C spelling ends in `*` going into an `int64_t` slot -- the same test
  `ptr_into_carrier` makes.
- A fixture per shape; the run.sh `-Wint-conversion` ratchet then pins both
  on gcc as well.

## Resolution

Both shapes asked "what C type is this value?" of something that did not
know the answer for a parameter.

- **Shape 1.** The `do`/`let` joins (`bridge_control_result_int_ptr`) consult
  the localvar ctype table for a bare identifier, and that table never holds
  parameters, so the parameter fell back to its Type -- the typed pointer --
  and no straddle was seen. The `if`/`match` joins (`if_arm_word_ptr_cast`,
  `emit_merge_assign_bridged`) consulted only the table. A new helper,
  `emit_ident_emitted_ctype` (`src/compiler/emit_expr.c`), answers a recorded
  local from the table and a parameter of the function being emitted from
  that function's recorded signature (`emit_sig_lookup_param_ctype`); all
  three joins use it.
- **Shape 2.** The direct call to a let-bound closure thunk keyed its bridges
  on the argument's TypeKind, none of which covers a `:heap` ADT. A final
  branch now compares the thunk's recorded slot with the argument's emitted
  spelling and casts through `intptr_t` either way, as `ptr_into_carrier`
  does for the fat-dispatch arm.

**Pinned by** `heap-adt-param-returned-through-join` (`do`, `let`, `do` over
a `match`, `if`, `match`, nested) and `heap-adt-arg-to-known-closure` (a
constructor, a let-bound value and a nullary constructor into a known
closure). The old compiler emits five and three `-Wint-conversion`s for them,
which the run.sh ratchet fails. The full suite is otherwise unchanged (no
codegen snapshot moved).
