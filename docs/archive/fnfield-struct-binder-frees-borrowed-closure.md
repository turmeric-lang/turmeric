# A let binder frees the closure of a struct it does not own

> **RESOLVED 2026-11-28.** `drops_fn_fields` now asks the binding's init, not
> only its body (`fnfield_value_owned`, `src/compiler/elab_forms.c`). A local
> frees its closures only when it owns them: built here from fresh closures (a
> lambda, a named defn, a call's closure result), moved from a binding that
> owned them, a by-value move-only parameter, or a call -- for a `^copy` struct,
> only a call to a defn whose results are stamped owned (`elab_stamp_fnfield_ret`).
> A `^copy` of a local, a closure parameter, a field of another struct and a
> global `def` are borrowed. A let-bound closure stored in a struct is owned
> only if that is its one use: its own let un-flags the struct
> (`fnfield_release_shared_takers`) when it is used again, captured, or stored
> inside a `while`. That un-flagging also closed a fourth shape the report did
> not list -- a let-bound closure stored in a struct and then called after the
> struct's scope. The field-call receiver hoist (`elab_typeclasses.c`) asks the
> same question.
>
> All four shapes now run clean of use-after-free and double free. Shapes that
> share a closure with no other owner leak it instead -- the safe direction,
> filed as [let-bound-closure-shared-with-fnfield-struct-leaks](../reported/let-bound-closure-shared-with-fnfield-struct-leaks.md).
> Pinned by the leak-checked `fnfield-struct-binder-borrowed-closure` (clean)
> and `fnfield-struct-binder-shared-closure` (`known-leak`).

**Severity: high (double free / use-after-free on the default build).** A
`let` local whose type is a by-value struct with a closure field frees that
closure at scope end (local-struct-drop). The rule assumes the local owns the
closure. Three ordinary shapes break that assumption, and each frees a closure
that its real owner frees again or still uses.

Found 2026-11-24 while executing
[cps-byvalue-struct-with-closure-field](cps-byvalue-struct-with-closure-field.md).
All three reproduce on `main` at `1227412b`, with no effects involved: the
first two in direct-emitted code, the third in a function the CPS backend
emits anyway (`mk__cps`), where the entry boundary's reap does the freeing.
The plain build usually prints the right answer; build as
`tests/run-leak-check.sh` does (ASan, `TUR_RUNTIME=source`) to see them.

## Repros

**1. A rebinding of a `^copy` struct.** Both `h` and `g` are flagged.

```turmeric
(defstruct ^copy H [raw : int handler : (fn [int] int)])
(defn main [] : int
  (let [n 3
        h (H 100 (fn [x : int] : int (* x n)))]
    (let [g h] (println ((.handler g) 2)))
    (println ((.handler h) 2)))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in main
```

**2. A function that returns the `^copy` struct it was given.** `r` is flagged
as if `pick` had built a fresh one.

```turmeric
(defstruct ^copy H [raw : int handler : (fn [int] int)])
(defn pick [h : H n : int] : H
  h)
(defn main [] : int
  (let [h (H 100 (fn [x : int] : int (* x 10)))]
    (let [r (pick h 3)]
      (println ((.handler r) 4))))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in tur_closure_drop
    #1 drop_fnfields_tur_adt_H
    #2 main
```

Without `^copy` this one is fine: passing `h` moves it, so only `r` is
flagged.

**3. A struct built from a closure parameter.** `h` is flagged, and frees the
caller's closure. This one does not need `^copy`.

```turmeric
(defdata Ev (Start int) (End))
(defstruct H [raw : int handler : (fn [Ev] int)])
(defn mk [f : (fn [Ev] int)] : int
  (let [h (H 1 f)]
    ((.handler h) (End))))
(defn main [] : int
  (let [n 3
        g (fn [e : Ev] : int n)]
    (println (mk g))
    (println (mk g)))
  0)
```

```
ERROR: AddressSanitizer: heap-use-after-free ... in mk__cps
```

## Root cause

`Binding.drops_fn_fields` is set per `let` binding from the binding's TYPE and
a few guards on the BODY (`src/compiler/elab_forms.c`, the "local-struct-drop
(fn-field)" loop: `is_moved`, `is_binding_consumed`, `is_field_consumed`, and
since 2026-11-27 `fnfield_local_leaves_scope`). Nothing looks at the INIT, so
the rule cannot tell a freshly built struct from a copy of one, or a closure
the constructor was handed from one it made.

`elab_structs.c` states the precondition the free relies on: a boxed fn field
makes the struct move-only, "the precondition that keeps a single owner and
avoids a copy double-freeing the shared handle". Two things void it:

- `^copy` on the struct (repros 1 and 2). The recursive-spine free beside this
  one excludes `:copy` for exactly this reason (`!def->is_copy`, "`:copy` IS
  THE SOUNDNESS LINE"); the fn-field free does not.
- A constructor argument that is borrowed (repro 3). A fn parameter is the
  caller's, and storing it in a field does not transfer it.

The CPS path mirrors the direct one (`emit_fnfield_reap`,
`src/compiler/emit_cps_ir.c`). Its `__dk_reap_closure_once` hides shapes 1 and
2 when both binders are in one CPS entry, and nothing more.

## Fix directions

1. **Ask the init.** Flag only a binding whose init is fresh: a constructor
   call whose fn-field arguments are closure literals or top-level function
   names, or a call to a function proven to return a fresh one. The RM1 sum
   work already has that shape of analysis (`returns_fresh_sum_box`,
   `call_returns_fresh_sum_box`, `src/compiler/elab_fns.c`). Everything else
   keeps the leak.
2. **Apply the `:copy` line.** A `^copy` struct with a closure field is not
   freed at all unless the init is a constructor written in place. This is the
   smaller change, and it turns repros 1 and 2 into leaks. The SAX parsers in
   turmeric-spices are `^copy`.
3. Either way, repro 3 needs the constructor to know whether it took
   ownership of each fn argument.

## Workaround

Do not `let`-bind a copy of a closure-field struct or the result of a function
that hands one back; use the original binding. Build such a struct from a
closure written at the constructor call.
