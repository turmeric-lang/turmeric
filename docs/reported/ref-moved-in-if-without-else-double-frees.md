# A `ref<T>` moved inside an `if` with no else is freed twice

**Severity:** high (a double free, and a use after free, in programs the
compiler accepts).

Moving a `ref<T>` on one side of an `if ... else` is TUR-E0104. The same move
inside an `if` with no else, or a `when`, is accepted: after the `if` the ref
reads as never moved, so it is dropped again at scope exit, and it can be read
after the branch freed it.

Found 2026-11-27 checking which other scope-exit drops had the
one-decision-per-`let` shape #1486 fixed for Drop opaques. This one predates
it: the pre-#1486 `main` (`d4ad1b0`) behaves the same. Verified on `main` at
`853f866`.

## Repro

`r/main.tur`:

```turmeric
(defn f [c : bool] : int
  (let [r (ref 41)]
    (if c (let [tmp r] 0))
    1))

(defn main [] : int
  (println (f true))
  (println (f false))
  0)
```

```sh
tur check r/main.tur                       # no diagnostic
TUR_SANITIZE_PROGRAMS=1 tur run r/main.tur
```

```
ERROR: AddressSanitizer: attempting double-free on 0x502000000010 in thread T0:
SUMMARY: AddressSanitizer: double-free ... in free
```

Reading the ref afterwards is a use after free:

```turmeric
(defn f [c : bool] : int
  (let [r (ref 41)]
    (if c (let [tmp r] 0))
    @r))
```

```
ERROR: AddressSanitizer: heap-use-after-free ... READ of size 8
```

| Shape | Result |
|---|---|
| `(if c (let [tmp r] 0))` | double free |
| `(when c (let [tmp r] 0))` | double free |
| `(if c (sink r))`, `sink` taking `ref<int>` by value and dropping it | double free |
| `(if c (let [tmp r] 0) 0)` | TUR-E0104, as it should be |
| the same shapes with `rc<T>` (`rc/of`, `rc/drop`, a by-value callee) | clean under ASan and LeakSanitizer |
| a `^linear` opaque consumed in an `if` with no else | TUR-E0100 at the binding |

`tests/fixtures/ref-if-branch-move-suppression` pins the accepted form with a
condition that is never true -- `(if false (let [tmp r] 0))` followed by
`@r` -- so it passes without ever taking the branch.

## Root cause

`elab_if`, the no-else path (`src/compiler/elab_forms.c:4855`):

```c
} else {
    /* Without else, then-branch moves are not guaranteed after the if. */
    move_state_restore(move_bindings, before_states, n_move_bindings);
    ...
    if (n_lin > 0) {
        linear_state_restore(lin_bindings, lin_before, n_lin);
    }
}
```

Restoring is right for "is it certainly moved", and wrong for "who releases
it": the then-branch took ownership, and the scope exit still drops. #1486
closed this for a local with `scope_drop_inst` by giving the missing else a
branch that releases it (`:4860`). A `ref<T>` takes the other drop path
(`:2713`, the `TY_REF` count in `elab_let`), has no `scope_drop_inst`, and is
skipped. The TUR-E0104 check that catches the two-armed form is inside the
`len == 4` block (`:4640`), so the no-else form never reaches it. A `^linear`
opaque is caught later, by the unconsumed check at the binding; a ref is
exempt from that one because it auto-drops.

## Fix directions

- Reject it, as the two-armed form is: a linear binding the then-branch
  consumed, in an `if` with no else, is TUR-E0104. That matches what the
  user is told one keystroke away, and needs no codegen. The fixture above
  then has to change -- its program is this bug with a dead branch.
- Or accept it and release on the missing path, as #1486 does for Drop
  opaques: synthesize the else with a `drop!` of the ref and mark it moved.
  `elab_branch_with_scope_drop` builds its glue from `scope_drop_inst`, so
  it needs a ref variant.
- Either way, add the three double-free shapes to a fixture under
  `tests/run-leak-check.sh` with a condition that is true.
