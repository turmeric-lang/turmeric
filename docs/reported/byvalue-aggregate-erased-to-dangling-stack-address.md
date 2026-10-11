# A by-value aggregate erased into a retaining generic parameter becomes a dangling stack address

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
