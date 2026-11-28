# A by-value struct with a closure field keeps its function out of the CPS backend

**Status: RESOLVED 2026-11-27** -- see [Resolution](#resolution). The text
from "Narrowed 2026-11-05" down is the report as it stood while open.

**Narrowed 2026-11-05: the repro below compiles and runs (`100`, `101`,
`102`).** Fix direction 1, extended one step. `fn_sig_ok`'s param gate
(`fn_byval_agg_param_ok`, `src/compiler/emit_cps_ir.c`) now also admits a
by-value product whose only owning fields are closures
(`byval_fnfield_only_product`: every other field a slot scalar) when the body
only READS it -- every use is `(.f h)`, a call through a field included, or
`h` handed whole, by value, to a plain function whose parameter there is the
same type and is itself only read (`agg_param_only_field_reads` /
`agg_param_handed_to_reader`, bounded for recursion). Such a struct is
borrowed on the direct path too: the caller's `drop_fnfields_<T>` (or the CPS
entry's reap) frees its closures, the callee never does. A cps->cps call may
then pass it as an argument to such a parameter (`fnfield_param_handed_on`
in `call_args_ok`); the `__cps` entry takes it by value and nothing crosses a
one-word slot. Pinned by `tests/fixtures/cps-byvalue-struct-closure-field-param`
(the repro, a call through the handler field across the perform, and a
function that hands the struct to one that reads it; leak-checked, both
engines agree).

**Still open:** a body that stores, returns, rebinds or captures the struct
keeps evicting, as does one handing it to an inline-C, closure or generic
callee; and the closure exception in `adt_is_byvalue_product_d`
(`src/compiler/types.c`) stays, so a struct with an opaque field beside a
closure (the SAX parsers) is still on the carrier ABI and still leaks its box
once per parser. Lifting it wants turmeric-spices' `html` `sax_tokens` and
`xml` SAX suites as the regression check; they need lexbor / the xml spice's
C libraries, which the 2026-11-05 session's container did not have, so it was
not attempted blind. A struct with an opaque field would also need
`byval_fnfield_only_product` to admit the opaque word.

**Severity: medium** (compile failure: a diagnostic, or emitted C that does not
compile).

A function that takes a by-value struct with a closure field cannot be CPS
converted. When it, or anything sharing an effect with it, performs, the
compile fails:

- usually with the direct emitter's "this effect operation has no lowering
  here";
- once, in turmeric-spices, with emitted C naming an undeclared `__kont`.

The same struct with the closure field changed to an `int` compiles and runs.

Found 2026-10-16 while fixing
[opaque-field-struct-box-never-freed](opaque-field-struct-box-never-freed.md).
That fix lays a struct with an opaque field out by value. For a struct that also
has a closure field, doing so moved it into this gap, so the fix leaves those
structs on the carrier ABI for now. The SAX parsers in turmeric-spices are that
shape (`HtmlSaxParser`, `SaxParser`: an opaque handle beside a handler closure).

## Repro

```turmeric
(defeffect Log [x : int] : nil)
(defstruct H :copy [raw : int handler : (fn [int] int)])
(defn feed! [r : int s : int] : (Result int int)
  (perform (Log (+ r s)))
  (ok 0))
(defn feed-n [h : H n : int] : nil
  (let [^mut i 0]
    (while (< i n)
      (feed! (.raw h) i)
      (set! i (+ i 1)))))
(defn main [] : int
  (let [h (H 100 (fn [x : int] : int (* x 10)))]
    (handle (feed-n h 3)
      (Log [x] k) (do (println x) (resume k nil))))
  0)
```

```
repro.tur:4:3: error: this effect operation has no lowering here: the enclosing
function left the CPS backend's supported subset, ...
```

Changing `handler : (fn [int] int)` to `handler : int` and passing `7` instead
of the lambda prints `100`, `101` and `102`.

`TUR_TRACE_EVICT=1` names the root:

```
[EVICT] SIG-TAINT              eff=1 feed!
[EVICT] SIG-REJECT             eff=0 feed-n
[EVICT] SIG-TAINT              eff=1 main
```

## Root cause

`feed-n` is rejected at its signature.

- **The param gate.** `fn_sig_ok`'s param loop (`src/compiler/emit_cps_ir.c:3819`)
  admits a by-value aggregate only through `fn_byval_agg_param_ok`. That needs
  `slot_box_ty`.
- **The owning-free rule.** `slot_box_ty` (`emit_cps_ir.c:195`) admits only an
  owning-free product: `return d && !d->needs_drop_glue` at line 231.
- **Why this struct fails it.** A closure field gives the struct drop glue, for
  its env, so it is not owning-free.
- **The knock-on.** The rejected `feed-n` is permanent routing, and every
  function sharing its effect is tainted with it (`SIG-TAINT`). That includes
  `feed!`, whose `perform` then reaches the direct emitter, which cannot lower
  it.

The comment at the head of `fn_sig_ok` states the rule: "an owning-field
aggregate stays out". So this is the Tier C boundary
(`docs/archive/cps-tier-c-effect-result-native-plan.md`), not a slip.

The `__kont` variant was seen only with the opaque-field change applied without
its closure exception. There, turmeric-spices' `html/tests/sax_tokens.tur`
emitted:

```
error: '__kont' undeclared (first use in this function)
  DK *__ej13 = __dk_reap_node(dk_frame_resume(..._ej13, (intptr_t)__ce_..._ej13, __kont));
```

That is a function that was admitted, reaching a join that assumes a
continuation it does not have. It is worse than the diagnostic, but this report
has no repro for it on main.

## Fix directions

1. **Admit an owning aggregate param that is only borrowed.** A by-value
   struct that the function reads but never stores or returns could cross by
   value with no slot box. That is the common shape: a parser or options record
   handed down a loop. This needs the same "only read" walk that
   `fatparam_only_called` does for fat closures.
2. **Box an owning aggregate into the slot with its drop glue.** The slot
   box's free on the consuming load would run the glue rather than a plain
   `free`. This is wider, and it touches `slot_store` / `slot_load` and
   `cap_ty_ok`.
3. Either way, `cps-while-owning-invariant` and turmeric-spices' html
   `sax_tokens` test are the regression checks. Once a closure field no longer
   evicts, drop the closure exception in `adt_is_byvalue_product_d`
   (`src/compiler/types.c`), and the SAX parser structs stop leaking their
   carrier box.

## Workaround

Pass the closure as its own parameter instead of inside the struct. Or keep a
`defopaque` field beside the closure, which keeps the struct on the carrier ABI
for now, at the cost of one leaked box per construction.

## Resolution

**Fixed 2026-11-27**, by fix direction 2, in a form that needed no new drop
glue.

**The ownership fact.** A by-value product whose only owning fields are
closures is never dropped through a copy. The one `let` that binds the
freshly built struct frees its closures (local-struct-drop:
`drop_fnfields_<T>` on the direct path, the entry boundary's reap on the CPS
path). A parameter, a rebinding, a capture, a field read and a returned value
are borrowed words. That is what a closure passed on its own already is to
the backend: a `TY_FN` word in a slot is never retained or dropped. The
report's own workaround ("pass the closure as its own parameter") said as
much.

**The change.** `slot_box_ty` (`src/compiler/emit_cps_ir.c`) admits such a
product (`byval_fnfield_only_product`), so it goes wherever an owning-free
aggregate goes: boxed across a slot, copied into a continuation env, passed
and returned by value. "Only closures" is the complement of every other rule
that sets `needs_drop_glue` (`src/compiler/elab_structs.c`): no rc / ref /
weak field, no nested owning aggregate or recursive spine (`drop_inner_def`),
no owning `any`. Those still fall back, since their copies need a retain.
`owning_byvalue_aggregate` excludes the same set, so a closure-field struct
takes the plain copy capture and not the clone-on-read-out one.

The 2026-11-05 read-only walk (`agg_param_only_field_reads`,
`agg_param_handed_to_reader`, `fnfield_param_handed_on`) is subsumed and
removed.

**What compiles now.** Each of these was "this effect operation has no
lowering here":

- the struct rebound (`(let [g h] ...)`) in a function that performs;
- captured by a lambda;
- handed to a generic callee (`(ident h)`);
- returned from a function that performs, whether it was given the struct or
  built it;
- delivered twice by a `^multishot` resume.

**The closure exception is gone.** `adt_is_byvalue_product_d`
(`src/compiler/types.c`) admits an opaque field beside a closure, so
`SaxParser` and `HtmlSaxParser` are `tur_adt_SaxParser` by value and no
longer an `int64_t` carrier. A struct built and bound directly leaks nothing:
the report's shape with an opaque in place of the `int` went from 40 B in 2
allocations to 0 under LeakSanitizer.

**The owner's free on the CPS path.** A `let` local bound to a struct that a
call returned registers its closure fields with the boundary reap
(`emit_fnfield_reap`), at the lifted join for a cps->cps call and at the call
site for a cps->direct one. Both go through `__dk_reap_closure_once`, a new
preamble helper that skips a closure already registered: a multi-shot resume
inside the callee, after it built the struct, delivers the same closure again.
The split runtime artifacts and the 162 codegen snapshots were regenerated for
that helper (7 lines each, nothing else).

**Regression checks.**

- `tests/run.sh`: 4327 passed, 0 failed.
- `tests/run-leak-check.sh`: 294 passed, 0 failed, 8 known-open.
- turmeric-spices `xml` and `html` (`scripts/run-shard.sh`): pass, the same
  763 `ok` lines as on `main`. The 2026-11-05 session could not run these;
  lexbor and expat build from the spices' own `:cmake-deps`.
- turmeric-spices `plot`, whose ten renderer structs (`FunctionR`,
  `ContoursR`, ...) hold a `LineStyle` / `FillStyle` opaque beside a closure
  and so change layout too: pass.
- The `xml`, `html` and `plot` test directories under
  `TUR_SANITIZE_PROGRAMS=1` with leak detection off: no AddressSanitizer or
  UBSan report.

Pinned by `tests/fixtures/cps-byvalue-struct-closure-field-value`
(leak-checked: every shape above, and the opaque-beside-closure struct) and
the existing `cps-byvalue-struct-closure-field-param`.

## Found on the way

- [fnfield-struct-local-freed-while-returned](fnfield-struct-local-freed-while-returned.md)
  -- fixed in the same change. `(let [h (H n (fn ...))] h)` freed the closure
  at scope end and the caller called it afterwards. The CPS path would
  otherwise have mirrored it.
- [fnfield-struct-binder-frees-borrowed-closure](../reported/fnfield-struct-binder-frees-borrowed-closure.md)
  -- open. A `let` bound to a copy of someone else's struct, or to one built
  from a closure parameter, frees a closure it does not own.
- [cps-match-sum-payload-box-not-freed](../reported/cps-match-sum-payload-box-not-freed.md)
  -- open. A parser constructor that returns `(Result P E)`, matched inside
  a `handle` body, leaks 16 B per parser before and after this change
  (measured on a probe of the SAX shape; the spices' own suites were not
  measured for leaks): the box is the Result's payload box now, where it was
  the struct's carrier box before.
- [cps-while-then-uncarried-var-result-evicts](../reported/cps-while-then-uncarried-var-result-evicts.md)
  -- open. A performing `while` followed by a bare variable the loop does not
  set, which is what "return the struct after the loop" is.

