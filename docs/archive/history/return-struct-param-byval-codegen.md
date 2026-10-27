# Returning a multi-field `:copy` struct *parameter* directly mis-emits (pointer-to-value assignment)

**Status:** RESOLVED. Fixed by dereferencing a pass-by-ptr struct parameter at
the value-consumption sites (the `RSP1` edits): the `if`-branch merge in
`src/compiler/emit_expr.c` (`emit_if_value`, both arms) and the two
function-body return paths in `src/compiler/emit_fns.c` (`emit_tail` and the
non-TCO `ret_val` path). The deref is gated on `expr_is_pbp_param` /
`ctx->pbp_param_ptrs` membership, so it fires only for a genuine pass-by-ptr
struct param consumed by value -- never where the pointer is passed on to
another pass-by-ptr callee (that path keys off the same predicate to skip the
`&temp` wrap). Regression fixture:
`tests/fixtures/return-struct-param-byval/` (covers both the direct param
return and the if-branch param returns). `bash tests/run.sh`: 1717 passed, 0
failed.

**Severity:** medium (any function that returns a by-value `:copy` struct
parameter directly fails to compile when the struct is passed `const T *`;
the C compiler rejects `T = const T *`).

**Layer:** codegen (NOT covered by #459 -- not a return-type conflict; declared
and produced types agree, the C lowering of "return a pass-by-ptr struct param
by value" is wrong).

## Repro

```turmeric
(defstruct B4 :copy [a : float b : float c : float d : float])
(defn is-z [x : B4] : bool (< (.a x) 0.0))
(defn pick [a : B4 b : B4] : B4
  (if (is-z a) b                                      ;; returning param b
    (if (is-z b) a                                    ;; returning param a
      (make-struct B4 (.a a) (.b b) (.c a) (.d b))))) ;; this branch is fine
;; tur run =>
;;   cc: error: incompatible types when assigning to type 'B4' from type 'const B4 *'
;;     B4 __t42; ... __t42 = b;   // b is `const B4 *`; assigns pointer -> value
```

Boundaries (all verified against a clean Debug build at `b6b532d`):

- A freshly `make-struct`'d return value is fine; only returning the *param*
  fails.
- A 2-field register-class struct (`:copy [a : int b : int]`, passed by value)
  is fine; the ≥3-field / float struct fails because it crosses the
  pass-by-ptr threshold and is passed as `const B4 *`.

## Root cause

The struct is passed by pointer (`type_struct_pass_by_ptr`, `types.c:3682` --
true for a non-opaque, non-heap, non-parametric struct whose `def->pass_by_ptr`
is set, i.e. it does not fit the register-class ABI). The C parameter is
therefore `const B4 * b`.

When that parameter is used in **value context** -- here as an `if`-branch
result assigned into the by-value result temp (`B4 __t42; __t42 = b;`) -- the
emitter loads the parameter via `name_for_binding`
(`src/compiler/emit_core.c:1459`), which for a function parameter returns the
raw C name `b` (`raw_name_for_binding`, `emit_core.c:887`). For a pass-by-ptr
struct param that raw name is a *pointer*, so the value-context use needs a
dereference `(*b)` and does not get one. The `make-struct` branch works because
it materializes a fresh by-value aggregate rather than loading the param.

The branch/return temp assignment that emits `%s = %s` is in the `if` /
last-expr value-assignment paths in `src/compiler/emit_expr.c` (e.g. the
`buf_printf(body, "%s = %s;\n", tmp, ...)` sites around `emit_expr.c:986-922`);
the right value string `b` is produced upstream without the deref.

## Fix direction

When emitting an `EX_VAR` that refers to a **pass-by-ptr struct parameter** in
a value context (assignment into a by-value temp, `return`, struct field source
of a `make-struct`, etc.), dereference it: emit `(*b)` instead of `b`. The
cleanest place is at the value-load site -- a small wrapper around the EX_VAR
atom path that checks `ctx->fn_params` membership *and*
`type_struct_pass_by_ptr(param_type)` and parenthesize-derefs. `atom_var` /
`name_for_binding` already special-case parameters (`emit_core.c:1462-1468`),
so the deref decision has all the information it needs there; care is needed to
deref only in value contexts, not where the param's address is what's wanted
(e.g. passing it on to another pass-by-ptr callee, which already expects the
pointer).

Add a fixture: `pick` above (returns each of two pass-by-ptr struct params from
distinct branches plus a `make-struct` branch) should `tur run` and print the
expected fields; include a 2-field register-class control that already works.

**Workaround (spice-side, today):** rebuild the struct via `make-struct` from
the param's fields instead of returning the param directly:
`(make-struct B4 (.a a) (.b a) (.c a) (.d a))`.
