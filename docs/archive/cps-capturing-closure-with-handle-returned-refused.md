# A returned capturing closure that handles its own effect is refused

**RESOLVED 2026-10-09.** Both shapes compile; see "Fixed: shape 1" at the
end. The second shape was never about closures: its handling function had a
parameter named `k`.

**Severity: low** (a refusal, not a miscompile: the build fails with a
located error, and `tur --interpret` runs the program).  Found 2026-10-08
while resolving
[cps-effectful-closure-returned-through-empty-row-aborts](cps-effectful-closure-returned-through-empty-row-aborts.md);
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

## A second shape in the same family (found 2026-10-08)

A function whose `handle` body builds a capturing closure into a local and
hands it to an effectful callee is refused the same way. Nothing is returned
here, so this is not the shape above, but `TUR_TRACE_EVICT=1` shows the same
SIG-REJECT on the handling function, which then taints the callee:

```turmeric
(defeffect Tick [] : int)
(defn eff-call [o : (Option (fn [int] int))] : int
  (let [t (perform (Tick))]
    (match o (Some f) (+ t (f 1)) (None) t)))
(defn effect-callee [k : int] : int
  (handle (let [ff (some (fn [x : int] : int (+ x k)))]
            (eff-call ff))
    (Tick [] k2) (resume k2 1)))
(defn main [] : int (println (effect-callee 40)) 0)
```

```
[EVICT] SIG-TAINT              eff=1 eff-call
[EVICT] SIG-REJECT             eff=1 effect-callee
ev3.tur:3:11: error: this effect operation has no lowering here: ...
$ tur --interpret ev3.tur
42
```

The same callee called with the closure built OUTSIDE the handling function
(`(handle (eff-call (some (fn ...))) ...)` in `main`) compiles and prints
42. Found while writing `sum-closure-payload-kept`, which leaves this shape
out for that reason; it predates the closure-payload drop work.

### Fixed 2026-10-09: it was the parameter name

`effect-callee`'s parameter is `k`, and `param_name_clashes_cps` keeps every
function with a parameter named `k` off the CPS backend (a defensive rule,
load-bearing only for some Saffron self-applying functions and sum-closure
drops the CPS path does not yet reproduce -- lifting it outright still leaks
`saffron-lambda-arg-env-freed` and `sum-closure-payload-dropped`, measured
today). Renamed to `m`, the program compiled and printed 42. A function that
installs a `handle` (`cps_fn_installs_handle`, `src/passes/cps.c`; an
`(unsafe ...)` marker does not count) is now exempt: evicted, it took every
effect it handles off the backend, so the refusal was certain. Pinned by
`tests/fixtures/handle-fn-with-k-param`, which also has the plain shape --
`(defn run-with [k : int] ...)` handling a named callee's `perform` -- refused
the same way before.

### Fixed: shape 1 (2026-10-09)

Two changes, both needed:

- **Admission.** A capturing lambda went to the CPS backend only when
  threadable, i.e. passed to a threading parameter. One that installs a
  `handle` (`cps_fn_installs_handle`) is admitted too (`fn_sig_ok`,
  `src/compiler/emit_cps_ir.c`). Evicted, its `handle` took the effect off
  the backend, because a non-candidate's effect set counts what it handles.
- **The unnamed permanent source.** With the lambda admitted, it and `ask`
  were `SIG-TAINT` from the BASE taint: `adder` performs nothing itself, so
  it is uncolored, and the base-taint seed of an uncolored function
  (`expr_collect_effects`) descended into the closure literal's body. The
  lambda's `handle` of `Ask` therefore counted as fiber code. The lambda is
  lifted to its own top-level FnDef and classified as its own entry, so the
  seed now leaves its body alone (`expr_collect_effects_base`,
  `EffAcc.skip_lifted_bodies`).

A returned closure whose effect ESCAPES (no `handle` inside) is still
refused: it is an unthreaded fn value, a permanent fiber source of its own.

Pinned by `tests/fixtures/cps-returned-capturing-closure-handles-own-effect`.
It covers the repro, two instances of a closure whose resumed value depends
on its capture (let-bound, called through a parameter and directly), and the
same effect performed and handled elsewhere. It equals `tur --interpret`, is
leak-checked, and passes the JIT harness and fnsan. The fixture suite, the
leak harness and the fuzzer (seed 3333: 282 ok, no bug) are unchanged.
