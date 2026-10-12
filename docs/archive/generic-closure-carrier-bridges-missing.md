# Generic closures skip two carrier bridges: a let-bound call's result, and a typed capture into a generic callee

**RESOLVED 2026-10-12** -- both bridges are in; see "Fixed" at the end.

**Severity: medium (C compile failure on current toolchains).** Two shapes of
a closure returned by a generic function emit C that passes an integer where a
pointer is expected, or the reverse. gcc 13 only warns (`-Wint-conversion`)
and the program prints the right values; gcc 14 and clang 15+ make
`-Wint-conversion` an error by default, so the same program does not build
there (Apple clang on macOS included). The interpreter is unaffected.

Found writing turmeric-spices' signal guide: a `cycle` -- a closure that hands
out the next value of a list on each call -- written generically,
`(defn cycle [A] [values : (Vec A)] : (fn [] A) ...)`. The guide ships a
`(Vec float)` version instead.

All repros below build with `CC="gcc -Werror=int-conversion"` to stand in for
gcc 14 / clang 15+.

## Defect 1: a let-bound generic closure's result is not bridged

```turmeric
(defn capture [A] [v : A] : (fn [] A)
  (fn [] : A v))

(defn main [] : int
  (println ((capture "hi")))          ;; compiles
  (let [g (capture "bound")]
    (println (g)))                    ;; error: passing argument 1 of 'puts'
                                      ;; makes pointer from integer
  0)
```

Applying the result at once is fine -- that is the shape
`tests/fixtures/generic-closure-capture-register-class` pins. Binding it first
is not:

```c
void * __ps_208 = (capture__spec__void___const_char__("bound"));
int64_t g_12 = (int64_t)(intptr_t)(__ps_208);
int64_t __ps_209 = (__fn_5((void *)(intptr_t)(g_12)));
puts(__ps_209);
```

The call through `g` takes the direct-dispatch path for a binding with a known
`closure_fn_binding` (`src/compiler/emit_expr.c`, the
`if (fn_binding->closure_fn_binding)` arm of call emission, ~line 11077).
Because `cstr` does not change the register class, no per-spec clone of the
inner closure body exists (`emit_inner_closure_needs_float_spec` is for
float and by-value aggregate tyvars), so the call goes to the shared carrier
thunk `__fn_5`, which returns the `int64_t` carrier. Nothing converts that
back to the call's static type (`cstr`) before it reaches `puts`.

## Defect 2: a typed capture handed to a generic callee is not bridged

```turmeric
(defn first-of [A] [values : (Vec A)] : (fn [] A)
  (fn [] : A (vec-get values 0)))

(defn main [] : int
  (let [f (first-of [1.5 2.5])]
    (println (f)))                    ;; error: passing argument 1 of
                                      ;; 'vec_hyget' makes integer from pointer
  0)
```

Here a `float` binding does get the per-spec clone of the closure body, and its
env struct types the capture concretely:

```c
struct __env_7__spec__double { tur_thunk_double_t __fn; tur_adt_Vec__float * values; };
static double __fn_5__spec__double_void__(void * __env_p_8) {
    ...
    int64_t __tur_sr = vec_hyget(__env___env_7__spec__double->values, INT64_C(0));
    ...
}
```

But the body still calls the generic, carrier-typed `vec_hyget(int64_t,
int64_t)`, and the now-typed `tur_adt_Vec__float *` capture goes in with no
`(int64_t)(intptr_t)` cast. The result side is handled (the union
reinterpret back to `double`); the argument side is not.

The generic `cycle` that found this hits both: Defect 2 for its float
instance, Defect 1 for a `cstr` one.

## Fix directions

1. **Defect 1:** in the direct-dispatch arm, when the call goes to the shared
   (carrier) thunk and the call's type is a pointer or `float` at this
   instantiation, bridge the result from the carrier the way the immediately
   applied shape already does (`emit_carrier_bridge`, CK_CARRIER ->
   CK_CONCRETE).
2. **Defect 2:** a capture read in a per-spec clone body is a typed value;
   either record its C type where the call-argument bridge looks
   (`emit_env_struct_set_cap_ctypes` already records the field types, and the
   `gcc14-int-conversion` argument casts key on `emit_localvar_lookup_ctype`),
   so passing it to a carrier parameter casts it through `intptr_t`, or
   re-resolve `vec-get` to its by-value spec inside the clone, as the outer
   spec does for its own calls.
3. A fixture beside `generic-closure-capture-register-class` covering the
   let-bound call (int, float, cstr, bool, a struct) and a capture passed to a
   generic stdlib callee, built with `-Werror=int-conversion`, would pin both.

## Workaround

Write the closure-returning function at a concrete type, as the signal guide's
`(defn cycle [values : (Vec float)] : (fn [] float) ...)` does.

## Fixed (2026-10-12)

- **Defect 1** (`src/compiler/emit_expr.c`, the `closure_fn_binding`
  direct-dispatch arm of call emission). Beside the SR2a readback that
  unboxes a by-value sum, a call whose thunk is EMITTED returning the int64
  carrier (`emit_sig_lookup_ret_ctype`) while the call's resolved C type is
  a pointer is now cast back, `((const char *)(intptr_t)(__fn_5(...)))`, and
  the cast is noted as the call's return type so a hoisted `__ps_N` temp is
  declared at the pointer. Keyed on the thunk's spelling, so a clone that
  already returns the pointer is untouched. `float` and by-value aggregates
  never reach the shared thunk -- they get a per-spec clone -- and `bool`
  and the narrow integers convert implicitly.
- **Defect 2** (same file, the general call-argument loop's
  "cps-direct-arg-follows-emitted-signature" bridge). That bridge cast a
  local RECORDED as a pointer into a parameter emitted as the int64 word,
  but a capture read in a closure body is spelled through the env
  (`__env_X->values`) and is in no local table. When the argument is a
  capture of the closure being emitted, its C type is the env field's --
  `emit_type_c_name` of the binding under the active spec, the same
  spelling the env struct was declared with -- so the per-spec clone's
  `tur_adt_Vec__float *` capture now goes into `vec_hyget(int64_t, ...)` as
  `(int64_t)(intptr_t)(...)`. An argument an earlier rule already cast is
  left alone (otherwise the arrow fixtures' captured fn values got a second,
  redundant cast).

Pinned by `tests/fixtures/generic-closure-let-bound-carrier-bridge`: the
let-bound call at `cstr`, `int`, `float`, `bool` and a struct; a `(Vec A)`
capture passed to `vec-get` at `float`, `cstr` and `int`; and the generic
`cycle` that found the report at `float`, `cstr`, `bool` and a struct.
`tests/run.sh` fails a fixture on any `-Wint-conversion` warning, so the
fixture failed before the change (`emitted C representation warning`) and
passes after; every line equals `tur --interpret`. Checked by hand under
`gcc -Werror=int-conversion` and clang as well.
