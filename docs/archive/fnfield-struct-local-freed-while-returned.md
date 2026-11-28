# A let-bound struct with a closure field is freed at scope end even when the `let` returns it

**Status: RESOLVED 2026-11-27** -- see [Resolution](#resolution).

**Severity: high (use-after-free on the default build).** A by-value struct
local with a closure field has its closures freed when its `let` ends
(local-struct-drop). When the `let`'s value is that struct, the caller
receives a struct whose closure is already freed, and calls it. This is every
constructor function written with a `let`.

Found 2026-11-24 while executing
[cps-byvalue-struct-with-closure-field](cps-byvalue-struct-with-closure-field.md).
It reproduces on `main` at `1227412b`, on the direct path, with no effects
involved.

## Repro

```turmeric
(defstruct H [raw : int handler : (fn [int] int)])
(defn mk [n : int] : H
  (let [h (H n (fn [x : int] : int (+ x n)))]
    h))
(defn main [] : int
  (let [m (mk 7)]
    (println ((.handler m) (.raw m))))
  0)
```

The plain build prints `14`: it reads the freed env before anything reuses it,
which is why no fixture caught this. Built the way `tests/run-leak-check.sh`
builds (ASan, `TUR_RUNTIME=source`):

```
ERROR: AddressSanitizer: heap-use-after-free ... in main
```

The same happens with `(if (> n 0) h h)` as the body, and with the struct
declared `^copy`.

## Root cause

The emitted `mk` ends its scope with `drop_fnfields_tur_adt_H((void *)&h)` and
then returns `h`.

The elaborator sets `Binding.drops_fn_fields` for a by-value local that owns a
boxed fn field (`src/compiler/elab_forms.c`, the "local-struct-drop
(fn-field)" loop in the `let` elaboration). Its comment says a struct that
escapes is never flagged, but the guards it shares with the rc/ref auto-drop
do not see a return:

- `Binding.is_moved` is not set by using the local as the scope's value;
- `is_binding_consumed` looks for an explicit `drop!`.

## Resolution

**Fixed 2026-11-27.** The flagging loop skips a local that the scope hands out
(`fnfield_local_leaves_scope`, `src/compiler/elab_forms.c`). The walk follows
tail positions (`do`, `let`, `if`, `match` arms) and any `return`, and answers
yes for:

- the local itself, or a field of it whose type can hold a closure;
- a call or constructor that takes either as a direct argument and whose
  result is not a plain scalar: `(Ok h)`, `(pick h g)`;
- an alias: a `let` that rebinds the local and hands the alias out.

A local that is only read and called, or lent to a function returning a
scalar, is still freed at scope end. The walk errs toward yes: a wrong yes is
a leak, a wrong no is the use-after-free.

The receiving binder owns the struct from there, as before: `m` in the repro
is flagged and frees the closure once.

Pinned by `tests/fixtures/fnfield-struct-local-returned` (leak-checked: plain
return, through `if`, a `^copy` struct, an early `return`, and a local that
stays put). On `main` that fixture is a heap-use-after-free under ASan.

## Related

The binder of a struct someone else owns was a separate problem, filed as
[fnfield-struct-binder-frees-borrowed-closure](fnfield-struct-binder-frees-borrowed-closure.md)
and fixed 2026-11-28.
