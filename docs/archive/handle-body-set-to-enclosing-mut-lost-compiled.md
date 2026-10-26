# Compiled: a `set!` from a `handle` body to an enclosing `^mut` is lost after the `handle`

**RESOLVED 2026-10-26.** `byref_scan`'s `CT_HANDLE` case now runs `case_mut_scan` over the handled body, promoting every enclosing `^mut` it touches to a shared cell. The `reset` body had the same loss (compiled `8`, interpreter `58`) and gets the same promotion; `with-handler` was already correct. Pinned by `tests/fixtures/handle-body-set-enclosing-mut`. Checked on compiled, `tur jit` and `--interpret`.

**Severity:** high (silent wrong value, on the default engine). Compiled code
and `tur jit` both lose the write; `tur --interpret` keeps it. It needs no
effect to be performed, and nothing is reported.

Filed 2026-10-25, found while checking how `^multishot` resumes see state (the
effects guide's "What each run sees").

## Repro

```turmeric
(defeffect Ask [] : int)

(defn g [] : int
  (let [^mut c 0]
    (let [r (handle (do (set! c 5) (perform (Ask))) (Ask [] k) (resume k 1))]
      (+ (* 10 c) r))))

(defn h [] : int
  (let [^mut c 0]
    (handle (do (set! c 5) 1)       ; never performs (TUR-W0033)
      (Ask [] k) (resume k 1))
    c))

(defn main [] : int
  (println (g))   ; want 51
  (println (h))   ; want 5
  0)
```

| Engine | `(g)` | `(h)` |
| --- | --- | --- |
| `tur run` (compiled) | `1` | `0` |
| `tur jit` | `1` | `0` |
| `tur --interpret` | `51` | `5` |

The write is lost the same way under a `^multishot` clause, and when the
`set!` comes after the `perform`.

## Root cause

The CPS backend lowers the code that runs *after* the `handle` (the handle's
continuation, `CT_HANDLE`'s `body`) to its own C function, `g_hk0` in the
emitted C for `g`. The enclosing mutables it reads are copied into a malloc'd
env **when the handler is installed**, before the handled body runs:

```c
static int64_t g__cps(DK *__kont) {
    int64_t c_4;
    c_4 = INT64_C(0);
    g_hk0_env *__ce_g_hk0 = (g_hk0_env *)malloc(sizeof(g_hk0_env));
    __ce_g_hk0->f0 = c_4;                 /* snapshot: 0 */
    ...
    c_4 = INT64_C(5);                     /* the body's set! -- too late */
    return dk_perform(2, "Ask", (intptr_t)(0), __h0);
}
static intptr_t g_hk0(intptr_t env, intptr_t r_6__slot, DK *__kont) {
    int64_t c_4 = __cap->f0;              /* reads the snapshot */
```

This is the defect
[handler-clause-setbang-enclosing-mut-undeclared](../archive/handler-clause-setbang-enclosing-mut-undeclared.md)
fixed for handler *clauses* (B7b). That fix promotes every enclosing `^mut` a
clause touches to a shared heap cell (`case_mut_scan`, called from
`byref_scan`'s `CT_HANDLE` case at `src/compiler/emit_cps_ir.c:7995`). The
handled body (`t->as.handle.delim`) only goes through `byref_scan`, so a
mutable that only the body writes stays by value. When a clause also uses the
variable it is promoted and the write survives, which is why
`tests/fixtures/effect-handler-clause-setbang-enclosing-mut` ("a READ-only
clause still sees a write the handled BODY made") passes.

## Fix directions

1. In `byref_scan`'s `CT_HANDLE` case, also promote every enclosing `^mut`
   the `delim` assigns (a `set!` target bound outside the handle) to a shared
   cell, as `case_mut_scan` does for clauses. Reads alone need nothing: the
   value is current at the install.
2. Check the other continuation-splitting constructs for the same shape
   (`reset`/`shift` bodies, `with-handler`) while there.
3. Add the repro, compiled and interpreted, to
   `tests/fixtures/effect-handler-clause-setbang-enclosing-mut` or a sibling.
