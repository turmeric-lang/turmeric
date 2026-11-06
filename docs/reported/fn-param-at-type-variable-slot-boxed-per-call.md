# A fn-typed parameter passed into a type-variable slot mallocs a box per call that nothing frees

**Severity: low (32 B per call, at a narrow shape).**

**Narrowed 2026-11-06: a box whose call is invoked on the spot, by a callee
that only hands the argument back, lives on the stack.** `((ident d) 10)`,
`((pick d d) "there")` and `((unwrap-or (none) d) 5)` no longer leak. What is
left is a boxed value that goes anywhere else: let-bound, wrapped in a sum
(`(match (some d) ...)`), or pushed into a container. See "Fixed 2026-11-06"
at the end.

A `(fn ...)`-typed parameter with a scalar signature is the 16-byte poly
carrier (`tur_poly_fn_t {env, fn}`, typed `ptr<void>`), while a generic's
type-variable slot holds one word. Until 2026-10-12 nothing converted it, and
each of these failed on `main`:

```turmeric
(defn ident [A] [a : A] : A a)
(defn h [d : (fn [int] int)] : int ((ident d) 10))      ; A bound to ptr<void>: not callable
(defn u [d : (fn [int] int)] : int ((unwrap-or (none) d) 5))  ; cc: aggregate used as integer
(defn k [d : (fn [int] int) v : (Vec (fn [int] int))] : int
  (vec-push! v d) 1)                                    ; TUR-E0001: expected (fn ...), got ptr<void>
```

**Fixed 2026-10-12** (the compile and type failures): at a type-variable slot
the parameter is boxed into a fat handle, as a `^fat` sink boxes it
(`EX_POLY_TO_FAT`, `src/compiler/elab_call.c`, just before the argument kind
check). The node is typed with the parameter's declared signature, so `A`
binds to the function type, and float and cstr signatures cross through their
typed slot-0 shims. Both back ends agree. Pinned by
`tests/fixtures/fn-param-through-type-variable-slot` (int, float with a
capturing closure, cstr, 2-ary, `unwrap-or`, `some`, a `(Vec (fn [float]
float))`).

## What is left

The box is the malloc'd form, 32 B a call, and nothing frees it. The other
two forms do not fit:

- The stack box (`stack_ok`) needs a callee proven not to retain the value,
  and a type-variable slot is usually there to return or store it (`ident`,
  `unwrap-or`, `vec-push!`).
- The static box (`static_ok`) needs a link-time-constant function. A
  parameter is not one.

Measured on `fn-param-through-type-variable-slot` under ASan: one 32 B box
per conversion, 9 of them (288 B). The rest of its 400 B report is the
fixture's own: a `(Vec ...)` it never frees.

## Fix directions

- Ask the result instead of the callee, the way
  `sum-closure-payload-never-dropped` item 2 does for `unwrap`. If the call is
  `(ident d)` and its value is only invoked on the spot, the box is dead when
  the expression ends and may live on the stack.
- Or unwrap at the source: when the poly carrier was itself built from a fat
  handle (`{handle, fat trampoline}`), hand that handle over instead of
  boxing the carrier. That needs the trampoline's identity at runtime, a
  one-compare test.

## Fixed 2026-11-06

Fix direction 1, the on-the-spot half. `((g ... d ...) args)` elaborates to
`(let [tmp (g ... d ...)] (tmp args))`; at that point
(`mark_invoked_passthrough_boxes_stack`, `src/compiler/elab_call.c`) each
`EX_POLY_TO_FAT` argument of `g` is given the stack box (`stack_ok`) when
`g`'s body uses that parameter only as its value: a bare use in tail position,
through `if` branches, `match` arms, `let` / `do` bodies and ascriptions, and
nowhere else (`param_used_only_as_result`). No capture, no inline C, no use as
an argument or a `let` init. Then the value of the call is the argument or
something else, the callee kept nothing of it, and the box is dead when `tmp`
is. A dictionary-dispatched `g` is not asked. The stack box's header is the
no-op keep glue, so the temp's own drop at scope exit stays harmless.

On `fn-param-through-type-variable-slot`, LeakSanitizer measured 400 B before
and 168 B after. Of the 168, 32 B is the `(some d)` box. The rest is the
fixture's own: its `Vec`, and the envs of the two capturing lambdas `main`
passes in. Those used to be reachable through the leaked boxes, so they read
as indirect leaks; now they read as direct ones.

Pinned by `tests/fixtures/fn-param-tyvar-slot-invoked-stack-box`, which is
leak-checked: `ident`, `pick`, a `first-if` that picks between two parameters
under an `if`, 2-ary, `unwrap-or`, float and cstr signatures, and 100 turns
of each. `fn-param-through-type-variable-slot` gained the must-keep half:
`keep` both stores and returns its argument, the stored handle is called
after the boxing function has returned and the stack has been reused, and its
box stays malloc'd.

Still open: the let-bound, sum-wrapped and stored shapes above. The stored
one is a real owner question. The other two want the same result-side
question asked one step further, as
[sum-closure-payload-never-dropped](sum-closure-payload-never-dropped.md)
item 2 asks it of `unwrap`.
