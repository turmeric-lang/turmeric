# A by-value aggregate erased into a retaining generic parameter becomes a dangling stack address

**Status: RESOLVED 2026-10-11** (fix direction 1, see
[Resolution](#resolution)).

**Severity: high (silent wrong values).** Filed 2026-10-11 while executing
[parsec-guide-plan](../archive/parsec-guide-plan.md).

When a by-value aggregate -- a `(Pair A B)`, a plain `defstruct`, a concrete
`(Option T)` -- crosses into an erased `int64` carrier slot, the emitter spills
it to a stack temporary and passes the temporary's **address**. That is sound
only when the callee reads the value before the caller's frame goes away. A
callee that **retains** the word -- captures it in a closure, stores it --
keeps a pointer into a dead frame.

## Repro

```turmeric
(load "stdlib/parsec.tur")

(defn two [] : (Parser (Pair int int))
  (do-m _c (item) (pure (pair 1 2))))

(defn main [] : int
  (let [p (parse-first (two) "x")]
    (println (pair-fst p))      ; garbage, e.g. 94148180744024 -- expected 1
    (println (pair-snd p)))     ; garbage -- expected 2
  0)
```

`Applicative [Parser]`'s `pure` takes `[x]` erased and captures it in the
parser closure, which runs after `two`'s continuation has returned. The emitted
continuation:

```c
tur_adt_Pair__int__int __t172 = __ps_171;
return __inst_Applicative_pure_Parser((int64_t)(intptr_t)(&__t172));
```

The same happens for an `(Option T)` built with `some` and handed to `pure`
(`(do-m c (item) (pure (some c)))` reads back `#\null`), and for a plain
by-value `defstruct`.

## Root cause

`emit_carrier_bridge` (src/compiler/emit_core.c), the CK_CONCRETE ->
CK_CARRIER arm for a non-inline aggregate:

```c
/* Aggregate: spill to a local, return its address as int64_t. */
buf_printf(body, "%s %s = %s;\n", cname, tmp, src_str);
buf_printf(&out, "(int64_t)(intptr_t)(&%s)", tmp);
```

The comment's guarantee -- "stays live through the expression that consumes
the carrier value" -- holds for a synchronous reader, not for a retainer.

## Fix directions

1. Heap-box (`malloc` + copy) at the bridge when the consumer may retain its
   argument -- every erased typeclass-method parameter and every erased
   generic parameter whose callee captures or stores it. Needs an owner for
   the box (see `carrier-sum-option-boxes-have-no-owner`).
2. Monomorphize the retaining instance per concrete element type so the value
   never crosses into the carrier (`Applicative [Parser]` at `(Pair int int)`
   would capture the struct by value).
3. At minimum, diagnose a by-value aggregate erased into a parameter the
   callee captures.

## Workaround

Build one-word results: a `:heap` struct, a `String`, a `(List A)`, a `Char`,
an `int`. `stdlib/parsec.tur`'s own `optional` builds its `Option` boxes on
the heap (`tur_some_int`), so `(Parser (Option A))` is exact. The parsec guide
documents the limit under "What a parser can produce".

## Resolution

Fix direction 1, scoped by parametricity rather than by guessing. Both
call-argument arms that bridge a by-value aggregate into an erased carrier
parameter (`emit_value_dispatch`: the `TY_APP` block for `(Pair int int)` /
`(Option Char)`, and the non-parametric block for a plain `defstruct`) already
heap-promote for an inline-C container insert. They now also take the escaping
bridge (`emit_carrier_bridge_escaping`, a `malloc`'d copy, region-noted) when
the callee is a typeclass instance method that may RETAIN the parameter.

That is decided from the CLASS method's declared signature
(`inst_method_may_retain_param`, `src/compiler/emit_expr.c`). A method can hand
an argument back -- in a closure, a container, or as itself -- only through a
result whose type mentions one of the parameter's value-kinded type variables.
`pure : a -> (f a)` qualifies. `fmap`, `bind` and `ap` (results `(f b)`),
`show : a -> cstr` and the encoders do not, and keep the stack spill. A
constructor variable (`f` in `(f a)`) and a variable under a function type the
parameter holds (`ap`'s `(f (fn [a] b))` holds functions, not `b`s) are not
values the parameter contains.

Measured on the fixture corpus: a first version that heap-promoted for every
instance method fired at 20 fixtures (`bind`/`fmap`/`ap`/`catch-error`
receivers that only read their argument, so a leaked box per call); the
parametricity test fires at none of them, and at the repro.

**Cost:** the box has no owner yet, so each retaining crossing allocates once
and is never freed (the
[carrier-sum-option-boxes-have-no-owner](../reported/carrier-sum-option-boxes-have-no-owner.md)
problem). A leak where there was a dangling read.

**Not covered:** a method that stores the value through a side effect into
another argument (a mutable container) without returning it; the signature
does not show that.

**Pinned by** `parsec-pure-byvalue-aggregate` (`(pure (pair 1 2))`, `(pure
(some c))` and a plain `defstruct` through a parser; the old compiler printed
garbage for all three). The parsec guide, tutorial and `stdlib/parsec.tur`'s
header no longer describe the limit.
