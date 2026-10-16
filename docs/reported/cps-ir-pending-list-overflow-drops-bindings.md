# More than 32 computed operands in one CPS expression drops bindings

**Severity:** low. The build fails in cc on an undeclared temporary, and only
for an expression with more than 32 non-atomic operands inside a function the
CPS backend lowers.

## Repro

A colored function called with 34 computed arguments from a colored caller:

```python
# gen.py -- writes pend.tur
n = 34
params = " ".join(f"a{i} : int" for i in range(n))
print("(defeffect Ask [] : int)")
print(f"(defn sum-ask [{params}] : int\n  (+ (perform (Ask)) (+ a0 a33)))")
args = " ".join(f"(+ x {i})" for i in range(n))
print(f"(defn run [x : int] : int\n  (handle (sum-ask {args})\n    (Ask [] k) (resume k 1000)))")
print("(defn main [] : int\n  (println (run 1))\n  0)")
```

`tur run pend.tur` (expected `1035`):

```
error: '__t33' undeclared (first use in this function); did you mean '__t32'?
    return sum_hyask__cps(__t1, ..., __t32, __t33, __t34, __h0); /* cps->cps */
```

## Root cause

`src/passes/cps_ir.c:atomize` names a non-atomic operand with a fresh `__tN`
and records the binding in a fixed `Pending` array:

```c
typedef struct { PendItem items[32]; uint32_t n; } Pending;
...
if (p->n < 32) { p->items[p->n].expr = e; p->items[p->n].x = x; p->n++; }
return atom_cvar(x);
```

Past 32 it still returns the atom but drops the binding, so `fold_pending`
never binds it. `call_args_pendable` guards the registry-threaded fn-value
calls against exactly this (its comment says "atomize silently drops past
it"). The ordinary colored-call path and other atomizing forms have no such
guard.

## Fix directions

- Make `Pending` growable (arena-allocated), or
- make `atomize` record overflow, and have the callers turn an overflowed
  `Pending` into `CT_UNSUPPORTED` so the function is evicted with a reason
  instead of emitting an unbound name.
