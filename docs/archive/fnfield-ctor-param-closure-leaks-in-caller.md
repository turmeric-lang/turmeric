# A closure handed to a struct-building parameter is never freed

**Status: RESOLVED 2026-11-28** -- see [Resolution](#resolution). The text
down to it is the report as filed.

**Severity: low-medium (leak, one closure env per closure, 24 B in the repro).**
A caller's let-bound closure leaks when the callee stores it in a by-value
struct, even when that struct never leaves the callee.

Found 2026-11-28 while fixing
[fnfield-struct-binder-frees-borrowed-closure](fnfield-struct-binder-frees-borrowed-closure.md).
Before that fix the callee's struct binder freed the closure. That was wrong:
the second call ran a freed closure. Now nobody frees it.

## Repro

```turmeric
(defdata Ev (Start int) (End))
(defstruct H [raw : int handler : (fn [Ev] int)])
(defn mk [f : (fn [Ev] int)] : int
  (let [h (H 1 f)]
    ((.handler h) (End))))
(defn main [] : int
  (let [n 3
        g (fn [e : Ev] : int n)]
    (println (mk g))
    (println (mk g)))
  0)
```

Under `tests/run-leak-check.sh`'s build (ASan, `TUR_RUNTIME=source`):

```
SUMMARY: AddressSanitizer: 24 byte(s) leaked in 1 allocation(s).
```

Without the struct (`(defn mk [f : (fn [Ev] int)] : int (f (End)))`), `main`
frees `g` at scope end, and the program is leak-clean.

`tests/fixtures/fnfield-struct-binder-borrowed-closure` carries this repro and
its relatives (an unbound receiver `(.handler (H 1 f))`, a function that returns
the wrapped parameter) with a `known-leak` marker.

## Two more shapes with no owner

Both were freed on `main` by the struct's binder, with nothing proving the
struct was the closure's only holder. They leak now.

**A let-bound closure stored in a struct.** `cap` is nobody's to free: its
own `let` sees it escape into the constructor, and the struct did not mint it.

```turmeric
(defstruct Holder [run : (fn [int] int)])
(defn main [] : int
  (let [k   3
        cap (fn [x : int] : int (+ x k))]
    (let [h (Holder cap)]
      (println ((.run h) 1))))
  0)
```

`tests/fixtures/closure-slot-ptr-void-param-sinks` has this shape (its `cap`
is also pushed into a vec and passed to four calls) and leaks 24 B more than
on `main`. When the closure's only use is that one constructor argument, not
in a loop, not captured, and with no `perform` between the two, the struct
could take it over.

**A closure a generic call hands back.** `(Holder (ident f))`: the walk does
not know whether `ident` returned its argument or something new.

## Root cause

`main` frees `g` only when `g` does not escape (`let_binding_env_freeable`,
`src/compiler/emit_expr.c`). Passing `g` to `mk` escapes unless `mk`'s parameter
is non-retaining (`Binding.nonretain_param_mask`, inferred by
`elab_infer_nonretain_masks`, `src/compiler/elab_fns.c`, from
`closure_binding_escapes`). In `mk`, `f` is an argument of the `H` constructor.
The escape walk (`src/compiler/emit_core.c`) counts that as an escape, without
looking at where the constructed value goes.

## Fix directions

1. **Let the escape walk follow a constructor into a confined local.** `f` as
   a constructor argument does not escape when the constructed struct is bound
   by a `let` whose local is only read: fields read and called, never stored,
   returned, captured or passed on (`fnfield_local_leaves_scope` already
   answers the "returned" half). The same holds for the unbound receiver
   `(.handler (H 1 f))`.
2. Then the parameter is non-retaining, and the caller's existing free covers
   it. Delete the fixture's `known-leak` marker once it runs clean.

## Resolution

**Fixed 2026-11-28**, by fix direction 1: the escape walk follows a struct
built around the closure.

**The alias.** `binding_escapes_impl_x` (`src/compiler/emit_core.c`) keeps a
small alias set (`EscAlias`) beside the closure `b` it is asked about:

- **A struct alias** is a `let` local of a by-value single-constructor struct
  whose init puts `b` into a closure field. That is either a constructor,
  `(H 1 b)`, or a call to a constructor function, `(wrap b)`, whose
  `Binding.fresh_fnfield_via_param_mask` says the result holds that closure
  parameter and the body keeps it nowhere else.
- **A closure alias** is a `let` bound to a closure field read out of a
  struct alias. `((.run h) x)` elaborates to exactly that: a `__call_head`
  temp bound to `(.run h)`, then a call of the temp.

A call through an alias is an invocation, as a call of `b` is. Handing an
alias to a parameter that does not retain it (`^borrow`,
`nonretain_param_mask`, a `^borrow & rest`) is what handing `b` there is. A
struct alias's other fields hold words `b` is not in, so reading them is
nothing. Every other mention is an escape: the struct returned, passed,
stored, rebound, matched on, captured, `set!`, or its closure field read
anywhere else. The struct's `let` is lexically inside `b`'s scope, so it
cannot outlive `b` without one of those. It never frees `b`: it is not the
owner (`Binding.fnfield_owner`), and an owner whose field holds a wrapper box
around `b` frees only that box. Only the closure analyses alias; the box and
`any` variants keep their walks.

**One walk over the `let`.** The direct emitter's `let_binding_env_freeable`
and the CPS backend's two equivalents checked the body and each sibling
binding's init separately. So `(let [cap (fn ...) h (Holder cap)] ...)` walked
`(Holder cap)` on its own, outside any `let`, where the constructor argument
is an escape. `closure_let_binding_escapes` (and `_reaped`) walk the whole
`let` node with the binding's own init skipped. For a plain `let` that is the
same set of uses.

**Repro 3 and the rest.** In `mk`, `f` is now non-retaining, so `main` frees
`g` once at its scope end. A lambda literal passed to `mk` is freed after the
call. The same-function shape (a `let`-bound closure stored in a sibling or
inner struct) is freed by its own `let`. So are a self tail loop, a loop that
builds one each turn, and a multi-shot `perform` (on the CPS path, at the
entry's exit, after both resumes).

**The "names it once" count, tightened.** The constructor-function rule (and
the owner rule from
[fnfield-struct-binder-frees-borrowed-closure](fnfield-struct-binder-frees-borrowed-closure.md))
trusts a function that names its closure parameter once. That count now
disqualifies a `defer` or nested `defn` capturing the parameter, a `set!` of
it, and any inline-C block in the body, which can name the parameter by its C
name. A direct call of the parameter is not counted: invoking it keeps
nothing.

**Measured.** Every fixture's closure and closure-field frees, emitted before
and after: no existing fixture changed except
`fnfield-struct-binder-borrowed-closure`, which gained the free of `main`'s
`g` and dropped its `known-leak` marker. The two `requires.spices` fixtures
were compared from inside the checkout: unchanged.

The same comparison over turmeric-spices (`24d2bbd`): every `.tur` under a
spice's `src/`, `tests/` and `examples/`, 792 files, of which 782 emit with
both compilers. No file's frees changed, the `xml` and `html` SAX suites and
`plot` included. The spices' suites were not run.

**Regression checks.** `tests/run.sh`: 4366 passed, 0 failed.
`tests/run-leak-check.sh`: 310 passed, 0 failed, 9 known-open.

Pinned by `tests/fixtures/closure-in-confined-struct-freed` (leak-checked,
both engines: every shape above, plus a constructor function that also keeps
its parameter in a global, whose closure is called after the function that
built it returned), `closure-in-confined-struct-multishot` (leak-checked,
compiled only: the interpreter cannot take a multi-shot continuation past the
`let`-bound lambda), and `closure-in-escaping-struct-kept` (known-leak: each
closure is used after its `let` ended, so a free there would be a
use-after-free).

## Still open

A closure stored where it outlives its `let` still has no owner and leaks:
[closure-stored-beyond-its-let-has-no-owner](../reported/closure-stored-beyond-its-let-has-no-owner.md).
That includes this report's two extra shapes: `closure-slot-ptr-void-param-sinks`,
whose closure is also pushed into a vec, and a closure a generic call hands
back.
