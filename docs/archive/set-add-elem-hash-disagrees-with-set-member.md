# `set-add-elem__` adds elements that `set-member?` cannot find

**RESOLVED 2026-09-16** -- see Resolution at the end. It was TWO compiler defects, neither in `stdlib/set.tur`.

**Severity: medium.** A silent wrong answer at the API level: the element IS in
the set (`set-count` counts it) but every membership test says `false`. Nothing
crashes, so a caller mixing the two spellings gets a set that quietly behaves
as if empty.

**Status:** open. Found 2026-09-11 building `spices/crdt`'s G-Set on
crdt-spice-plan C2.

## Repro

```turmeric
(defn main [] : int
  (let [a  (set-add-elem__ (set-new) (quote x))
        a2 (set-add-elem__ a (quote y))]
    (println (set-count a2))                                ;; 2  -- both added
    (println (set-member? a2 (hash (quote x)) (quote x))))  ;; false -- WRONG
  0)
```

The same set built through the `set-add` macro is fine:

```turmeric
(let [s (set-add (set-new) (hash (quote x)) (quote x))]
  (println (set-count s))                              ;; 1
  (println (set-member? s (hash (quote x)) (quote x))))  ;; true
```

## Mechanism

`set-add` and `set-member?` (`stdlib/set.tur:127`, `:173`) are macros taking an
explicit hash and forwarding it to `set-add-eq-o` / `set-has-eq-o?` alongside
`mk-box` / `mk-cmp` / `mk-owned?`. Given the same `h` on both sides they agree,
which is what the second snippet shows.

`set-add-elem__` (`:524`) is the typed `[^Hash A ^MapKey A]` entry point and
computes the hash itself. Whatever it computes is not what `(hash x)` returns
for the same element, so a set built with it cannot be queried with the macro.

Which one is wrong is the open question. If `set-add-elem__`'s internal hash is
the intended one, then `set-member?` needs a typed twin that computes the hash
the same way -- there is `set-add1` (`:545`, sugar for `set-add-elem__`) but no
`set-member1`, which is probably how the gap survived.

## Why it bit here

A G-Set's entire content is "add elements, union two sets, ask whether an
element is present". The typed adder is the one a reader reaches for first --
it needs no hash argument and has the `Hash`/`MapKey` constraints that suggest
it is the principled entry point.

`spices/crdt` uses the explicit-hash `set-add` / `set-member?` pair throughout
as a result, with a comment pointing here.

## Fixture owed

The repro above, asserting `true`. Note that `set-count` alone passes either
way -- the assertion has to be a membership test, which is why a fixture that
only checked sizes would not have caught this.

## Resolution (2026-09-16)

Neither hash was "wrong" -- `set-add-elem__` never ran at `A = Sym` at all.
Two emitter defects, both general and neither about sets:

1. **The call stayed on the carrier base, whose `(.hash x)` is baked to the
   representative `Hash[int]`.** `(set-add-elem__ (set-new) x)` records
   `A -> <tyvar>` at elab: the receiver `(set-new)` is `(Set A')` with nothing
   to ground it, the binding walk takes the first mention, and the concrete
   `x` two slots later never reaches the binding. `emit_abi_register_call`'s
   site-correction pass deliberately skipped bare-tyvar parameters ("`x : A`
   is whatever the caller says it is"), so nothing repaired it and the
   generic was called through its int64 base -- where the Sym was hashed by
   its ADDRESS. A `set-add1` macro expansion hit this on BOTH calls, since the
   S4 let-local forward inference is skipped under a macro. `set-count` was
   right because the element WAS inserted, just under the wrong hash. int
   elements passed by luck (`Hash[int]` IS the representative). The pass now
   lets a bare-tyvar argument pin a binding, but ONLY one elab left abstract
   -- a concrete recorded binding is what the caller said and stays.

2. **Once the second call minted a clone, its forward declaration disagreed
   with its definition.** `call->type` for `(set-add-elem__ a y)` was still
   spelled `(Set A)` in the callee's letters; the clone name and prototype
   c-named that as `int64_t` while the definition emitter resolved it under
   the spec to `tur_adt_Set__sym *`: "conflicting types for
   `set_add_elem____spec__int64_t_...`", a cc error. Nothing typeclass about
   it: `(defn idset [A] [s : (Set A) x : A] : (Set A) s)` called twice fails
   identically. The result is now grounded through the call's own bindings
   when -- and only when -- every binding is itself ground; a half-abstract
   spec (`(result-map (ok 5) f)`, whose err var is never pinned) stays on the
   carrier, since grounding its result while its receiver stayed abstract
   tripped the arg-bridge repr shadow in `typed/result-basic`.

Both spellings now agree with `set-member?` for Sym, cstr and int; the
direct `set-add-elem__` call, the `set-add1` sugar, and the chained shape all
build. Pinned by `tests/fixtures/set-add-elem-typed-member` (membership
assertions, as the report insisted) and
`tests/fixtures/generic-heap-result-spec-fwd-decl` (the plain-generic
forward-declaration shape, Set and Vec). One snapshot moved
(`option-consumers-byvalue-arg`): a dead carrier base is no longer emitted.

Not touched: a receiver that stays unpinned through a USER generic
(`(addv (vec-new) "x")`) still leaves the let-bound result's ELEMENT type
unresolved at elab, so a bare `(vec-get v2 1)` reads the carrier word; the
fixture ascribes that read and notes it. That is an inference limit, not this
report.
