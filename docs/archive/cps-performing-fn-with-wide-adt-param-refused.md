# A function that performs and takes a wide by-value aggregate parameter is refused

**RESOLVED 2026-10-13 -- fix direction 1, plus one gate it exposed.** The
repro prints `11` compiled, as the interpreter does.

- **The admission.** `fn_byval_agg_param_ok` (`src/compiler/emit_cps_ir.c`)
  no longer excludes `type_struct_pass_by_ptr`. The pieces the report listed
  already agreed: the `__cps` entry takes the aggregate by value
  (`emit_params`); the plain-name wrapper keeps the forward decl's
  `const T *` and passes `*s` into `__cps` (`cps_entry_param_by_ptr`, since
  2026-07-22); a cps->cps caller passes the atom raw; a cps->direct caller of
  the wrapper passes `&(atom)`. Fix direction 2 (a `const T *` `__cps` ABI)
  was not needed.
- **The gate it exposed.** With only that change, `area2` passed as a fn
  value -- `(apply-s area2 (Rect 2 4))`, where `apply-s` calls `(f s)` -- built
  and then aborted at run time with `tur: unhandled effect (tag 2)`. Before,
  it was refused at compile time. `apply-s` threads `f` through the E2a
  registry dispatch (`via_registry`), but the tail-call arg gate classed that
  call cps->direct, because `f` has no SEnt. For a wide aggregate it then went
  looking for a direct `const T *` signature, found no FnDef for a parameter,
  and evicted `apply-s` (`BODY-STRUCT-CORE`, `eff=0`). A narrow aggregate got
  through on the RAW answer. The E2a call always lands in a `__cps` entry,
  which takes the aggregate by value: an aggregate argument has no direct
  fallback there (`e2a_slot_kind_ok`). So `cps_direct_agg_arg_mode` takes a
  `to_cps_entry` flag that answers RAW for any width once the types match, and
  both CT_TAILCALL gates (`term_core_ok_impl`, `handle_delim_ok`) go through
  one `tailcall_args_ok` that sets it from `via_registry`.
- **Builds.** Checked whole-program (`tur run`), a multi-module project
  (`tur build <dir>`, the performing function in an imported module), and
  `tur build --shared` with the function internal to the exported module. All
  three were refused before and are correct now. The `--shared` build of a
  multi-module project was broken independently of this, and is fixed too:
  [shared-build-imported-ctor-undeclared](shared-build-imported-ctor-undeclared.md).
  A generic that performs is refused at every width, `int` included -- also
  filed separately:
  [generic-fn-performing-caller-handled-effect-refused](../reported/generic-fn-performing-caller-handled-effect-refused.md).
- Pinned by `tests/fixtures/cps-wide-adt-param-perform` (leak-checked): the
  repro at both variants, a record ADT, a wide defstruct between scalars, a
  float product, two performs, a cps->cps non-tail call, the fn-value call
  through `apply-s` (tail) and `apply-s2` (under `+`, the E2a heap join), and
  a lambda with a wide parameter. The same answers under `tur --interpret` and
  `tur jit`. Before the fix it does not build: seven `perform` sites report
  "no lowering here".

**Left open, no repro on `main`.** The intermediate build above shows that an
E2-threading callee evicted for a STRUCTURAL reason (`BODY-STRUCT-CORE`,
`eff=0`) taints nothing, so a fn value colored on the promise that its callee
threads the continuation is called through its plain-name wrapper, with a
fresh root prompt, and the effect goes unhandled at run time instead of being
refused at compile time. A SIG-REJECT callee does taint (`apply-b` taking a
wide `Big` before this fix: `bump` went `SIG-TAINT`, a compile error). No
other structural eviction of such a callee was found to reproduce it. If a
`tur: unhandled effect` abort turns up under a handler, check
`TUR_TRACE_EVICT=1` for an `eff=0` `BODY-STRUCT-*` line on the function the fn
value was passed to.

---

**Severity: low-medium.** A correct program is refused at compile time, never
miscompiled. But nothing in the program points at the cause: the error lands
on the `perform` ("this effect operation has no lowering here"), and every
function sharing the effect is evicted with it (`SIG-TAINT` / `BODY-TAINT`).
Found 2026-10-13 while fixing
[adt-handle-result-as-call-arg-evicts](../archive/adt-handle-result-as-call-arg-evicts.md).

## Repro

```turmeric
(defdata Shape (Circle :int) (Rect :int :int))
(defeffect Ask [] int)
(defn area2 [s : Shape] : int
  (+ (perform (Ask)) (match s (Circle r) (* 3 r) (Rect w h) (* w h))))
(defn s5 [] : int (handle (area2 (Rect 2 4)) (Ask [] k) (resume k 3)))
(defn main [] : nil (println (s5)))
```

| | result |
| --- | --- |
| `tur --interpret` | `11` |
| `tur run` | "this effect operation has no lowering here" at the `perform` |
| `Shape` narrowed to fit 16 bytes (`(Rect :int)`) | compiles |

`TUR_TRACE_EVICT=1`: `SIG-REJECT eff=1 area2`, then `SIG-TAINT eff=1 s5`.

## Root cause

`fn_byval_agg_param_ok` (`src/compiler/emit_cps_ir.c`) admits a by-value
aggregate parameter only when the direct emitter ALSO passes it by value,
`!type_struct_pass_by_ptr(p->type)`. Its comment gives the reason: `emit_params`
spells the `__cps` parameter by value, so a by-pointer one made the entry
wrapper disagree with the direct forward decl (`const T *`) -- a `conflicting
types` error.

That reason may no longer hold. The entry wrapper was taught the by-pointer
spelling on 2026-07-22
([sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch](../archive/sepcomp-cps-entry-wrapper-byptr-agg-param-mismatch.md)):
`cps_entry_param_by_ptr` / `cps_entry_param_byptr_ctype` declare the wrapper's
parameter `const T *`, matching the forward decl, and dereference it into the
by-value `__cps` call. The plan that added the exclusion
(`docs/archive/cps-dk-sole-effect-lowering-plan.md`, E1 by-value param slice)
predates that fix.

## Fix directions

1. Lift the `type_struct_pass_by_ptr` exclusion in `fn_byval_agg_param_ok` and
   see what breaks. The pieces that have to agree:
   - the forward decl and the entry wrapper (`const T *`; done since 07-22);
   - the `__cps` body (by value);
   - a cps->cps caller (passes the atom by value; already what
     `atoms_csv_call_typed` does with `to_direct == false`);
   - a cps->direct caller of the wrapper (passes `&(atom)`; done by the fix
     that found this report).

   Check both whole-program and `--shared` builds; the 07-22 bug was a
   separate-compilation one.
2. If that is not enough, the parameter could cross the `__cps` boundary as
   `const T *` too. That is a larger ABI change.
