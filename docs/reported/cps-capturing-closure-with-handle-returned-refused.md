# A returned capturing closure that handles its own effect is refused

**Severity: low** (a refusal, not a miscompile: the build fails with a
located error, and `tur --interpret` runs the program).  Found 2026-10-08
while resolving
[cps-effectful-closure-returned-through-empty-row-aborts](../archive/cps-effectful-closure-returned-through-empty-row-aborts.md);
it reproduces identically on the compiler before that change.

## Repro

```turmeric
(defeffect Ask [] :int)
(defn ask [] : int (perform (Ask)))
(defn app1 [h : (fn [int] int) x : int] : int (h x))
(defn adder [m : int] : (fn [int] int)
  (fn [x : int] : int
    (handle (+ m x (ask))
      (Ask [] k) (resume k 10))))
(defn main [] : int
  (println (app1 (adder 3) 1))
  0)
```

```
$ tur run repro.tur
repro.tur:2:20: error: this effect operation has no lowering here: ...
$ tur --interpret repro.tur
14
```

Nothing escapes the closure -- it handles `Ask` itself -- so the call through
`h` needs no handler from the caller.  The same closure works passed straight
to `app1` (capturing or not), and returned when it captures nothing
(`cps-effectful-closure-returned-handles-its-own-effect`).

## Root cause

`TUR_TRACE_EVICT=1` shows the lambda as `SIG-REJECT eff=1`: a returned
capturing closure's signature is not one the CPS backend admits
(`fn_sig_ok`), so it is not a candidate, and a non-candidate's effect set is
everything it performs OR HANDLES (`emit_cps_ir.c`, ensure_S).  Its `handle`
of `Ask` therefore taints `Ask`, `ask` leaves the CPS backend with it, and the
direct emitter cannot lower `ask`'s `perform`.  The B5 rule that counts only
net escaping effects applies to candidates only -- correctly, since a handle
in a function the direct emitter owns has no lowering either.

## Fix directions

1. Admit the returned capturing closure's signature to the CPS backend (its
   env-taking `__cps` twin already exists for closures passed as arguments),
   so it is a candidate and B5 applies.
2. Failing that, a better diagnostic: name the closure whose `handle` took
   the effect off the backend, as the escaping-closure note does.
