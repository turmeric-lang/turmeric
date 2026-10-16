# A by-value struct with a closure field keeps its function out of the CPS backend

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
[opaque-field-struct-box-never-freed](../archive/opaque-field-struct-box-never-freed.md).
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
