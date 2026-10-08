# A generic call's unbound result variable takes the caller's variable of the same name

**RESOLVED 2026-10-01.** Three changes, one per place the name leaked:

1. **Elab, binding (`elab_call.c`).** A global generic call whose arguments
   bound SOME of its result variables now binds the rest from the expected
   type, on a scratch set adopted whole or not at all.  In `result-map`'s
   return position `(ok (f v))` is `(Result B E)` and `(err e)` is `(Result B
   E)`, so the match joins two agreeing arms.
2. **Emit, composition (`emit_module.c`).** The carrier-collapse rehydration
   looked a callee binding up by the CALLEE's name in the enclosing spec:
   `ok`'s `B := E` became result-map's `B := float`, so the arms were minted
   `ok__spec__Result__float__float`.  A binding whose value is a variable the
   active spec already binds is now left to the composition, which resolves
   it by the value's own name (`E := int`).
3. **Elab, naming (`elab_call.c`).** A result variable still unbound after
   both -- no argument, no expected type -- is renamed apart in the call's
   TYPE only (`ok.B`, an open slot) when it collides with the enclosing
   signature.  `(sink (ok x))` inside `[A B E]` now reports `(Result B
   ok.B)`, not `(Result B B)`.  The ABI bindings are untouched, and a
   recursive call keeps its names (they ARE the enclosing signature's).

`result-map` with a type-changing function runs in both engines, and both
spellings of a generic `Either` map (constructor arms, and arms routed
through generic constructor helpers) run, including `(ei-map inc (Lf 9))`
over an open `Left`.  Pinned by
`tests/fixtures/generic-call-result-binds-from-expected`.  Suite 3478/0.

What this does NOT do: land the generic `stdlib/either.tur`.  The blocker
[stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md) S3
recorded is gone; the rewrite itself is that audit's work.

---

**Severity: medium.** A program the checker accepts fails in cc. It happens
whenever a generic body's type variables share names with a generic callee's
unbound ones, and `A`/`B`/`E` are what everyone writes. No wrong answer has
been seen; the collision surfaces as a representation mismatch in C. Filed
2026-09-29 while making `stdlib/either.tur` generic
([stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md), S3). It
reproduces unchanged on `main` from before that work.

## Repro

`result-map` (stdlib/result.tur) with a function that changes the ok type,
over a by-value `Result`:

```turmeric
(defn half [x : int] : float (/ (as float x) 2.0))
(defn main [] : int
  (println (ok-val (result-map (:: (Ok 41) (Result int int)) half)))
  0)
```

```
In function 'result_map__spec__tur_adt_Result__float__int_tur_adt_Result__int__int_int64_t__cps':
error: incompatible types when assigning to type 'tur_adt_Result__int__int'
       from type 'tur_adt_Result__float__int'
```

The name collision itself, with no codegen involved:

```turmeric
(defn sink [x : cstr] : int 0)
(defn probe [A B E] [x : B e : E] : int (sink (ok x)))    ; got (Result B B)
(defn probe2 [A B E] [x : B e : E] : int (sink (err e)))  ; got (Result A E)
```

`(ok x)` should be `(Result B ?)`, where `?` is `ok`'s own unbound second
parameter. Instead it is `(Result B B)`.

## Root cause

`ok [A B] [x : A] : (Result A B)` binds `A` from `x` and leaves its own `B`
unbound. The call's result is the declared result with the bindings
substituted (elab_call_fn), so the unbound `B` survives by NAME. Inside
`result-map [A B E]` that name is `result-map`'s own `B`. `(err e)` likewise
leaks `err`'s `A` into `result-map`'s `A`.

In `result-map`'s body, `(match r (Ok v) (ok (f v)) (Err e) (err e))` joins
`(Result B B)` with `(Result A E)`. Each monomorph then resolves the match's
temporaries at the SCRUTINEE's instantiation, `(Result int int)`, while each
arm is the `(Result float int)` its callee's specialization returns.

## The constructor spelling fails too, and it blocks a generic `either.tur`

Writing the arms with the constructors instead, as `either-map` does, runs
into a neighbouring gap. In a generic body `(Left l)` with `l : L` fixes
nothing concrete, so it is the bare ADT, and the match's temporary is the
carrier. A by-value monomorph's arms return the aggregate, so C refuses the
assignment. A made-generic `either-map [L A B] [^fat f : (fn [A] B) e :
(Either L A)] : (Either L B)` failed this way for `(either-map inc (Left 9))`,
a SAME-type map. The `int`-typed `either-map` in the tree runs it. Routing
the arms through generic helpers (`(defn either--left [L R] [x : L] :
(Either L R) (Left x))`) failed the other way round, which is this report's
leak.

Letting the constructor's expected-type rescue (elab_call.c) accept a
signature-quantified variable types those arms as the declared `(Either L
B)`. With it, a type-changing `either-map` over a by-value `(Either int int)`
ran. The same-type shape above was not measured under it. It was reverted:
`some`'s body `(Some x)` became `(Option A)`, its constructor tail call lost
`TUR_MUSTTAIL`, 17 snapshots moved, and `cfnptr-vs-boxed-monomorph-split`
reached cc ("returning `tur_adt_Option__fn1_int__int` but
`tur_adt_Option__fnc1_int__int` was expected").

So the generic `either.tur` that
[stdlib-int-stand-in-audit](../reported/stdlib-int-stand-in-audit.md) S3 describes is
still not landed. The `fmap` blocker it recorded is resolved. What is left is
this report.

## Fix directions

- Give a generic call's unbound result variables their own identity. The
  `tyvar_.open_slot` bit that
  [fmap-over-underdetermined-constructor-is-a-defless-shell](fmap-over-underdetermined-constructor-is-a-defless-shell.md)
  added marks exactly this case for a constructor, so `(ok x)` could be
  `(Result B ?B)`. A `match` join would then need to combine complementary
  open slots, `(Result B ?)` with `(Result ? E)`, into `(Result B E)`. Today
  it joins two open arms to the bare ADT, which is right for constructor arms
  (they are the carrier) and wrong for generic-call arms (their
  specializations return the aggregate).
- Or bind a generic call's unbound result variables from the expected type,
  where there is one, as the constructor rescue does. Check first whether
  `result-map`'s declared `(Result B E)` reaches its match arms as their
  expected type; that has not been measured.
