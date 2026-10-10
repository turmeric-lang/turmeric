# `nil` passed to a `ptr<void>` parameter type-checks, then emits `((void)0)`

**Severity: low-medium (compile failure in cc, not a wrong answer).**
**Discovered:** 2026-10-10, spiking `docs/upcoming/spices/nng-async-plan.md`
against v0.63.9 (the prebuilt release `turmeric-spices/scripts/install-tur.sh`
fetches, which matches `main` at `f53e73ed`).

`tur check` accepts a literal `nil` as the argument for a `ptr<void>`
parameter. The emitter then lowers that `nil` to its unit placeholder
`((void)0)`, and cc rejects the call with `invalid use of void expression`. No
Turmeric diagnostic is printed. The failure needs no inline C: a pure Turmeric
callee shows it too.

It matters beyond a toy because **the reactor guide's own examples hit it**.
Every `reactor-add-timer` / `reactor-add-fd` / `reactor-add-chan` /
`local-spawn` example in `docs/guides/reactor-guide.md` passes `nil` as its
`user-data : ptr<void>` argument: 12 sites, counting the s-expression and
sweet-exp forms of each example. The draft nng async plan's reactor loop did
the same.

## Repro

```turmeric
(defn is-null? [p : ptr<void>] : bool (= (:: p :int) 0))
(defn main [] : int
  (println (is-null? nil))
  0)
```

```
$ tur check d.tur        # exit 0, no diagnostic
$ tur run d.tur
d_tur.c: error: invalid use of void expression
        bool __ps_173 = (is_hynull_qu(((void)0)));
```

The same call through `reactor-add-fd ... nil` fails the same way
(`reactor_hyadd_hyfd(..., ((void)0))`).

Binding the `nil` first gets a real diagnostic, which is the behaviour the
argument position should match:

```turmeric
(let [q : ptr<void> nil] ...)
;; error [TUR-E0023]: cannot bind 'q' to an expression of type :void
```

**Workaround:** pass a null pointer explicitly: `(:: 0 :ptr<void>)`. With that
spelling, the reactor fd / timer / fiber-group calls compile and run (checked in the
same spike).

## Root cause (where to look)

- `atom_nil()` (`src/compiler/emit_core.c:4447`) is the `((void)0)` placeholder
  every unit-valued expression lowers to. That is right for statement position
  and wrong as a call argument.
- The argument check does not reject a `:void`/`nil`-typed argument against a
  `ptr<void>` parameter, although the `let` path does (`TUR-E0023`). So the
  value reaches the call emitter, which splices the placeholder in as-is.

## Fix directions

1. **Reject it in the checker** (smallest): a `nil`-typed argument to a
   non-unit parameter is a type error. Name `(:: 0 :ptr<void>)` in the
   message, as `TUR-E0023` names `do`.
2. **Or give `nil` a meaning at pointer type**: lower a literal `nil` to
   `NULL` when the expected type is `ptr<T>`. That makes the guide examples
   correct as written, but it adds a second meaning to `nil`.
3. Either way, change the 12 reactor-guide sites to whatever the fix
   blesses, and add a fixture that passes `nil` (or the blessed spelling) as
   `reactor-add-fd`'s user-data, so the guide's shape is under test.
