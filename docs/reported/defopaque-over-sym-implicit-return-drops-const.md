# A `defopaque` over `:Sym` returned without an ascription drops `const` in C

**Severity:** low -- a C warning (gcc `-Wdiscarded-qualifiers`, clang
`-Wincompatible-pointer-types-discards-qualifiers`), not an error, and the
program runs correctly. It would become a build failure under `-Werror`, and
it is noise in every program that does it.

Found 2026-10-25 moving turmeric-spices' `crdt` `ReplicaId` from a `defalias`
to `(defopaque ReplicaId :Sym)` (crdt-spice-plan follow-ups); reproduced with
the v0.71.0 release.

## Repro

```turmeric
(defopaque Rid :Sym)
(defn rid [s : Sym] : Rid s)
(defn rid-sym [r : Rid] : Sym (:: r Sym))
(defn main [] : int
  (do (println (sym->str (rid-sym (rid (quote alice)))))
      0))
```

`tur run` prints `alice`. `tur emit-c` then `cc -c` warns on `rid`:

```c
static void * rid(const struct __tur_sym * s) {
        return s;      /* returning 'const struct __tur_sym *' ... discards qualifiers */
}
```

## Root cause

An opaque over `:Sym` lowers to `void *` (since
[defopaque-over-sym-skips-the-ptr-bridge](../archive/defopaque-over-sym-skips-the-ptr-bridge.md)),
while `Sym` itself is `const struct __tur_sym *`. The implicit `Sym -> Rid`
conversion at a return emits no cast. The explicit ascription does:
`(defn rid [s : Sym] : Rid (:: s Rid))` emits `return (void *)(s);` and
compiles clean, which is the workaround the crdt spice uses.

## Fix directions

- Emit the same `(void *)` cast for the implicit conversion that the
  ascription path emits -- at a return, and presumably at an argument or a
  `let` binding of the opaque type too (only the return was observed).
- Or lower an opaque over a const-pointer representation to `const void *`,
  so the qualifier survives; the `(:: r Sym)` direction would then need no
  cast either.
